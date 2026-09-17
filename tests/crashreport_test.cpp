// What a crash report says, asserted without a crash.
//
// The report is the only output of FrostMod that is read exactly once, by someone who
// cannot ask for a second one: the game is gone, the player has moved on, and whatever
// the log holds is the whole of the evidence. So the parts that decide what it holds -
// the ring that keeps the last N things that happened, and the layout that turns the ring
// and the session context into lines - are pure, and this runs them.
//
// Pure constants and <cstdio>, like tests/offsets_test.cpp: no Win32, no MSVC, no game, so
// CI and a Mac both execute it.

#include "../src/crashreport.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace frostmod::crash;

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

// ---- a sink that keeps the lines, so a test can read the report back --------
static void Collect(void* user, const char* line) {
    ((std::vector<std::string>*)user)->push_back(line);
}

static bool Mentions(const std::vector<std::string>& lines, const char* needle) {
    for (const auto& l : lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

static void TrailKeepsTheNewest() {
    Trail t;
    CHECK(t.Count() == 0, "a fresh trail has nothing on it");
    CHECK(t.TotalAdded() == 0, "and has counted nothing");

    // Under capacity: everything is there, oldest first.
    for (int i = 0; i < 5; ++i) {
        char s[32];
        std::snprintf(s, sizeof(s), "note %d", i);
        t.Add((unsigned long long)(i * 100), s);
    }
    CHECK(t.Count() == 5, "five notes, five readable - got %d", t.Count());
    CHECK(std::strcmp(t.At(0).text, "note 0") == 0, "At(0) is the oldest, got '%s'", t.At(0).text);
    CHECK(std::strcmp(t.At(4).text, "note 4") == 0, "At(n-1) is the newest, got '%s'", t.At(4).text);
    CHECK(t.At(2).ms == 200, "the timestamp rides along, got %llu", t.At(2).ms);

    // Past capacity: the oldest fall off, the order still reads forwards. This is the
    // case that matters - a crash comes after hours of play, never after five events.
    Trail full;
    const int total = kMaxCrumbs + 8;
    for (int i = 0; i < total; ++i) {
        char s[32];
        std::snprintf(s, sizeof(s), "note %d", i);
        full.Add((unsigned long long)i, s);
    }
    CHECK(full.Count() == kMaxCrumbs, "the ring holds kMaxCrumbs, got %d", full.Count());
    CHECK(full.TotalAdded() == (unsigned)total,
          "but remembers how many went through it, got %u", full.TotalAdded());
    char expectOldest[32], expectNewest[32];
    std::snprintf(expectOldest, sizeof(expectOldest), "note %d", total - kMaxCrumbs);
    std::snprintf(expectNewest, sizeof(expectNewest), "note %d", total - 1);
    CHECK(std::strcmp(full.At(0).text, expectOldest) == 0,
          "the oldest survivor is '%s', got '%s'", expectOldest, full.At(0).text);
    CHECK(std::strcmp(full.At(kMaxCrumbs - 1).text, expectNewest) == 0,
          "the newest is '%s', got '%s'", expectNewest, full.At(kMaxCrumbs - 1).text);

    // Out of range is empty, not a fault: the only caller is already inside a crash.
    CHECK(full.At(-1).text[0] == 0, "At(-1) is empty");
    CHECK(full.At(kMaxCrumbs).text[0] == 0, "At(past the end) is empty");

    // Longer than a crumb: truncated, never overrun.
    Trail cut;
    std::string huge(kCrumbLen * 2, 'x');
    cut.Add(0, huge.c_str());
    CHECK((int)std::strlen(cut.At(0).text) == kCrumbLen - 1,
          "a long note is truncated to the slot, got %d", (int)std::strlen(cut.At(0).text));
}

static void ReportSaysWhereTheyWere() {
    Context ctx;
    ctx.where.store((int)Where::OnTrack);
    ctx.riders.store(17);
    ctx.lastDrawMs.store(9'000);
    ctx.reloads.store(2);
    ctx.lastReloadMs.store(4'000);
    ctx.SetTrack("Northgate Raceway");
    ctx.SetServer("MXB App Public");
    ctx.SetRider("Frost");

    Trail t;
    t.Add(1'000, "event opened: track='Northgate Raceway'");
    t.Add(8'500, "rider #14 joined (17 in the session)");

    std::vector<std::string> out;
    WriteContext(ctx, t, /*nowMs=*/10'000, &Collect, &out);

    CHECK(Mentions(out, "on track"), "the report says where they were");
    CHECK(Mentions(out, "riders=17"), "and how many were in there with them");
    CHECK(Mentions(out, "Northgate Raceway"), "and which track");
    CHECK(Mentions(out, "MXB App Public"), "and which server");
    CHECK(Mentions(out, "last frame drawn 1000ms ago"), "and how long since a frame");
    CHECK(Mentions(out, "2 content reload(s)"), "and that reloads had run");
    CHECK(Mentions(out, "the last 6000ms ago"), "and how long before the crash");
    CHECK(Mentions(out, "rider #14 joined"), "the trail is in the report");
    CHECK(Mentions(out, "-1500ms  rider #14 joined (17 in the session)"),
          "each note carries how long before the crash it happened");

    // Oldest first: a report is read top to bottom as a sequence of events.
    int iEvent = -1, iJoin = -1;
    for (int i = 0; i < (int)out.size(); ++i) {
        if (out[i].find("event opened") != std::string::npos) iEvent = i;
        if (out[i].find("rider #14 joined") != std::string::npos) iJoin = i;
    }
    CHECK(iEvent >= 0 && iJoin > iEvent, "the trail reads in the order things happened");
}

// The injected copy is never called back with Draw's state, so on its own it would file a
// rider mid-lap as "in the menus". Given the session it mirrors from the plugin copy, it
// says what it does know and admits the rest.
static void ASessionWithoutFramesIsNotTheMenus() {
    Context ctx;
    ctx.inSession.store(true);
    ctx.SetTrack("Northgate Raceway");
    ctx.riders.store(22);
    Trail t;

    std::vector<std::string> out;
    WriteContext(ctx, t, /*nowMs=*/1'000, &Collect, &out);
    CHECK(Mentions(out, "in a session"), "a live session is not reported as the menus");
    CHECK(!Mentions(out, "in the menus"), "and not both at once");
    CHECK(Mentions(out, "this copy sees no frames"), "the limit is stated, not papered over");
    CHECK(Mentions(out, "riders=22"), "the mirrored grid is still in the report");
}

static void ReportIsHonestWhenItKnowsNothing() {
    Context ctx;   // untouched: injected into a game that never reached a track
    Trail t;
    std::vector<std::string> out;
    WriteContext(ctx, t, /*nowMs=*/5'000, &Collect, &out);

    CHECK(Mentions(out, "in the menus"), "an unknown state says so rather than guessing");
    CHECK(Mentions(out, "riders=n/a"), "no race means no rider count, not zero");
    CHECK(Mentions(out, "<none>"), "an unset track/server reads as none");
    CHECK(Mentions(out, "no content reload has run"), "and no reload is stated, not implied");
    CHECK(Mentions(out, "nothing on the trail"), "an empty trail is called empty");
    CHECK(!Mentions(out, "last frame drawn"), "with no frame drawn there is no frame time");
}

static void ReportSaysWhatItDropped() {
    Context ctx;
    Trail t;
    for (int i = 0; i < kMaxCrumbs + 10; ++i) t.Add((unsigned long long)i, "something happened");

    std::vector<std::string> out;
    WriteContext(ctx, t, /*nowMs=*/100'000, &Collect, &out);
    char expect[64];
    std::snprintf(expect, sizeof(expect), "the last %d of %d", kMaxCrumbs, kMaxCrumbs + 10);
    CHECK(Mentions(out, expect),
          "a wrapped trail says how many it dropped ('%s')", expect);
}

// A clock that has gone backwards (a report written a tick before the stamp it is
// comparing against) must not print a nonsense duration - it underflows to something
// like 18446744073709551615ms, which reads as a bug in the game rather than in us.
static void ClockSkewDoesNotPrintNonsense() {
    Context ctx;
    ctx.lastDrawMs.store(5'000);
    ctx.reloads.store(1);
    ctx.lastReloadMs.store(5'000);
    Trail t;
    t.Add(5'000, "a note from the future");

    std::vector<std::string> out;
    WriteContext(ctx, t, /*nowMs=*/4'000, &Collect, &out);
    for (const auto& l : out)
        CHECK(l.find("1844674407") == std::string::npos,
              "no underflowed duration in: %s", l.c_str());
}

int main() {
    TrailKeepsTheNewest();
    ReportSaysWhereTheyWere();
    ASessionWithoutFramesIsNotTheMenus();
    ReportIsHonestWhenItKnowsNothing();
    ReportSaysWhatItDropped();
    ClockSkewDoesNotPrintNonsense();

    if (g_failures) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("crashreport: all checks passed\n");
    return 0;
}
