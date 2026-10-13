// Cut markers (src/coachcuts.h): the markers from the centreline, which one a lap position is
// on, the live watch for a skip, the game's penalty and lap payloads, the tally, the files
// MXB Coach reads, and the HUD layer. Pure C++, runs anywhere.

#include "../src/coachhud.h"
#include "../src/coachcutmarks.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            ++g_failures;                                    \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n  (%s)\n", #cond);                \
        }                                                    \
    } while (0)

using namespace coachcuts;

// A round track, 1000 m: four right-hand quarter arcs of 250 m, as SPluginsTrackSegment_t.
static const float kLen = 1000.0f;
static const float kR   = kLen / (2.0f * 3.14159265f);

static coachhud::Track RoundTrack(float start_line) {
    uint8_t segs[4 * 28] = {0};
    for (int i = 0; i < 4; ++i) {
        uint8_t*      r      = segs + i * 28;
        const int32_t type   = 1;
        const float   len    = 250.0f, radius = kR, angle = 0, h = 0;
        const float   start[2] = {0, 0};
        std::memcpy(r, &type, 4);
        std::memcpy(r + 4, &len, 4);
        std::memcpy(r + 8, &radius, 4);
        std::memcpy(r + 12, &angle, 4);
        std::memcpy(r + 16, start, 8);
        std::memcpy(r + 24, &h, 4);
    }
    coachhud::Track t;
    const float race[1] = {start_line};
    t.build(4, segs, 28, start_line > 0 ? race : nullptr);
    return t;
}

static void markers() {
    const coachhud::Track t = RoundTrack(0);
    const std::vector<Marker> ms = coachhud::Markers(t);
    CHECK(ms.size() == 4, "4 markers, got %d", int(ms.size()));
    CHECK(std::fabs(ms[1].at_m - 250.0f) < 0.5f, "marker 1 at 250 m, got %.1f", double(ms[1].at_m));
    CHECK(MarkerAt(ms, 10) == 0 && MarkerAt(ms, 260) == 1 && MarkerAt(ms, 999) == 3, "marker at");

    // A start line 100 m along the centreline: marker 1 is 150 m past it and the stretch
    // between the line and marker 1 belongs to marker 0.
    const coachhud::Track t2 = RoundTrack(100);
    const std::vector<Marker> m2 = coachhud::Markers(t2);
    CHECK(std::fabs(m2[1].at_m - 150.0f) < 0.5f, "marker 1 150 m past the line, got %.1f", double(m2[1].at_m));
    CHECK(std::fabs(m2[0].at_m - 900.0f) < 0.5f, "marker 0 900 m past the line, got %.1f", double(m2[0].at_m));
    CHECK(MarkerAt(m2, 10) == 0, "before marker 1 is marker 0, got %d", MarkerAt(m2, 10));
    CHECK(MarkerAt(m2, 160) == 1, "past marker 1, got %d", MarkerAt(m2, 160));
}

// Rides the round track at `speed` m/s, 50 Hz, from lap position `from` for `secs`.
struct Ride {
    const coachhud::Track&     t;
    const std::vector<Marker>& ms;
    Watch                      w;
    float                      time = 0, pos = 0;
    int                        cuts = 0;
    Cut                        last;
    void go(float secs, float speed) {
        for (float s = 0; s < secs; s += 0.02f) {
            time += 0.02f;
            pos = std::fmod(pos + speed * 0.02f / kLen, 1.0f);
            float x = 0, z = 0;
            t.at(pos * kLen, x, z);
            Cut c;
            if (w.sample(time, pos, x, z, false, kLen, ms, c)) ++cuts, last = c;
        }
    }
    /// Jumps `skip` metres along the line while the bike moves a short way straight across.
    void skip(float skip) {
        time += 0.02f;
        pos = std::fmod(pos + skip / kLen, 1.0f);
        float x = 0, z = 0;
        t.at(pos * kLen, x, z);
        Cut c;
        if (w.sample(time, pos, x, z, false, kLen, ms, c)) ++cuts, last = c;
    }
};

