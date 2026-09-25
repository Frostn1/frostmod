// The NaN watch in nanwatch.h: when a rider's position stops being a number, it says so
// once, with where they last were; and at a crash it names the XMM registers that hold
// something that is not a number. Pure, so it runs on any host.

#include "../src/nanwatch.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <limits>

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

using namespace frostmod::nanwatch;

// MSVC rejects a compile-time 0.0/0.0 outright (C2124), so these come from <limits>.
static const float kNaN = std::numeric_limits<float>::quiet_NaN();
static const float kInf = std::numeric_limits<float>::infinity();

static void TheFirstBadPositionIsReportedOnce() {
    Watch w;
    CHECK(w.See(7, 1.f, 2.f, 3.f, 1000) == Edge::None, "a number is not news");
    CHECK(w.See(7, 1.5f, 2.f, 3.f, 1100) == Edge::None, "neither is the next one");
    CHECK(w.See(7, kNaN, 2.f, 3.f, 1200) == Edge::WentBad, "one NaN axis is enough");
    CHECK(w.See(7, kNaN, kNaN, kNaN, 1300) == Edge::None, "still bad is not a new report");

    char line[256];
    w.Describe(7, Edge::WentBad, kNaN, 2.f, 3.f, 1200, line, sizeof(line));
    CHECK(std::strstr(line, "rider #7") != nullptr, "names the rider: %s", line);
    CHECK(std::strstr(line, "(1.50, 2.00, 3.00)") != nullptr, "the last real position: %s", line);
    CHECK(std::strstr(line, "100ms before") != nullptr, "and how long before: %s", line);

    CHECK(w.See(7, 4.f, 5.f, 6.f, 1500) == Edge::Recovered, "coming back is its own report");
    w.Describe(7, Edge::Recovered, 4.f, 5.f, 6.f, 1500, line, sizeof(line));
    CHECK(std::strstr(line, "after 300ms") != nullptr, "how long it was bad: %s", line);
    CHECK(w.See(7, kInf, 5.f, 6.f, 1600) == Edge::WentBad, "an infinity counts too");
}

static void RidersAreWatchedApart() {
    Watch w;
    w.See(kMe, 1.f, 1.f, 1.f, 10);
    w.See(3, 2.f, 2.f, 2.f, 10);
    CHECK(w.See(3, kNaN, 0.f, 0.f, 20) == Edge::WentBad, "rider 3 goes bad");
    CHECK(w.See(kMe, 1.f, 1.f, 1.f, 20) == Edge::None, "that is not our bike");
    char line[256];
    w.Describe(kMe, Edge::WentBad, kNaN, 0.f, 0.f, 30, line, sizeof(line));
    CHECK(std::strstr(line, "our bike") != nullptr, "names us: %s", line);

    Watch fresh;
    CHECK(fresh.See(9, kNaN, 0.f, 0.f, 5) == Edge::WentBad, "bad from the start");
    fresh.Describe(9, Edge::WentBad, kNaN, 0.f, 0.f, 5, line, sizeof(line));
    CHECK(std::strstr(line, "never was one") != nullptr, "and says so: %s", line);
}

static void TheTableAndTheLogAreBounded() {
    Watch w;
    for (int i = 0; i < kSlots; ++i) w.See(i, 0.f, 0.f, 0.f, 1);
    CHECK(w.See(kSlots, kNaN, 0.f, 0.f, 2) == Edge::None, "one past the table is not watched");
    CHECK(w.See(0, kNaN, 0.f, 0.f, 2) == Edge::WentBad, "everyone in it still is");

    unsigned logged = 0;
    for (unsigned i = 0; i < kMaxReports + 5; ++i) logged += w.Budget() ? 1u : 0u;
    CHECK(logged == kMaxReports, "the log budget holds: %u", logged);

    w.Reset();
    CHECK(w.See(0, kNaN, 0.f, 0.f, 3) == Edge::WentBad, "a new session starts clean");
    CHECK(!w.Budget(), "but the run's log budget is not refilled");
}

