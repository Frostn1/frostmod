// Which vehicle the rejoin fix may free (src/rejoinguard.h).
//
// Freeing the wrong record takes a live rider out of the game - on 2026-10-04 it was the local
// rider's own bike, and the Garage page crashed on it 16 s later. Every "no" here matters as
// much as the one "yes". Pure, so CI runs it.

#include "../src/rejoinguard.h"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n  (%s)\n", #cond);                                   \
        }                                                                       \
    } while (0)

using namespace rejoin;

static VehicleView Live(int key, int conn = 0, bool req = false) {
    VehicleView v; v.live = true; v.key = key; v.hasRequest = req; v.requestConn = conn; return v;
}

// Run the tracker the way RjTick does for one pending departure; returns the first non-Wait
// decision (or Wait if it is still waiting at `until`).
static Decision RunUntil(Tracker& t, int ownVeh, int ownKey, const VehicleView* views,
                         uint64_t from, uint64_t until) {
    for (uint64_t now = from; now <= until; now += 16) {
        for (int i = 0; i < kVehMax; ++i) t.Observe(i, views[i], now);
        if (t.PendingCount() == 0) return {Verdict::Drop, Why::Ok};
        const Pending p = t.PendingAt(0);
        const Decision d = Decide(p, ownVeh, ownKey, views[p.veh], t.RemovalSeen(p.key, p.since), now);
        if (d.verdict != Verdict::Wait) { t.RemovePendingAt(0); return d; }
    }
    return {Verdict::Wait, Why::WaitingForRemoval};
}

