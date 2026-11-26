// hangwatch.h: one report per stall (and one more if it lasts), a recovery line after it, and
// nothing before the first frame or while frames keep coming.
#include "../src/hangwatch.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); } } while (0)

using namespace hangwatch;

int main() {
    State s;
    CHECK(Step(s, 100000, 0) == Action::None, "no frame yet: nothing to watch");

    // Frames keep coming: nothing, ever.
    for (uint64_t t = 1000; t < 60000; t += 500) CHECK(Step(s, t + 16, t) == Action::None, "steady frames");

    // A frame at 100 s, then none.
    const uint64_t beat = 100000;
    CHECK(Step(s, beat + 4999, beat) == Action::None, "under 5 s");
    CHECK(Step(s, beat + 5000, beat) == Action::Report, "5 s: report");
    CHECK(Step(s, beat + 5500, beat) == Action::None, "once per stall");
    CHECK(Step(s, beat + 29999, beat) == Action::None, "not again before 30 s");
    CHECK(Step(s, beat + 30000, beat) == Action::ReportAgain, "30 s: once more");
    CHECK(Step(s, beat + 90000, beat) == Action::None, "and no more");
    CHECK(Step(s, beat + 91000, beat + 90900) == Action::Recovered, "a frame again: recovered");
    CHECK(Step(s, beat + 91500, beat + 91400) == Action::None, "back to normal");

    // A heartbeat stamped on the render thread a moment after `now` was read.
    CHECK(Step(s, 200000, 200010) == Action::None, "beat after now");

    // A short stall that never reached the threshold says nothing when frames return.
    State q;
    CHECK(Step(q, 300000 + 3000, 300000) == Action::None, "3 s");
    CHECK(Step(q, 300000 + 3500, 300000 + 3400) == Action::None, "no recovery line without a report");

    // Back to the menu (no frames counted as zero): reset.
    State r;
    CHECK(Step(r, 400000 + 6000, 400000) == Action::Report, "report");
    CHECK(Step(r, 400000 + 7000, 0) == Action::None && r.stallBeat == 0, "reset on zero");

    Module mods[2] = {{0x140000000ull, 0x1000000, "mxbikes.exe"}, {0x7ff800000000ull, 0x20000, "frostmod.dlo"}};
    const Module* m = nullptr; uintptr_t rva = 0;
    CHECK(Describe(mods, 2, 0x140123456ull, &m, &rva) && m == &mods[0] && rva == 0x123456, "in the exe");
    CHECK(Describe(mods, 2, 0x7ff80001ffffull, &m, &rva) && m == &mods[1] && rva == 0x1ffff, "last byte");
    CHECK(!Describe(mods, 2, 0x7ff800020000ull, &m, &rva), "one past the end");

    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("hangwatch: all checks passed\n");
    return 0;
}