static void ALeaverFreesTheirSlot() {
    Watch w;
    for (int i = 0; i < kSlots; ++i) w.See(i, 0.f, 0.f, 0.f, 1);
    w.See(5, kNaN, 0.f, 0.f, 2);
    w.Forget(5);
    CHECK(w.See(500, kNaN, 0.f, 0.f, 3) == Edge::WentBad, "a newcomer gets the freed slot");
    w.Forget(500);
    CHECK(w.See(500, 1.f, 1.f, 1.f, 4) == Edge::None, "and the next one on it starts clean");
}

static void TheTrailLineFitsTheTrail() {
    Watch w;
    w.See(kMe, -12345.6f, 123.4f, -9876.5f, 1000);
    w.See(kMe, kNaN, kNaN, kNaN, 1250);
    char brief[112];
    const int len = w.Brief(kMe, Edge::WentBad, 1250, brief, sizeof(brief));
    CHECK(len > 0 && len < (int)sizeof(brief), "fits: %d %s", len, brief);
    CHECK(std::strstr(brief, "our bike NaN, last real 250ms before") != nullptr, "%s", brief);
}

static uint64_t FloatBits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}
static uint64_t DoubleBits(double d) {
    uint64_t b;
    std::memcpy(&b, &d, sizeof(b));
    return b;
}

static void TheRegistersThatAreNotNumbersAreNamed() {
    uint64_t lows[16] = {};
    for (int i = 0; i < 16; ++i) lows[i] = FloatBits(1.0f + (float)i);
    char out[200];
    CHECK(NonFiniteXmm(lows, out, sizeof(out)) == 0, "all numbers");
    CHECK(std::strcmp(out, "none") == 0, "says none: %s", out);

    lows[3] = FloatBits(kNaN);
    lows[8] = DoubleBits(std::numeric_limits<double>::infinity());
    CHECK(NonFiniteXmm(lows, out, sizeof(out)) == 2, "two of them: %s", out);
    CHECK(std::strstr(out, "xmm3 (float NaN)") != nullptr, "%s", out);
    CHECK(std::strstr(out, "xmm8 (double inf)") != nullptr, "%s", out);

    // The value cvttss2si makes of a NaN is 0x80000000, and read as a float that is -0.0:
    // a number. It must not be flagged, or every index register would be.
    lows[3] = 0x80000000u;
    lows[8] = FloatBits(2.f);
    CHECK(NonFiniteXmm(lows, out, sizeof(out)) == 0, "0x80000000 is -0.0f: %s", out);

    // Every one of them fits the buffer the crash report gives it.
    for (int i = 0; i < 16; ++i) lows[i] = DoubleBits(std::numeric_limits<double>::quiet_NaN());
    char full[320];
    CHECK(NonFiniteXmm(lows, full, sizeof(full)) == 16, "all 16");
    CHECK(std::strstr(full, "xmm15 (") != nullptr, "the last one is still named: %s", full);

    // A buffer too small for the list still ends in a terminated string.
    for (int i = 0; i < 16; ++i) lows[i] = FloatBits(kNaN);
    char tiny[16];
    CHECK(NonFiniteXmm(lows, tiny, sizeof(tiny)) == 16, "counts them all");
    CHECK(std::memchr(tiny, 0, sizeof(tiny)) != nullptr, "terminated");
}

int main() {
    TheFirstBadPositionIsReportedOnce();
    RidersAreWatchedApart();
    TheTableAndTheLogAreBounded();
    ALeaverFreesTheirSlot();
    TheTrailLineFitsTheTrail();
    TheRegistersThatAreNotNumbersAreNamed();

    if (g_failures == 0) std::printf("nanwatch_test: all checks passed\n");
    else                 std::printf("nanwatch_test: %d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