int main() {
    // ---- Decide, case by case -------------------------------------------------------------
    const Pending p{31, 13, 4, 1000};
    const int own = 0, ownKey = 41;
    {
        auto d = Decide(p, own, ownKey, Live(4), true, 1100);
        CHECK(d.verdict == Verdict::TearDown, "the one yes: removal seen, released, not ours");
    }
    CHECK(Decide({31, -1, 4, 0}, own, ownKey, Live(4), true, 10).why == Why::BadIndex, "negative index");
    CHECK(Decide({31, 50, 4, 0}, own, ownKey, Live(4), true, 10).why == Why::BadIndex, "index == max");
    CHECK(Decide(p, -1, 0, Live(4), true, 1100).why == Why::OwnUnknown, "own vehicle unknown: refuse");
    CHECK(Decide(p, 50, 0, Live(4), true, 1100).why == Why::OwnUnknown, "own vehicle garbage: refuse");
    CHECK(Decide({26, 0, 41, 0}, 0, 41, Live(41), true, 10).why == Why::LocalVehicle,
          "never the local rider's own vehicle, even with a removal");
    CHECK(Decide({26, 5, 41, 0}, 0, 41, Live(41), true, 10).why == Why::LocalRaceNumber,
          "never a record carrying our race number");
    CHECK(Decide(p, own, ownKey, VehicleView{}, true, 1100).why == Why::AlreadyFreed, "already freed");
    CHECK(Decide(p, own, ownKey, Live(77), true, 1100).why == Why::Reused, "reused by race 77");
    CHECK(Decide(p, own, ownKey, Live(4, 99, true), true, 1100).why == Why::OtherOwner,
          "a request from another connection owns it");
    CHECK(Decide(p, own, ownKey, Live(4, 31, true), true, 1100).verdict == Verdict::Wait,
          "own request still held: wait");
    CHECK(Decide(p, own, ownKey, Live(4, 31, true), true, 1000 + kGiveUpMs + 1).why == Why::StillInUse,
          "held past the give-up: leave it");
    CHECK(Decide(p, own, ownKey, Live(4), false, 1100).verdict == Verdict::Wait,
          "no removal yet: wait");
    CHECK(Decide(p, own, ownKey, Live(4), false, 1000 + kRemoveWindowMs + 1).why == Why::NoRaceEntryRemoval,
          "no removal in the window: never act");

    // ---- mapping ----------------------------------------------------------------------------
    {
        Tracker t; int veh, key;
        CHECK(t.OnDisconnect(26, 100, &veh, &key) == Lookup::NoMapping && veh == -1,
              "nobody mapped: a spectator or a rider still joining");
        t.Observe(3, Live(12, 7, true), 100);
        CHECK(t.OnDisconnect(7, 150, &veh, &key) == Lookup::Mapped && veh == 3 && key == 12, "fresh mapping");
        Tracker u;
        u.Observe(3, Live(12, 7, true), 100);
        u.Observe(3, Live(12), 100 + kMapFreshMs + 1);         // request gone long ago
        CHECK(u.OnDisconnect(7, 100 + kMapFreshMs + 2) != Lookup::Mapped, "stale mapping is cleared");
        CHECK(!u.MapOf(3).valid, "Observe dropped it");
        Tracker w;
        w.Observe(3, Live(12, 7, true), 100);
        w.Observe(3, Live(13), 120);                            // record now someone else's
        CHECK(!w.MapOf(3).valid, "a key change clears the mapping at once");
        w.Observe(4, VehicleView{}, 130);
        CHECK(!w.MapOf(4).valid, "a dead record has no mapping");
        Tracker a;
        a.Observe(3, Live(12, 7, true), 100);
        a.Observe(9, Live(14, 7, true), 100);
        CHECK(a.OnDisconnect(7, 110) == Lookup::Ambiguous && a.PendingCount() == 0,
              "two vehicles for one connection: refuse");
        Tracker r;
        r.Observe(3, Live(12, 7, true), 100);
        r.OnDisconnect(7, 110);
        r.OnRaceRemove(12, 120);
        CHECK(r.PendingCount() == 1, "pending");
        CHECK(r.OnRaceAdd(12) == 1 && r.PendingCount() == 0, "a rejoin cancels the pending free");
        CHECK(!r.RemovalSeen(12, 110), "and forgets the removal");
        CHECK(!r.MapOf(3).valid, "and the mapping");
        Tracker s;
        s.Observe(3, Live(12, 7, true), 100);
        s.OnDisconnect(7, 110);
        s.Reset();
        CHECK(s.PendingCount() == 0 && !s.MapOf(3).valid, "session/event change drops everything");
        Tracker o;
        for (int k = 0; k < kRemovedMax + 3; ++k) o.OnRaceRemove(100 + k, 50);
        CHECK(o.RemovalSeen(100 + kRemovedMax + 2, 50), "newest removal kept");
        CHECK(!o.RemovalSeen(100, 50), "oldest removal dropped");
        CHECK(!o.RemovalSeen(100 + kRemovedMax + 2, 50 + kRemoveWindowMs + 1), "removal outside the window");
    }

    // ---- regression: 2026-10-04 -------------------------------------------------------------
    // Local rider on vehicle 0, race 41. Rider #4 (conn 31) on vehicle 13.
    {
        VehicleView views[kVehMax];
        views[0]  = Live(41);              // our bike: no load request by now
        views[13] = Live(4, 31, true);
        Tracker t;
        uint64_t now = 0;
        // Earlier: a load request from connection 26 named vehicle 0 (a duplicate plate 41
        // joining, or a request long since gone). The old code kept {26 -> 0, 41} for ever.
        t.Observe(0, Live(41, 26, true), now);
        for (int i = 1; i < kVehMax; ++i) t.Observe(i, views[i], now);

        // 13:55:37.722 - a real departure: Remove Entry for race 4, then rider 31 disconnects.
        now = 200000;
        for (int i = 0; i < kVehMax; ++i) t.Observe(i, views[i], now);
        t.OnRaceRemove(4, now);
        int veh = -1, key = 0;
        CHECK(t.OnDisconnect(31, now + 5, &veh, &key) == Lookup::Mapped && veh == 13 && key == 4,
              "13:55:37 rider 31 -> vehicle 13 (race 4)");
        views[13].hasRequest = false;      // the game's disconnect handling releases it
        Decision d = RunUntil(t, 0, 41, views, now + 16, now + 5000);
        CHECK(d.verdict == Verdict::TearDown, "13:55:37 the real departure is still freed");
        views[13] = VehicleView{};

        // 13:58:57 - DISCONNECTION_INFO for connection 26. No Remove Entry. Vehicle 0 is ours.
        now = 400000;
        for (int i = 0; i < kVehMax; ++i) t.Observe(i, views[i], now);
        const Lookup l = t.OnDisconnect(26, now, &veh, &key);
        CHECK(l != Lookup::Mapped && t.PendingCount() == 0,
              "13:58:57 connection 26 maps to nothing (stale mapping gone): %s", LookupText(l));

        // Even had the mapping been fresh (case a: conn 26 joining with plate 41 right now):
        Tracker f;
        f.Observe(0, Live(41, 26, true), now);
        CHECK(f.OnDisconnect(26, now + 1, &veh, &key) == Lookup::Mapped && veh == 0, "fresh dup-plate map");
        VehicleView v0 = Live(41);
        Decision g = Decide(f.PendingAt(0), 0, 41, v0, false, now + 100);
        CHECK(g.verdict == Verdict::Drop && g.why == Why::LocalVehicle,
              "13:58:57 refused: the local rider's own vehicle (%s)", WhyText(g.why));
        // And with no own-vehicle guard at all, the missing removal alone stops it.
        Decision h = Decide(f.PendingAt(0), 7, 0, v0, false, f.PendingAt(0).since + kRemoveWindowMs + 1);
        CHECK(h.verdict == Verdict::Drop && h.why == Why::NoRaceEntryRemoval,
              "13:58:57 refused: no race-entry removal for race 41 (%s)", WhyText(h.why));
    }

    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("rejoinguard: all checks passed\n");
    return 0;
}
