// src/petride.h: when the panel may draw (never while riding) and Pip riding along. Pure, so
// CI runs it.

#include "../src/petride.h"

#include <cstdio>
#include <cstring>
#include <vector>

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

using namespace petride;

static void Riding_() {
    CHECK(!Riding(false, 1000, 990), "stopped (RunStop) is paused, even with a fresh sample");
    CHECK(!Riding(true, 1000, 0), "no sample yet is not riding");
    CHECK(Riding(true, 1000, 900), "a sample 100 ms ago is riding");
    CHECK(Riding(true, 1000, 1000 - kTelemetryFreshMs), "the edge of fresh is riding");
    CHECK(!Riding(true, 1000, 1000 - kTelemetryFreshMs - 1), "samples stopped: paused");
    CHECK(Riding(true, 1000, 1005), "a stamp from another thread a moment later counts");

    const char* riding[] = {"mtrackm"};
    const char* esc[] = {"mtrackm", "pause"};
    const char* pits[] = {"chat", "pit", "multi_pit"};
    const char* other[] = {"mtrack"};
    CHECK(TrackOnTop(riding, 1) && TrackOnTop(other, 1), "the track page alone: riding");
    CHECK(!TrackOnTop(esc, 2), "a page over the track (the Esc menu): not riding");
    CHECK(!TrackOnTop(pits, 3) && !TrackOnTop(pits, 0), "pits, or no pages: not riding");
    CHECK(!Riding(0, true, 1000, 990), "online Esc: the page says no while telemetry still flows");
    CHECK(Riding(1, true, 1000, 990) && Riding(-1, true, 1000, 990), "track on top, or unreadable: telemetry decides");
    CHECK(!Riding(1, false, 1000, 990), "offline pause (RunStop) with the track page still on top");
    CHECK(EscMenuOver(2, 0), "Esc menu: UI one deeper, input off");
    CHECK(!EscMenuOver(1, 1) && !EscMenuOver(2, 1) && !EscMenuOver(1, 0), "one signal alone is not the Esc menu");
    CHECK(!EscMenuOver(-1, -1), "unreadable is not the Esc menu");
}

static std::vector<char> Bike(float speed, int crashed, int w0, int w1) {
    std::vector<char> b(200, 0);
    std::memcpy(b.data() + kOffSpeed, &speed, 4);
    std::memcpy(b.data() + kOffCrashed, &crashed, 4);
    std::memcpy(b.data() + kOffWheelMat, &w0, 4);
    std::memcpy(b.data() + kOffWheelMat + 4, &w1, 4);
    return b;
}

static void Read_() {
    Sample s;
    auto b = Bike(12.5f, 0, 3, 3);
    CHECK(Read(b.data(), (int)b.size(), &s) && s.speed == 12.5f && !s.air && !s.crashed, "on the ground");
    b = Bike(20.0f, 0, 0, 0);
    CHECK(Read(b.data(), (int)b.size(), &s) && s.air, "both wheels off: air");
    b = Bike(20.0f, 0, 0, 2);
    CHECK(Read(b.data(), (int)b.size(), &s) && !s.air, "one wheel down: not air");
    b = Bike(0.0f, 1, 0, 0);
    CHECK(Read(b.data(), (int)b.size(), &s) && s.crashed && !s.air, "crashed is not air");
    CHECK(!Read(b.data(), kMinSize - 1, &s), "short payload refused");
    CHECK(!Read(nullptr, 400, &s), "null refused");
}

static void Mood_() {
    Mood m;
    Sample ground{10.0f, false, false}, air{15.0f, true, false}, crash{0.0f, false, true}, still{};
    m.step(ground, true, 0.1f);
    CHECK(m.stride != 0, "running: the feet move");
    for (int i = 0; i < 5; ++i) m.step(air, true, 0.1f);
    CHECK(m.face == WOW && m.hop == 1.0f, "a short flight: wow, up with the bike");
    for (int i = 0; i < 6; ++i) m.step(air, true, 0.1f);
    CHECK(m.face == SCARED, "a long flight: scared");
    m.step(ground, true, 0.1f);
    CHECK(m.face == HAPPY, "landed: happy");
    for (int i = 0; i < 15; ++i) m.step(ground, true, 0.1f);
    CHECK(m.face != HAPPY, "the cheer ends");
    m.step(crash, true, 0.1f);
    CHECK(m.face == SCARED && m.shake != 0.0f, "crashed: scared and shaking");
    for (int i = 0; i < 210; ++i) m.step(still, true, 0.1f);
    CHECK(m.face == SLEEP || m.face == BLINK, "still for 21 s: asleep");
    Mood p;
    p.step(air, false, 0.1f);
    CHECK(p.face != WOW && p.hop == 0.0f && p.stride == 0, "paused: the bike's state is ignored");
    Mood q;
    q.step(ground, true, 5.0f);
    CHECK(q.t == 0.0f, "a long gap (back from pause) does not jump the clock");
}

static void Place_() {
    const Box b = Place(BOTTOM_RIGHT, 16.0f / 9.0f);
    CHECK(b.x0 + b.w < 1.0f && b.y0 + b.h < 1.0f && b.x0 > 0.8f && b.y0 > 0.8f, "bottom right, on screen");
    CHECK(b.w * 16.0f / 9.0f > b.h * 0.99f && b.w * 16.0f / 9.0f < b.h * 1.01f, "square pixels");
    const Box l = Place(BOTTOM_LEFT, 16.0f / 9.0f);
    CHECK(l.x0 < 0.05f, "bottom left");
    const Box t = Place(TOP_RIGHT, 16.0f / 9.0f);
    CHECK(t.y0 < 0.1f && t.x0 > 0.8f, "top right, clear of the standings on the left");
    const Box bad = Place(BOTTOM_RIGHT, 0.0f);
    CHECK(bad.w > 0.0f && bad.x0 + bad.w < 1.0f, "no viewport yet: 16:9");
}

static void Build_() {
    Mood m;
    std::vector<Quad> q;
    Build(q, OFF, 1.7f, m);
    CHECK(q.empty(), "off draws nothing");
    Build(q, BOTTOM_RIGHT, 16.0f / 9.0f, m);
    CHECK(q.size() > 20 && q.size() < 80, "a sprite's worth of quads (%zu), inside the draw budget", q.size());
    for (const Quad& x : q) {
        CHECK(x.x0 >= 0 && x.x1 <= 1 && x.y0 >= 0 && x.y1 <= 1 && x.x0 < x.x1 && x.y0 < x.y1, "on screen and the right way round");
    }
    for (Corner c = BOTTOM_RIGHT; c != OFF; c = NextCorner(c)) CHECK(CornerName(c)[0] != 'o', "named");
    CHECK(NextCorner(TOP_RIGHT) == OFF && NextCorner(OFF) == BOTTOM_RIGHT, "F8 > P walks round to off and back");
}

int main() {
    Riding_();
    Read_();
    Mood_();
    Place_();
    Build_();
    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("petride: all passed\n");
    return 0;
}
