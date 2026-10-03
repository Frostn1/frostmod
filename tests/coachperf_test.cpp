// MXB Coach recorder: the frame report, the hook gate, the bounded camera search and safe mode
// (src/coachperf.h). Pure C++, runs anywhere.

#include "../src/coachperf.h"
#include "../src/coachhud.h"

#include <cmath>
#include <cstdio>
#include <string>

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

using namespace coachperf;

// Column-major, as GL takes them.
static void Ortho(float l, float r, float b, float t, float n, float f, float m[16]) {
    for (int i = 0; i < 16; ++i) m[i] = 0;
    m[0] = 2 / (r - l), m[5] = 2 / (t - b), m[10] = -2 / (f - n);
    m[12] = -(r + l) / (r - l), m[13] = -(t + b) / (t - b), m[14] = -(f + n) / (f - n), m[15] = 1;
}
static void Perspective(float fovy_deg, float aspect, float n, float f, float m[16]) {
    for (int i = 0; i < 16; ++i) m[i] = 0;
    const float c = 1.0f / std::tan(fovy_deg * 0.5f * 0.0174533f);
    m[0] = c / aspect, m[5] = c, m[10] = (f + n) / (n - f), m[11] = -1, m[14] = 2 * f * n / (n - f);
}

static void TestHist() {
    FrameHist h;
    for (int i = 0; i < 99; ++i) h.add(5000);  // 5 ms
    h.add(2100000);                            // one 2.1 s freeze
    CHECK(h.count() == 100, "count %llu", (unsigned long long)h.count());
    CHECK(h.over50() == 1, "over50 %llu", (unsigned long long)h.over50());
    CHECK(h.max_us() == 2100000, "max %llu", (unsigned long long)h.max_us());
    const uint64_t p50 = h.percentile_us(0.5), p99 = h.percentile_us(0.99), p100 = h.percentile_us(1.0);
    CHECK(p50 >= 5000 && p50 <= 5100, "p50 %llu", (unsigned long long)p50);
    CHECK(p99 >= 5000 && p99 <= 5100, "p99 %llu (99 of 100 frames are 5 ms)", (unsigned long long)p99);
    CHECK(p100 == 2100000, "p100 %llu", (unsigned long long)p100);
    // A bad tail shows in p99.
    FrameHist t;
    for (int i = 0; i < 95; ++i) t.add(6000);
    for (int i = 0; i < 5; ++i) t.add(100000);
    CHECK(t.percentile_us(0.99) >= 100000 && t.percentile_us(0.99) <= 110000, "p99 of a 5%% tail %llu",
          (unsigned long long)t.percentile_us(0.99));
    CHECK(t.percentile_us(0.5) <= 6100, "p50 %llu", (unsigned long long)t.percentile_us(0.5));
    const std::string s = FrameText(t, 10.0);
    CHECK(s.find("frames=100") != std::string::npos && s.find("over50ms=5") != std::string::npos, "%s", s.c_str());
    t.reset();
    CHECK(t.count() == 0 && t.percentile_us(0.5) == 0, "reset");
    // Buckets: fine to 50 ms, coarse to 2 s, one more past that.
    CHECK(FrameHist::Bucket(0) == 0 && FrameHist::Bucket(49999) == 499, "fine buckets");
    CHECK(FrameHist::Bucket(50000) == FrameHist::kFine, "first coarse bucket");
    CHECK(FrameHist::Bucket(5000000) == FrameHist::kBuckets - 1, "overflow bucket");
}

static void TestAcc() {
    Acc     a;
    AccSnap prev;
    a.add(10), a.add(30), a.skip(), a.skip(), a.skip();
    AccSnap d = Take(a, prev);
    CHECK(d.n == 2 && d.sum == 40 && d.max == 30 && d.pass == 3, "first window n=%llu sum=%llu max=%llu pass=%llu",
          (unsigned long long)d.n, (unsigned long long)d.sum, (unsigned long long)d.max, (unsigned long long)d.pass);
    a.add(5);
    d = Take(a, prev);
    CHECK(d.n == 1 && d.sum == 5 && d.max == 5 && d.pass == 0, "second window is a difference, max per window");
    d = Take(a, prev);
    CHECK(d.n == 0 && d.max == 0, "an empty window");
    const std::string s = SlotText(HK_MODELVIEW, AccSnap{1200, 4200, 7, 40000}, 0.1);
    CHECK(s.find("hook.modelview n=1200 pass=40000") == 0, "%s", s.c_str());
}

static void TestSpikes() {
    SpikeRing r;
    for (int i = 0; i < 3; ++i) {
        Spike s;
        s.frame_us = uint64_t(60000 + i);
        r.add(s);
    }
    int n = 0;
    r.each_since(0, [&](const Spike&) { ++n; });
    CHECK(n == 3, "three spikes, got %d", n);
    for (int i = 0; i < 20; ++i) r.add(Spike{});
    n = 0;
    r.each_since(3, [&](const Spike&) { ++n; });
    CHECK(n == SpikeRing::kN, "only the last %d are kept, got %d", SpikeRing::kN, n);
    Spike s;
    s.frame_us = 2000000, s.ours_us = 300, s.top = SWAP_WORK, s.top_us = 200, s.loads = 1200;
    const std::string t = SpikeText(s);
    CHECK(t.find("2000.0 ms frame: ours 0.30 ms (top swap.work 0.20 ms)") == 0, "%s", t.c_str());
}