static void watch() {
    const coachhud::Track     t  = RoundTrack(0);
    const std::vector<Marker> ms = coachhud::Markers(t);
    {
        Ride r{t, ms};
        r.go(60, 20);  // more than a lap, on the line
        CHECK(r.cuts == 0, "riding the line is no cut, got %d", r.cuts);
    }
    {
        // A skip from 230 m to 290 m across a short chord: ground ~ the chord (the arc there
        // is nearly straight), so the gain is small - a skip of 120 m is not.
        Ride r{t, ms};
        r.go(11, 20);  // 220 m
        r.skip(120);   // to 340 m in one sample: the bike "moved" ~118 m - a teleport
        CHECK(r.cuts == 0, "a jump over 60 m on the ground is a reset, not a cut, got %d", r.cuts);
    }
    {
        // Short chord across an S: in four samples the line advances 60 m while the bike
        // covers 8 m on the ground - the watch sees the lap position, not the ground.
        Ride r{t, ms};
        r.go(11.5f, 20);  // 230 m, on marker 0
        // Ground position held near where the bike is; lap position moves on.
        float x0, z0;
        t.at(r.pos * kLen, x0, z0);
        for (int i = 0; i < 4; ++i) {
            r.time += 0.02f;
            r.pos += 15.0f / kLen;
            Cut c;
            if (r.w.sample(r.time, r.pos, x0 + float(i) * 2.0f, z0, false, kLen, ms, c)) ++r.cuts, r.last = c;
        }
        CHECK(r.cuts == 1, "a 60 m skip over 8 m of ground is a cut, got %d", r.cuts);
        CHECK(r.last.from_id == 0 && r.last.to_id == 1, "marker 0 > 1, got %d > %d", r.last.from_id, r.last.to_id);
        CHECK(r.last.gain_m >= kMinGainM, "gain %.1f", double(r.last.gain_m));
        CHECK(r.w.pending().size() == 1, "one pending");
        std::vector<Cut> seen;
        Cut              best;
        bool             has = false;
        r.w.end_lap(seen, best, has);
        CHECK(seen.size() == 1 && r.w.pending().empty(), "end_lap hands them over");
    }
    {
        // A crash resets: the respawn's jump is never a cut.
        const coachhud::Track     tt = RoundTrack(0);
        Watch                     w;
        Cut                       c;
        float                     x = 0, z = 0;
        tt.at(100, x, z);
        w.sample(1.0f, 0.1f, x, z, false, kLen, ms, c);
        w.sample(1.02f, 0.1f, x, z, true, kLen, ms, c);
        tt.at(300, x, z);
        CHECK(!w.sample(1.04f, 0.3f, x, z, false, kLen, ms, c), "after a crash, no cut");
    }
}

static void payloads() {
    int32_t comm[10] = {5, 42, 2, 0, 0, 2, 3, 0, 0, 10000};
    int     num = 0, lap = 0, ms = 0;
    CHECK(CutPenalty(comm, sizeof(comm), num, lap, ms) && num == 42 && lap == 3 && ms == 10000, "cut penalty");
    comm[5] = 1;  // jump start
    CHECK(!CutPenalty(comm, sizeof(comm), num, lap, ms), "a jump start is no cut");
    CHECK(!CutPenalty(comm, 36, num, lap, ms), "short payload");
    int32_t bl[4] = {7, 1, 61000, 0};
    bool    inv   = false;
    CHECK(LapInvalid(bl, sizeof(bl), lap, inv) && lap == 7 && inv, "invalid lap");
}

static void tally_and_files() {
    Tally t;
    t.add(27), t.add(12), t.add(12), t.add(12), t.add(-1);
    CHECK(t.total() == 4 && t.count(12) == 3, "tally");
    CHECK(t.Line(4) == "#12 x3  #27 x1", "line '%s'", t.Line(4).c_str());
    Cut c;
    c.source = SRC_GAME, c.confirmed = true, c.lap = 2, c.from_id = 12, c.to_id = 13, c.penalty_ms = 5000;
    const std::string j = CutLine(c, "20261013-100000-000", "20261013-095500-000", "My \"Track\"", "Benny", true);
    CHECK(j.find("\"marker\":12") != std::string::npos && j.find("\"to\":13") != std::string::npos, "%s", j.c_str());
    CHECK(j.find("My \\\"Track\\\"") != std::string::npos && j.find("\"source\":\"game\"") != std::string::npos, "%s",
          j.c_str());
    std::vector<Marker> ms(2);
    ms[1].id = 1, ms[1].at_m = 250;
    const std::string mj = MarkersJson("T", 1000, ms);
    CHECK(mj.find("\"markers\":[{\"id\":0") != std::string::npos && mj.find("\"at_m\":250.0") != std::string::npos,
          "%s", mj.c_str());
    CHECK(FileSafe("a/b:c") == "a_b_c", "file safe");
}

