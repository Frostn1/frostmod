// Whether MXB Coach is open (src/coachlink.h): the recorder clears the line, the hints and the
// cues when Coach says it was closed or stops writing coach.ini, and draws as always when there
// is no coach.ini (a Coach from before it). Pure C++, runs anywhere.

#include "../src/coachlink.h"

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

using namespace coachlink;

static void AnOlderCoachKeepsDrawing() {
    CHECK(Decide(false, "", 0) == NONE, "no file");
    CHECK(Draws(NONE), "no file is a Coach from before it: draw as always");
}

static void AnOpenCoachDraws() {
    const char* on = "[coach]\non=1\nversion=0.1.25\n";
    CHECK(Decide(true, on, 0) == ON, "just written");
    CHECK(Decide(true, on, kTimeoutMs) == ON, "on the edge of the timeout");
    CHECK(Draws(ON), "drawn");
    // A file caught mid-replace reads empty: not a reason to clear while the beat is fresh.
    CHECK(Decide(true, "", 1000) == ON, "an unreadable but fresh file");
}

static void ClosingCoachClears() {
    CHECK(Decide(true, "[coach]\non=0\nversion=0.1.25\n", 0) == OFF, "on=0");
    CHECK(Decide(true, "[coach]\r\non = 0\r\n", 100000) == OFF, "on=0 with spaces and CRLF");
    CHECK(!Draws(OFF), "cleared");
}

static void ACoachThatStopsAnsweringClears() {
    CHECK(Decide(true, "[coach]\non=1\n", kTimeoutMs + 1) == GONE, "no beat for longer than the timeout");
    CHECK(!Draws(GONE), "cleared");
}

static void Ages() {
    CHECK(AgeMs(0, 10000 * 2500) == 2500, "FILETIME ticks are 100 ns");
    CHECK(AgeMs(10000 * 5000, 10000 * 1000) == 0, "a file from the future is just written");
}

int main() {
    AnOlderCoachKeepsDrawing();
    AnOpenCoachDraws();
    ClosingCoachClears();
    ACoachThatStopsAnsweringClears();
    Ages();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("coachlink: all passed\n");
    return 0;
}
