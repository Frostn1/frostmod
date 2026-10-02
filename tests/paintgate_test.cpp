// When FrostMod may rebuild the paint lists (src/paintgate.h): never while riding, at most
// once per join, and a burst of asks is one refresh.
//
// Pure, so CI runs it.

#include "../src/paintgate.h"

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

using namespace frostmod::paintgate;

// Drive a gate through `frames` frames of `phase`, 16 ms apart, starting at `t`. Returns
// how many refreshes it started; a started refresh is "busy" for `busyFrames` frames.
static int Run(Gate& g, Phase phase, uint64_t& t, int frames, int busyFrames = 6) {
    int started = 0, busy = 0;
    for (int i = 0; i < frames; ++i, t += 16) {
        if (g.Tick(phase, busy > 0, t) != kNone) { ++started; busy = busyFrames; }
        else if (busy > 0) --busy;
    }
    return started;
}

static void PhasesFromPages() {
    const char* menu[] = {"main", "multi"};
    const char* joining[] = {"multi", "connection_dialog"};
    const char* pits[] = {"chat", "pit", "multi_pit"};
    const char* riding[] = {"mtrackm"};
    const char* ridingWithSettings[] = {"mtrack", "settings"};
    CHECK(PhaseFromPages(menu, 2, false) == Phase::Menu, "menu");
    CHECK(PhaseFromPages(joining, 2, false) == Phase::Joining, "connection dialog = joining");
    CHECK(PhaseFromPages(pits, 3, false) == Phase::Pits, "pits");
    CHECK(PhaseFromPages(riding, 1, false) == Phase::Riding, "track page = riding");
    CHECK(PhaseFromPages(ridingWithSettings, 2, false) == Phase::Riding, "settings over the track");
    // The game's own riding flag wins over whatever the pages say.
    CHECK(PhaseFromPages(pits, 3, true) == Phase::Riding, "vehicle riding flag wins");
    CHECK(PhaseFromPages(nullptr, -1, false) == Phase::Unknown, "unreadable stack");
}

static void NeverWhileRiding() {
    Gate g;
    uint64_t t = 1000;
    g.Request(kStaged, t);
    CHECK(Run(g, Phase::Riding, t, 5000) == 0, "a staged paint never refreshes while riding");
    g.Request(kLook, t);
    CHECK(Run(g, Phase::Riding, t, 5000) == 0, "nor does the player's own look change");
    CHECK(Run(g, Phase::Unknown, t, 5000) == 0, "an unreadable page stack counts as riding");
    CHECK(g.Pending() == (kStaged | kLook), "both still queued");
    // Back to the pits: one refresh covers both.
    CHECK(Run(g, Phase::Pits, t, 200) == 1, "applied once back in the pits");
    CHECK(g.Pending() == kNone, "nothing left");
    CHECK(!FullReloadMayStart(Phase::Riding) && !FullReloadMayStart(Phase::Unknown),
          "full reload waits while riding");
    CHECK(FullReloadMayStart(Phase::Pits) && FullReloadMayStart(Phase::Menu) &&
              FullReloadMayStart(Phase::Joining),
          "full reload runs off track");
}

// The case the old flow got wrong: the app asked up to three times per sync (the room's
// arrival, its full reload, the folder watcher), each one a refresh.
static void OneJoinIsAtMostOneRefresh() {
    Gate g;
    uint64_t t = 1000;
    CHECK(Run(g, Phase::Menu, t, 50) == 0, "nothing asked, nothing done");
    // The join starts. The app stages paints during the load and asks three times.
    int started = Run(g, Phase::Joining, t, 20);
    g.Request(kStaged, t); started += Run(g, Phase::Joining, t, 5);
    g.Request(kStaged, t); started += Run(g, Phase::Joining, t, 5);
    g.Request(kLook, t);   started += Run(g, Phase::Joining, t, 300);
    CHECK(started == 1, "three asks during one join -> one refresh (got %d)", started);
    CHECK(g.RefreshesThisJoin() == 1, "counted against this join");

    // More asks later in the same join (paints staged after the refresh began) wait.
    g.Request(kStaged, t);
    started = Run(g, Phase::Joining, t, 600);
    CHECK(started == 0, "a second refresh in the same join is refused (got %d)", started);
    CHECK(g.Pending() == kStaged, "it waits for the next load point");
    // Into the pits: that is the next load point.
    CHECK(Run(g, Phase::Pits, t, 200) == 1, "applied in the pits after the join");

    // The dialog's page flickering away and back inside one join is still that join.
    g.Request(kStaged, t);
    Run(g, Phase::Riding, t, 100);            // (queued while riding)
    started = Run(g, Phase::Joining, t, 100); // a new join: may refresh once
    CHECK(started == 1, "new join refreshes once");
    g.Request(kStaged, t);
    started = Run(g, Phase::Menu, t, 10);     // a 160 ms flicker, inside the settle
    started += Run(g, Phase::Joining, t, 300);
    CHECK(started == 0, "a flicker is not a new join (got %d)", started);
    Run(g, Phase::Pits, t, 200);

    // A new join resets the count.
    g.Request(kStaged, t);
    Run(g, Phase::Riding, t, 100);
    CHECK(Run(g, Phase::Joining, t, 200) == 1, "the next join may refresh once again");
}

// A rider who joins after you: their paint is queued while you ride and applied when you
// are back in the pits, never on the out-lap.
static void LateJoinerWaitsForThePits() {
    Gate g;
    uint64_t t = 1000;
    Run(g, Phase::Pits, t, 50);
    Run(g, Phase::Riding, t, 50);
    g.Request(kStaged, t);                    // they joined; you are on track
    CHECK(Run(g, Phase::Riding, t, 10000) == 0, "queued while riding");
    CHECK(Run(g, Phase::Pits, t, 200) == 1, "applied on return to the pits");
    CHECK(Run(g, Phase::Pits, t, 200) == 0, "and only once");
}

// A burst of asks must not start a refresh per ask: it waits for the asks to stop.
static void BurstsSettle() {
    Gate g(750);
    uint64_t t = 1000;
    int started = 0;
    for (int i = 0; i < 10; ++i) {          // ten asks, 200 ms apart
        g.Request(kStaged, t);
        started += Run(g, Phase::Menu, t, 12);
    }
    CHECK(started == 0, "still settling while asks keep coming");
    started += Run(g, Phase::Menu, t, 100);
    CHECK(started == 1, "one refresh for the burst (got %d)", started);
}

// Asks that arrive while a refresh is already running are one more refresh, not many.
static void AsksDuringARefreshCoalesce() {
    Gate g(0);
    uint64_t t = 1000;
    g.Request(kStaged, t);
    CHECK(g.Tick(Phase::Pits, false, t) == kStaged, "starts");
    for (int i = 0; i < 5; ++i) { g.Request(kStaged, t); CHECK(g.Tick(Phase::Pits, true, t) == kNone, "busy"); }
    CHECK(g.Tick(Phase::Pits, false, t) == kStaged, "one follow-up");
    CHECK(g.Tick(Phase::Pits, false, t) == kNone, "and no more");
    CHECK(g.Started() == 2, "two refreshes total");
}

int main() {
    PhasesFromPages();
    NeverWhileRiding();
    OneJoinIsAtMostOneRefresh();
    LateJoinerWaitsForThePits();
    BurstsSettle();
    AsksDuringARefreshCoalesce();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("paintgate: all checks passed\n");
    return 0;
}