static void TestGate() {
    float o[16], p[16], flip[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1}, id[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                                                                                0, 0, 1, 0, 0, 0, 0, 1};
    Ortho(0, 1920, 1080, 0, -1, 1, o);
    Perspective(70, 16.0f / 9.0f, 0.1f, 2000, p);
    CHECK(IsOrtho(o) && !IsPerspective(o), "glOrtho is ortho");
    CHECK(IsPerspective(p) && !IsOrtho(p), "a frustum is perspective");
    CHECK(IsOrtho(id), "identity counts as 2D");

    HookGate g;
    CHECK(!g.offer(), "nothing before the first swap says so");
    g.frame(true, true);
    CHECK(g.offer(), "a live frame starts in the world pass");
    g.projection(o);
    CHECK(!g.offer(), "an ortho projection: the HUD pass, passthrough");
    g.frustum();
    CHECK(g.offer(), "a frustum: back in the world");
    g.ortho();
    CHECK(!g.offer(), "glOrtho: HUD");
    g.projection(p);
    CHECK(g.offer(), "a perspective load: world");
    // The game's z flip is multiplied onto its frustum; the hook only lets a perspective multiply
    // change the pass (hkMult), so the flip never reads as a HUD pass.
    CHECK(!IsPerspective(flip), "the z flip is not a perspective");
    g.frame(true, false);
    CHECK(!g.offer() && g.live, "live but the search resting: loads pass, the projection is still tracked");
    g.frame(false, true);
    CHECK(!g.offer() && !g.search, "not riding: nothing, whatever the search says");
    g.ortho();
    g.frame(true, true);
    CHECK(g.world, "each frame starts in the world pass again");
}

static void TestBackoff() {
    SearchBackoff b;
    uint64_t      t = 1000;
    CHECK(b.frame(t, true, false), "searching from the first frame");
    t += SearchBackoff::kFirstMs - 1;
    CHECK(b.frame(t, true, false) && !b.resting(), "still searching just before the first window ends");
    t += 2;
    CHECK(!b.frame(t, true, false) && b.resting() && b.rests() == 1, "no camera in the first window: rest");
    t += SearchBackoff::kRestMs - 10;
    CHECK(!b.frame(t, true, false), "resting");
    t += 20;
    CHECK(b.frame(t, true, false) && !b.resting(), "a retry after the rest");
    t += SearchBackoff::kRetryMs + 1;
    CHECK(!b.frame(t, true, false) && b.rests() == 2, "the retry is short");
    // Over a minute of searching for a camera that is never there, it runs about a seventh of the time.
    SearchBackoff c;
    int on = 0, frames = 0;
    for (uint64_t ms = 0; ms < 60000; ms += 10, ++frames)
        if (c.frame(ms, true, false)) ++on;
    const double share = double(on) / double(frames);
    CHECK(share < 0.20, "searching %.0f%% of a minute with nothing to find", share * 100);
    // Found: it is followed every frame, and the clock starts over.
    CHECK(c.frame(70000, true, true) && !c.resting(), "a camera found: loads looked at (followed)");
    CHECK(c.frame(70010, true, false), "lost again: a fresh first window");
    CHECK(c.frame(70010 + SearchBackoff::kFirstMs - 10, true, false), "the full first window again");
    // Not riding: nothing, and the clock starts over.
    CHECK(!c.frame(80000, false, false) && !c.resting(), "not riding");
    // Safe mode: shorter looks, longer rests.
    SearchBackoff s;
    s.set_safe(true);
    on = frames = 0;
    for (uint64_t ms = 0; ms < 60000; ms += 10, ++frames)
        if (s.frame(ms, true, false)) ++on;
    CHECK(double(on) / double(frames) < 0.08, "safe mode searches %.1f%% of a minute", 100.0 * on / frames);
}

static void TestSafe() {
    CHECK(ParseSafe("1") == SAFE_ON && ParseSafe("on") == SAFE_ON, "on");
    CHECK(ParseSafe("0") == SAFE_OFF && ParseSafe("off") == SAFE_OFF, "off");
    CHECK(ParseSafe("") == SAFE_AUTO && ParseSafe("auto") == SAFE_AUTO && ParseSafe("x") == SAFE_AUTO, "auto");
    CHECK(SafeOn(SAFE_AUTO, true) && !SafeOn(SAFE_AUTO, false), "auto follows MXBMRP3");
    CHECK(SafeOn(SAFE_ON, false) && !SafeOn(SAFE_OFF, true), "an explicit setting wins");
    CHECK(coachhud::ParseSettings("", false).safe == SAFE_AUTO, "hud.ini without the key: auto");
    CHECK(coachhud::ParseSettings("[hud]\nsafe_mode=0\n", true).safe == SAFE_OFF, "safe_mode=0");
    CHECK(coachhud::ParseSettings("[hud]\nsafe_mode=1\n", false).safe == SAFE_ON, "safe_mode=1");
    CHECK(std::string(SafeDrops()).find("depth reads") != std::string::npos, "the log says what it drops");
}

int main() {
    TestHist();
    TestAcc();
    TestSpikes();
    TestGate();
    TestBackoff();
    TestSafe();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("coachperf: all passed\n");
    return 0;
}