static void hud() {
    const coachhud::Track     t  = RoundTrack(0);
    const std::vector<Marker> ms = coachhud::Markers(t);
    Tally                     tally;
    tally.add(2);
    coachhud::View v;
    v.set        = coachhud::ParseSettings("[hud]\nmap=0\ncuts=1\n", false);
    v.track      = &t;
    v.markers    = &ms;
    v.marker_now = 1;
    v.tally      = &tally;
    v.has_flash  = true;
    v.flash      = 1;
    v.flash_cut.from_id = 2, v.flash_cut.to_id = 3;
    coachhud::Frame f;
    coachhud::Build(v, f);
    bool here = false, cuts = false, flash = false, num = false;
    for (const auto& s : f.texts) {
        here  |= s.s == "Marker 1";
        cuts  |= s.s == "Cuts #2 x1";
        flash |= s.s == "CUT  marker 2 > 3";
        num   |= s.s == "2";
    }
    CHECK(here && cuts && flash && num, "cut layer texts (here %d cuts %d flash %d num %d)", here, cuts, flash, num);
    CHECK(!f.quads.empty(), "map drawn with map=0 when cuts=1");

    coachhud::View off = v;
    off.set = coachhud::ParseSettings("[hud]\nmap=0\n", false);
    coachhud::Frame g;
    coachhud::Build(off, g);
    CHECK(g.texts.empty() && g.quads.empty(), "cuts off by default: nothing");
}

static void gates() {
    using namespace coachcutmarks;
    CHECK(Fade(10) == 1.0f && Fade(kFar) == 0.0f && Fade(1000) == 0.0f, "fade ends");
    CHECK(Fade(60) > 0.0f && Fade(60) < 1.0f, "fading between, %.2f", double(Fade(60)));
    CHECK(Fade(1) < Fade(kNear + 1), "faded right under the bike");
    // Three markers north of the rider at 10, 60 and 100 m, the line heading north.
    std::vector<Pose> ps(3);
    const float at[3] = {10, 60, 100};
    for (int i = 0; i < 3; ++i) ps[i].id = i, ps[i].x = 0, ps[i].y = 5, ps[i].z = at[i], ps[i].heading = 0;
    Tally t;
    t.add(1);
    std::vector<coachmark::Quad> q;
    CHECK(Build(q, ps, 0, 0, 0, &t) == 2, "the one 100 m away is not drawn");
    // Each gate: two posts, a bar, then its number's quads.
    CHECK(q.size() > 6, "posts, bars and numbers, %d quads", int(q.size()));
    // Marker 0: the posts stand 4 m either side across the line (east-west for a line heading north).
    CHECK(std::fabs(q[0].p[0][0] + kHalfGate) < 0.2f && std::fabs(q[1].p[0][0] - kHalfGate) < 0.2f, "posts across");
    CHECK(std::fabs(q[0].p[2][1] - (5 + kPostH)) < 1e-3f && std::fabs(q[0].p[0][1] - 5) < 1e-3f, "post height");
    CHECK(q[0].rgba[1] < 1.0f && q[0].rgba[3] > 0.8f, "the current one is amber and solid");
    bool red = false;
    for (const auto& k : q) red |= k.rgba[1] < 0.3f && k.p[0][2] > 50;
    CHECK(red, "the cut one is red");
    q.clear();
    CHECK(Build(q, ps, 0, 0, -1, nullptr) == 2 && q[0].rgba[3] < 0.4f, "others are faint white");
}

int main() {
    markers();
    gates();
    watch();
    payloads();
    tally_and_files();
    hud();
    if (g_failures) {
        std::printf("coachcuts: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("coachcuts: all passed\n");
    return 0;
}
