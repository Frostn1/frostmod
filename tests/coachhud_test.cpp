// MXB Coach's in-game HUD (src/coachhud.h).
//
// The .hud file is a wire contract with MXB Coach's Rust writer, compiled separately, so its
// bytes are pinned here, with the gap to Coach's lap, the ghost, hud.ini and where the parts go.
// Pure C++, runs anywhere.

#include "../src/coachhud.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

using namespace coachhud;

struct Out {
    std::vector<uint8_t> b;
    void raw(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    void u32(uint32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void u8(uint8_t v) { b.push_back(v); }
    void str(const std::string& s) {
        u8(uint8_t(s.size()));
        raw(s.data(), s.size());
    }
};

// Writes a sheet the way the app does.
static std::vector<uint8_t> Write(const Sheet& s) {
    Out o;
    o.raw(kMagic, 4);
    o.u32(kVersion);
    o.f32(s.track_len);
    o.u32(uint32_t(s.ref.size()));
    for (const RefPoint& p : s.ref) {
        o.f32(p.pos);
        o.f32(p.t);
        o.f32(p.x);
        o.f32(p.z);
    }
    o.u32(uint32_t(s.sections.size()));
    for (const Section& sec : s.sections) {
        o.f32(sec.start_m);
        o.f32(sec.end_m);
        o.str(sec.name);
        o.str(sec.tip);
    }
    o.u32(s.flags);
    return o.b;
}

static bool Load(const std::vector<uint8_t>& b, Sheet& s) { return Parse(b.data(), b.size(), s); }

// A 1000 m lap ridden in 100 s at a steady 10 m/s, due north from (5, 7).
static Sheet SteadyLap() {
    Sheet s;
    s.track_len = 1000;
    for (int i = 0; i <= 100; ++i) s.ref.push_back({i / 100.0f, float(i), 5.0f, 7.0f + i * 10.0f});
    return s;
}

static void TheBytesAreExact() {
    // Hand-laid, byte by byte, as docs/PLUGIN.md describes it.
    const uint8_t b[] = {
        'M', 'X', 'H', 'D',                 // 0  magic
        1, 0, 0, 0,                         // 4  version
        0x00, 0x40, 0xCE, 0x44,             // 8  track length 1650.0
        2, 0, 0, 0,                         // 12 two points
        0, 0, 0, 0,  0, 0, 0, 0,            // 16 pos 0, 0 s
        0, 0, 0x80, 0x3F, 0, 0, 0, 0x40,    //    x 1.0, z 2.0
        0, 0, 0x80, 0x3F, 0, 0, 0xF0, 0x42, // 32 pos 1.0, 120 s
        0, 0, 0x40, 0x40, 0, 0, 0x80, 0x40, //    x 3.0, z 4.0
        1, 0, 0, 0,                         // 48 one section
        0, 0, 0xC8, 0x42,                   // 52 start 100 m
        0, 0, 0x48, 0x43,                   // 56 end 200 m
        6, 'W', 'h', 'o', 'o', 'p', 's',    // 60 name
        8, 'S', 't', 'a', 'n', 'd', ' ', 'u', 'p',  // 67 tip
        1, 0, 0, 0,                         // 76 flags: sag
    };
    static_assert(sizeof(b) == 80, "the example is 80 bytes");
    Sheet s;
    CHECK(Parse(b, sizeof(b), s), "parse the hand-laid sheet");
    CHECK(s.track_len == 1650.0f, "length %f", s.track_len);
    CHECK(s.ref.size() == 2 && s.ref[1].pos == 1.0f && s.ref[1].t == 120.0f, "points");
    CHECK(s.ref[0].x == 1.0f && s.ref[0].z == 2.0f && s.ref[1].x == 3.0f && s.ref[1].z == 4.0f, "x/z");
    CHECK(s.sections.size() == 1 && s.sections[0].start_m == 100.0f && s.sections[0].end_m == 200.0f, "section");
    CHECK(s.sections[0].name == "Whoops" && s.sections[0].tip == "Stand up", "text");
    CHECK(s.flags == FLAG_SAG, "flags %u", s.flags);
    // And the test writer lays out the same bytes.
    CHECK(Write(s) == std::vector<uint8_t>(b, b + sizeof(b)), "writer matches the layout");
    // Every cut is refused.
    for (size_t n = 0; n < sizeof(b); ++n) CHECK(!Parse(b, n, s), "cut at %zu", n);
}

static void RefusesWhatTheAppDoesNotWrite() {
    Sheet s;
    const std::vector<uint8_t> ok = Write(SteadyLap());
    CHECK(Load(ok, s), "a steady lap");
    std::vector<uint8_t> bad = ok;
    bad[3] = 'Q';
    CHECK(!Load(bad, s), "wrong magic");
    bad = ok;
    bad[4] = 2;
    CHECK(!Load(bad, s), "wrong version");

    Sheet w = SteadyLap();
    w.ref[50].t = 10;
    CHECK(!Load(Write(w), s), "time going backwards");
    w = SteadyLap();
    w.ref[50].pos = 0.1f;
    CHECK(!Load(Write(w), s), "position going backwards");
    w = SteadyLap();
    w.ref[100].pos = 1.2f;
    CHECK(!Load(Write(w), s), "position past the lap");
    w = SteadyLap();
    w.ref[3].x = NAN;
    CHECK(!Load(Write(w), s), "not a number");
    w = SteadyLap();
    w.ref.assign(2001, RefPoint{});
    CHECK(!Load(Write(w), s), "too many points");
    w = SteadyLap();
    w.sections.push_back({300, 200, "Backwards", ""});
    CHECK(!Load(Write(w), s), "a section that ends before it starts");
    w = SteadyLap();
    w.sections.push_back({900, 1200, "Past the end", ""});
    CHECK(!Load(Write(w), s), "a section past the end of the track");
    w = SteadyLap();
    w.sections.assign(65, Section{0, 10, "x", ""});
    CHECK(!Load(Write(w), s), "too many sections");
    w = SteadyLap();
    w.ref.clear();
    CHECK(Load(Write(w), s) && s.ref.empty(), "no reference lap is fine");
    std::vector<uint8_t> more = ok;
    more.push_back(0xEE);
    CHECK(Load(more, s), "bytes after the flags are ignored");
}

static void TextIsMadeSafeToDraw() {
    Sheet w = SteadyLap();
    w.sections.push_back({500, 600, "Caf\xC3\xA9 corner", "Tab\there \xE2\x80\x94 now"});
    w.sections.push_back({100, 200, "First", ""});
    Sheet s;
    CHECK(Load(Write(w), s), "parse");
    CHECK(s.sections[0].name == "First", "sections by start");
    CHECK(s.sections[1].name == "Caf? corner", "name %s", s.sections[1].name.c_str());
    CHECK(s.sections[1].tip == "Tab here ? now", "tip %s", s.sections[1].tip.c_str());
    CHECK(SectionAt(s, 150) == &s.sections[0] && SectionAt(s, 599) == &s.sections[1], "section lookup");
    CHECK(!SectionAt(s, 200) && !SectionAt(s, 50), "between sections");
}

static void NamesAndFits() {
    coachcue::Event e;
    e.track     = "indiana nationals";
    e.bike      = "MX2OEM_2023_KTM_250_SX-F";
    e.track_len = 1650;
    const std::vector<std::string> n = HudNames(e);
    CHECK(n.size() == 2 && n[0] == "indiana_nationals.MX2OEM_2023_KTM_250_SX-F.hud" && n[1] == "indiana_nationals.hud",
          "names %s", n.empty() ? "" : n[0].c_str());
    Sheet s;
    s.track_len = 1650.5f;
    CHECK(Fits(s, e), "same length");
    s.track_len = 1400;
    CHECK(!Fits(s, e), "another track");
}

// Rides `laps` laps of a 1000 m track at `speed` m/s, 50 Hz, from 300 m before the line.
template <typename F>
static void Ride(float speed, int laps, F each) {
    float t = 0;
    for (float m = 700; m < 1000.0f * (laps + 1); m += speed * 0.02f, t += 0.02f) each(t, std::fmod(m, 1000.0f) / 1000.0f);
}

static void TheGapToCoachsLap() {
    Sheet s;
    Load(Write(SteadyLap()), s);
    RefLap ref;
    ref.load(s.ref);
    float rt = 0;
    CHECK(ref.time_at(0.5f, rt) && std::fabs(rt - 50) < 1e-3f, "Coach at halfway: %f", rt);
    CHECK(ref.time_at(0.1234f, rt) && std::fabs(rt - 12.34f) < 1e-3f, "between slots: %f", rt);

    // 5% slower than Coach: 9.5238 m/s, so 105 s a lap.
    LapClock clock;
    bool before = false;
    float at_half = 0, at_end = 0;
    Ride(1000.0f / 105.0f, 1, [&](float t, float pos) {
        clock.on_sample(t, pos);
        float g;
        const bool has = Gap(ref, clock, t, pos, g);
        if (!has && t < 30) before = true;  // mid-lap: nothing until the line
        if (has && pos > 0.499f && pos < 0.501f) at_half = g;
        if (has && pos > 0.989f && pos < 0.991f) at_end = g;
    });
    CHECK(before, "no gap before the first crossing");
    CHECK(std::fabs(at_half - 2.5f) < 0.03f, "2.5 s down at halfway: %f", at_half);
    CHECK(std::fabs(at_end - 4.95f) < 0.03f, "4.95 s down at the end: %f", at_end);

    // Faster than Coach: ahead, a negative gap.
    clock.reset();
    float fast = 0;
    Ride(1000.0f / 90.0f, 1, [&](float t, float pos) {
        clock.on_sample(t, pos);
        float g;
        if (Gap(ref, clock, t, pos, g) && pos > 0.499f && pos < 0.501f) fast = g;
    });
    CHECK(std::fabs(fast + 5.0f) < 0.03f, "5 s up at halfway: %f", fast);
}

static void TheClockRestartsAtTheLine() {
    LapClock c;
    c.on_sample(10.00f, 0.98f);
    CHECK(!c.valid(), "joined mid-lap");
    c.on_sample(10.02f, 0.99f);
    c.on_sample(10.04f, 0.01f);  // crossed halfway between the two samples
    CHECK(c.valid() && std::fabs(c.elapsed(10.04f) - 0.01f) < 1e-4f, "elapsed %f", c.elapsed(10.04f));
    c.on_sample(12.0f, 0.2f);
    CHECK(std::fabs(c.elapsed(12.0f) - 1.97f) < 1e-4f, "track time");
    c.on_sample(12.1f, 0.98f);  // back over the line
    CHECK(!c.valid(), "riding backwards over the line");
    c.on_sample(12.2f, 0.99f);
    c.on_sample(12.3f, 0.02f);
    CHECK(c.valid(), "the next crossing");
    c.on_sample(1.0f, 0.03f);
    CHECK(!c.valid(), "time went back: a new stint");
}

static void TheGhost() {
    RefLap ref;
    ref.load(SteadyLap().ref);
    float x = 0, z = 0;
    CHECK(ref.world_at(0, x, z) && x == 5 && z == 7, "at the line");
    CHECK(ref.world_at(42.5f, x, z) && std::fabs(z - 432) < 1e-2f, "42.5 s in: z %f", z);
    CHECK(ref.world_at(100, x, z) && std::fabs(z - 1007) < 1e-2f, "the end");
    CHECK(!ref.world_at(100.5f, x, z), "Coach has finished");
    CHECK(!ref.world_at(-1, x, z), "before the lap");
    RefLap none;
    none.load({});
    CHECK(!none.ready() && !none.world_at(1, x, z) && !none.time_at(0.5f, x), "no reference");
}

static void HudIni() {
    Settings s = ParseSettings("", false);
    CHECK(s.enabled && s.cue && s.section && s.gap && s.stance && s.map && s.setup, "no file: all on");
    s = ParseSettings("", true);
    CHECK(!s.map && s.cue, "MXBMRP3 installed: no map by default");
    s = ParseSettings("[hud]\r\nmap=1\r\n", true);
    CHECK(s.map, "map=1 wins over MXBMRP3");
    s = ParseSettings("[hud]\nenabled=1\ncue=1\nsection=0\ngap=0\nstance=1\nmap=0\nsetup=0\n", false);
    CHECK(s.enabled && s.cue && !s.section && !s.gap && s.stance && !s.map && !s.setup, "each key");
    s = ParseSettings("[other]\ncue=0\n", false);
    CHECK(s.cue, "another section's key");
    s = ParseSettings("[hud]\ncue=maybe\n", false);
    CHECK(s.cue, "a value that isn't 0 or 1 keeps the default");

    // The two new parts are off until they are asked for: a HUD that grows things on its own
    // after an update is a worse surprise than one that waits.
    s = ParseSettings("", false);
    CHECK(!s.susp && !s.trail, "suspension and trail are off by default");
    CHECK(s.cue_x == kCueDefaultX && s.cue_y == kCueDefaultY, "the cue box starts where it always was");
    s = ParseSettings("[hud]\nsusp=1\ntrail=1\ncue_x=0.2\ncue_y=0.75\n", false);
    CHECK(s.susp && s.trail, "switched on");
    CHECK(std::fabs(s.cue_x - 0.2f) < 1e-6f && std::fabs(s.cue_y - 0.75f) < 1e-6f, "cue at %f %f", s.cue_x, s.cue_y);
    // A value that isn't a fraction keeps the default rather than putting the cue somewhere it
    // can't be read.
    s = ParseSettings("[hud]\ncue_x=middle\ncue_y=2.5\n", false);
    CHECK(s.cue_x == kCueDefaultX && s.cue_y == kCueDefaultY, "junk and out of range keep the default");
    s = ParseSettings("[hud]\ncue_x=-0.5\ncue_y=0.4x\n", false);
    CHECK(s.cue_x == kCueDefaultX && s.cue_y == kCueDefaultY, "negative, and trailing rubbish");
    s = ParseSettings("[hud]\ncue_x=0\ncue_y=1\n", false);
    CHECK(s.cue_x == 0.0f && s.cue_y == 1.0f, "the ends of the range are values, not junk");
}

// Where the cue block goes, now the rider can move it off the middle of the screen.
static void TheCueBoxMoves() {
    const Box d = CueBoxAt(kCueDefaultX, kCueDefaultY);
    CHECK(std::fabs(d.x0 - kCueBox.x0) < 1e-6f && std::fabs(d.y0 - kCueBox.y0) < 1e-6f &&
              std::fabs(d.x1 - kCueBox.x1) < 1e-6f && std::fabs(d.y1 - kCueBox.y1) < 1e-6f,
          "the default is exactly where it has always been: %f %f %f %f", d.x0, d.y0, d.x1, d.y1);
    const Box ds = SectionBoxAt(d);
    CHECK(std::fabs(ds.x0 - kSectionBox.x0) < 1e-6f && std::fabs(ds.y0 - kSectionBox.y0) < 1e-6f &&
              std::fabs(ds.y1 - kSectionBox.y1) < 1e-6f,
          "and so is the section line: %f %f", ds.y0, ds.y1);

    // Moved: same size, and the section follows it rather than staying behind.
    const Box m = CueBoxAt(0.25f, 0.70f);
    CHECK(std::fabs((m.x1 - m.x0) - kCueWidth) < 1e-6f && std::fabs((m.y1 - m.y0) - kCueHeight) < 1e-6f, "same size");
    CHECK(std::fabs(m.x0 - 0.10f) < 1e-6f && std::fabs(m.y0 - 0.70f) < 1e-6f, "at %f %f", m.x0, m.y0);
    CHECK(SectionBoxAt(m).y0 > m.y1 && SectionBoxAt(m).x0 == m.x0, "the section follows it down");

    // Pushed at the edges it stops at the edge, with the section still on screen under it.
    const Box tl = CueBoxAt(0.0f, 0.0f);
    CHECK(tl.x0 >= -1e-6f && tl.y0 >= -1e-6f, "top left: %f %f", tl.x0, tl.y0);
    const Box br = CueBoxAt(1.0f, 1.0f);
    CHECK(br.x1 <= 1.0f + 1e-6f, "right edge: %f", br.x1);
    CHECK(SectionBoxAt(br).y1 <= 1.0f + 1e-6f, "the section stays on screen: %f", SectionBoxAt(br).y1);
}

// How much travel an end is using. The published header says only "shocks length", which does
// not say which way it runs; this is MXBMRP3's reading of it.
static void SuspensionTravel() {
    CHECK(SuspUsed(0.30f, 0.30f) == 0.0f, "at full length, none of it is used");
    CHECK(SuspUsed(0.0f, 0.30f) == 1.0f, "at no length, all of it");
    CHECK(std::fabs(SuspUsed(0.15f, 0.30f) - 0.5f) < 1e-6f, "half way");
    CHECK(SuspUsed(0.40f, 0.30f) == 0.0f, "longer than its travel is clamped");
    CHECK(SuspUsed(-0.1f, 0.30f) == 1.0f, "past the stop is clamped");
    CHECK(SuspUsed(0.15f, 0.0f) == 0.0f, "no travel means nothing to show a proportion of");
    CHECK(SuspUsed(NAN, 0.3f) == 0.0f && SuspUsed(0.1f, NAN) == 0.0f, "not a number");
}

// A circle of radius 100 m in four quarter curves, anticlockwise from (0, -100) heading east.
static std::vector<uint8_t> Circle(float radius_sign) {
    Out o;
    const float quarter = 3.14159265f * 100 * 0.5f;
    for (int i = 0; i < 4; ++i) {
        o.u32(1);
        o.f32(quarter);
        o.f32(-100.0f * radius_sign);
        o.f32(90);
        o.f32(0);
        o.f32(-100);
        o.f32(0);
    }
    return o.b;
}

static void TheMap() {
    const std::vector<uint8_t> segs = Circle(1);
    const float race[4] = {157.08f, 0, 0, 0};  // the line a quarter of the way round
    Track t;
    CHECK(t.build(4, segs.data(), 28, race), "build");
    CHECK(std::fabs(t.length() - 628.32f) < 0.1f, "length %f", t.length());
    CHECK(t.line().size() >= 201 && t.line().size() <= 401, "%zu points", t.line().size());
    const Pt a = t.line().front(), e = t.line().back();
    CHECK(std::hypot(a.x - e.x, a.y - e.y) < 0.05f, "closes on itself");
    const float w = t.hi().x - t.lo().x, h = t.hi().y - t.lo().y;
    CHECK(std::fabs(w - 220) < 1 && std::fabs(h - 220) < 1, "bounds plus 5%%: %f x %f", w, h);
    float x, z;
    CHECK(t.at(0, x, z) && std::hypot(x - 0, z - 0) > 99 && std::hypot(x, z) < 101, "the line is on the circle");
    // Negative radius is a left curve: from heading east at (0,-100), a quarter left is (100, 0).
    CHECK(t.at(-157.08f, x, z) && std::fabs(x - 0) < 0.1f && std::fabs(z + 100) < 0.1f, "start of the data");
    CHECK(t.at(0, x, z) && std::fabs(x - 100) < 0.1f && std::fabs(z - 0) < 0.1f, "S/F at (100, 0): %f %f", x, z);
    CHECK(t.at(628.32f, x, z) && std::fabs(x - 100) < 0.2f, "a lap on wraps to the line");

    // Fitted in its box, square: equal extents map to equal heights and aspect-corrected widths.
    const Pt l = Project(t, kMapBox, t.lo().x, t.lo().y), r = Project(t, kMapBox, t.hi().x, t.hi().y);
    CHECK(l.x >= kMapBox.x0 - 1e-4f && r.x <= kMapBox.x1 + 1e-4f && r.y >= kMapBox.y0 - 1e-4f && l.y <= kMapBox.y1 + 1e-4f,
          "inside the box: %f %f %f %f", l.x, r.x, r.y, l.y);
    CHECK(std::fabs((r.x - l.x) * kAspect - (l.y - r.y)) < 1e-4f, "square after aspect correction");
    CHECK(r.y < l.y, "north up");

    CHECK(!t.build(0, segs.data(), 28, nullptr) && !t.ready(), "no segments");
    CHECK(!t.build(4, segs.data(), 16, nullptr), "a stride too short to hold a segment");
    CHECK(t.build(4, segs.data(), 28, nullptr) && t.start_line() == 0, "no race data: the line at 0");
}

static void UpcomingCues() {
    coachcue::Sheet s;
    for (float m : {100.f, 300.f, 500.f, 900.f}) {
        coachcue::Cue c;
        c.at_m = m;
        s.cues.push_back(c);
    }
    std::vector<float> u = Upcoming(s, 400, 3);
    CHECK(u.size() == 3 && u[0] == 500 && u[1] == 900 && u[2] == 100, "wraps past the line");
    CHECK(Upcoming(s, 950, 2) == std::vector<float>({100, 300}), "near the end");
    CHECK(Upcoming(coachcue::Sheet{}, 0, 3).empty(), "no cues");
}

static void Stopped() {
    StopWatch w;
    w.on_sample(0, 10);
    w.on_sample(1, 0.3f);
    w.on_sample(1.9f, 0.2f);
    CHECK(!w.stopped(), "under a second");
    w.on_sample(2.1f, 0.1f);
    CHECK(w.stopped(), "over a second");
    w.on_sample(2.2f, 3);
    CHECK(!w.stopped(), "moving again");
}

static void SetupNameFromTheSession() {
    std::vector<uint8_t> ses(112, 0);
    std::memcpy(ses.data() + 12, "setups\\sand fast.stp", 20);
    CHECK(SetupName(ses.data(), int(ses.size())) == "sand fast.stp", "%s", SetupName(ses.data(), 112).c_str());
    CHECK(SetupName(ses.data(), 12).empty() && SetupName(nullptr, 0).empty(), "short");
}

static bool Within(const Frame& f, const Box& b) {
    for (const Quad& q : f.quads)
        for (auto& p : q.p)
            if (p[0] < b.x0 - 1e-4f || p[0] > b.x1 + 1e-4f || p[1] < b.y0 - 1e-4f || p[1] > b.y1 + 1e-4f) return false;
    return true;
}

static void TheLayout() {
    coachcue::Cue cue;
    cue.text = "Brake";
    cue.kind = coachcue::BRAKE;
    Section sec{0, 10, "Whoops", std::string(80, 'x')};
    const std::vector<uint8_t> segs = Circle(1);
    Track t;
    t.build(4, segs.data(), 28, nullptr);

    View v;
    v.cue     = &cue;
    v.section = &sec;
    v.has_gap = true;
    v.gap     = 0.34f;
    v.stance  = stance::SIT;
    v.conf    = stance::CONF_SURE;
    v.track   = &t;
    v.has_rider = v.has_ghost = true;
    v.rider = {100, 0};
    v.ghost = {0, 100};
    v.upcoming = {10, 200};
    v.stopped  = true;
    v.setup    = "sand fast.stp";
    v.sag      = true;

    Frame f;
    Build(v, f);
    std::vector<std::string> said;
    for (const Text& x : f.texts) {
        said.push_back(x.s);
        CHECK(x.s.size() <= kMaxChars, "%zu chars", x.s.size());
    }
    CHECK(said.size() == 6, "cue, section, gap, stance, setup, sag: %zu", said.size());
    CHECK(said[0] == "Brake" && f.texts[0].color == kRed && f.texts[0].justify == 1, "the cue");
    CHECK(f.texts[0].y > kCueBox.y0 && f.texts[0].y + kCueSize <= kCueBox.y1 + 1e-4f, "cue inside its box");
    CHECK(said[1].size() * kCharWidth * kSmallSize <= kSectionBox.x1 - kSectionBox.x0, "the tip is cut to fit");
    CHECK(said[1].compare(0, 8, "Whoops: ") == 0 && said[1].substr(said[1].size() - 3) == "...", "%s", said[1].c_str());
    CHECK(said[2] == "vs Coach +0.34" && f.texts[2].color == kRed, "gap %s", said[2].c_str());
    CHECK(said[3] == "SIT", "stance");
    CHECK(f.texts[3].x > f.texts[2].x + TextWidth(said[2].size(), kSmallSize), "stance beside the gap");
    CHECK(said[4] == "Setup: sand fast.stp" && said[5] == "Stop 2 seconds in neutral to measure sag", "card");
    // The line and the trail are thinned to what a map this size can show, so the whole HUD
    // now costs a fraction of the cap — and the reserve below is never touched by ordinary
    // drawing.
    CHECK(f.quads.size() > 40 && f.quads.size() <= kMaxQuads - kReserve, "%zu quads", f.quads.size());

    // Each part off in turn, and the map on its own stays in its box.
    v.set = ParseSettings("[hud]\ncue=0\nsection=0\ngap=0\nstance=0\nsetup=0\n", false);
    Build(v, f);
    CHECK(f.texts.empty() && Within(f, kMapBox), "only the map, in its box");
    v.set = ParseSettings("[hud]\nenabled=0\n", false);
    Build(v, f);
    CHECK(f.quads.empty() && f.texts.empty(), "all off");

    // What hides itself.
    v.set     = Settings{};
    v.stance  = stance::STANCE_UNKNOWN;
    v.stopped = false;
    v.gap     = -0.5f;
    v.cue = nullptr, v.section = nullptr, v.track = nullptr;
    Build(v, f);
    CHECK(f.texts.size() == 1 && f.texts[0].s == "vs Coach -0.50" && f.texts[0].color == kGreen, "ahead, no stance");
    v.stance = stance::STAND;
    v.conf   = stance::CONF_NONE;
    v.has_gap = false;
    Build(v, f);
    CHECK(f.texts.empty() && f.quads.empty(), "no confidence: no stance");
}

// The suspension bars and the blue line to take: both new, both off unless asked for.
static void TheSuspensionAndTheTrail() {
    const std::vector<uint8_t> segs = Circle(1);
    Track t;
    t.build(4, segs.data(), 28, nullptr);
    Sheet sheet;
    CHECK(Load(Write(SteadyLap()), sheet), "a reference lap to draw");

    View v;
    v.set         = ParseSettings("[hud]\ncue=0\nsection=0\ngap=0\nstance=0\nsetup=0\nsusp=1\ntrail=1\n", false);
    v.track       = &t;
    v.has_rider   = true;
    v.rider       = {100, 0};
    v.pos         = 0.0f;
    v.ref         = &sheet.ref;
    v.has_susp    = true;
    v.susp[0]     = 0.5f;
    v.susp[1]     = 0.25f;
    v.susp_max[0] = 0.9f;
    v.susp_max[1] = 0.4f;

    Frame f;
    Build(v, f);
    CHECK(f.quads.size() <= kMaxQuads, "%zu quads fit", f.quads.size());
    size_t in_susp = 0;
    for (const Quad& q : f.quads) {
        if (q.p[0][0] >= kSuspBox.x0 - 1e-3f && q.p[2][0] <= kSuspBox.x1 + 1e-3f &&
            q.p[0][1] >= kSuspBox.y0 - 0.01f && q.p[2][1] <= kSuspBox.y1 + 0.01f)
            ++in_susp;
    }
    CHECK(in_susp >= 7, "backing, two tracks, two fills, two bottomed marks: %zu", in_susp);
    std::vector<std::string> said;
    for (const Text& x : f.texts) said.push_back(x.s);
    CHECK(said.size() == 2 && said[0] == "F" && said[1] == "R", "both ends are labelled");
    const size_t with_trail = f.quads.size();

    // No sheet, no trail. Nothing is guessed from the centreline, because a confidently drawn
    // wrong line is worse than no line at all.
    v.ref = nullptr;
    Build(v, f);
    CHECK(f.quads.size() < with_trail, "no reference, no trail: %zu vs %zu", f.quads.size(), with_trail);
    const size_t without_trail = f.quads.size();
    Sheet empty;
    v.ref = &empty.ref;
    Build(v, f);
    CHECK(f.quads.size() == without_trail, "an empty reference draws no trail either");

    // The trail goes with the rider round the lap, and is about as long wherever they are. Not
    // exactly as long: the sheet's points sit 1% of a lap apart, so whether the one on the
    // horizon falls just inside it or just outside is a rounding question, not a rule.
    v.ref                      = &sheet.ref;
    const size_t at_the_line   = with_trail - without_trail;
    v.pos                      = 0.5f;
    Build(v, f);
    const size_t at_half = f.quads.size() - without_trail;
    CHECK(at_half + 1 >= at_the_line && at_half <= at_the_line + 1, "%zu segments at half a lap, %zu at the line",
          at_half, at_the_line);
    CHECK(at_the_line > 5, "the trail is actually drawn: %zu segments", at_the_line);
    // And it carries on across the line instead of stopping dead at it.
    v.pos = 0.95f;
    Build(v, f);
    CHECK(f.quads.size() > without_trail, "the trail carries on past the line");

    // The bike said nothing about its travel: no bars, rather than bars of unknown scale.
    v.pos      = 0;
    v.has_susp = false;
    Build(v, f);
    for (const Text& x : f.texts) CHECK(x.s != "F" && x.s != "R", "no bars without a travel to measure against");
    // And they stay away unless hud.ini asks.
    v.has_susp = true;
    v.set.susp = false;
    Build(v, f);
    CHECK(f.texts.empty(), "susp=0 draws none of it");
    v.set.susp  = true;  // back on, so this is a test of the trail key and nothing else
    v.set.trail = false;
    Build(v, f);
    CHECK(f.quads.size() == without_trail, "trail=0 draws none of it");
}

// A sheet written to disk and read back, as the plugin does. TMPDIR, then TEMP, then /tmp.
static void FromAFile() {
    const char* tmp = std::getenv("TMPDIR");
    if (!tmp) tmp = std::getenv("TEMP");
    if (!tmp) tmp = "/tmp";
    const std::string path = std::string(tmp) + "/coachhud_test.hud";
    const std::vector<uint8_t> b = Write(SteadyLap());
    std::FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr, "open %s", path.c_str());
    if (!f) return;
    std::fwrite(b.data(), 1, b.size(), f);
    std::fclose(f);
    std::vector<uint8_t> back(b.size() + 16);
    f = std::fopen(path.c_str(), "rb");
    const size_t n = f ? std::fread(back.data(), 1, back.size(), f) : 0;
    if (f) std::fclose(f);
    std::remove(path.c_str());
    Sheet s;
    CHECK(n == b.size() && Parse(back.data(), n, s) && s.ref.size() == 101, "read back %zu bytes", n);
}


/// The rider's marker says which way they are pointing, and the parts sit where hud.ini puts
/// them rather than in the corner they were first drawn in.
static void TheRiderPointsAndThePartsMove() {
    // An arrow is a triangle: two of its four corners are folded together onto the point.
    Frame f;
    Arrow(f, 0.5f, 0.5f, 1.0f, 0.0f, kWhite, 0.02f);
    CHECK(f.quads.size() == 1, "one quad for the arrow: %zu", f.quads.size());
    const Quad& q = f.quads[0];
    CHECK(q.p[0][0] == q.p[3][0] && q.p[0][1] == q.p[3][1], "folded into a triangle");
    CHECK(q.p[0][0] > q.p[1][0] && q.p[0][0] > q.p[2][0], "pointing the way it was given");
    // Turned a quarter turn, it points that way instead.
    f.clear();
    Arrow(f, 0.5f, 0.5f, 0.0f, 1.0f, kWhite, 0.02f);
    CHECK(f.quads[0].p[0][1] > f.quads[0].p[1][1], "pointing down the screen");
    // Standing still is a dot, not an arrow with no direction.
    f.clear();
    Arrow(f, 0.5f, 0.5f, 0.0f, 0.0f, kWhite, 0.02f);
    CHECK(f.quads.size() == 1 && f.quads[0].p[0][0] != f.quads[0].p[3][0], "a dot when stopped");

    // hud.ini moves the map and the bars, and a part dragged off the edge stays on screen.
    const Settings moved = ParseSettings("[hud]\nsusp=1\nmap_x=0.60\nmap_y=0.10\nsusp_x=0.02\nsusp_y=0.30\n", false);
    CHECK(MapBoxAt(moved).x0 == 0.60f && MapBoxAt(moved).y0 == 0.10f, "the map where it was put");
    CHECK(SuspBoxAt(moved).x0 == 0.02f && SuspBoxAt(moved).y0 == 0.30f, "the bars where they were put");
    const Settings off = ParseSettings("[hud]\nmap_x=0.99\nmap_y=0.99\n", false);
    CHECK(MapBoxAt(off).x1 <= 1.0f + 1e-6f && MapBoxAt(off).y1 <= 1.0f + 1e-6f, "kept on screen");
    // Defaults are the corners it always used, so nobody's HUD moves on updating.
    const Settings plain = Settings{};
    CHECK(MapBoxAt(plain).x0 == kMapBox.x0 && MapBoxAt(plain).y0 == kMapBox.y0, "map unchanged by default");
    CHECK(SuspBoxAt(plain).x0 == kSuspBox.x0 && SuspBoxAt(plain).y0 == kSuspBox.y0, "bars unchanged by default");
    CHECK(plain.move, "right-drag is on unless turned off");
    // The gap line moves too: it was the one part left nailed down, and a rider who drags the
    // map under it had no way to get it out of the way.
    const Settings row = ParseSettings("[hud]\nrow_x=0.20\nrow_y=0.70\n", false);
    CHECK(RowBoxAt(row, kRowHitW).y0 == 0.70f, "the gap line where it was put");
    CHECK(PartAt(row, 0.20f, 0.70f + kRowH * 0.5f) == PART_ROW, "and can be taken hold of there");
    // The pointer: drawn only when asked for, over everything, and on the reserve so a busy
    // frame can never be the reason a rider cannot see what they are aiming.
    {
        View pv;
        pv.set = ParseSettings("[hud]\n", false);
        Frame pf;
        Build(pv, pf);
        const size_t without = pf.quads.size();
        pv.has_pointer = true;
        pv.pointer     = {0.42f, 0.61f};
        Build(pv, pf);
        CHECK(pf.quads.size() == without + 2, "a pointer is two quads: %zu", pf.quads.size() - without);
        const Quad& tip = pf.quads.back();
        CHECK(tip.p[0][0] == 0.42f && tip.p[0][1] == 0.61f, "its point is where the mouse is");
        CHECK(tip.p[0][0] == tip.p[3][0] && tip.p[0][1] == tip.p[3][1], "folded into a triangle");
    }

    Settings put = Settings{};
    SetPartOrigin(put, PART_ROW, 0.30f, 0.55f);
    const Box moved_row = RowBoxAt(put, kRowHitW);
    CHECK(std::fabs(moved_row.x0 - 0.30f) < 1e-5f && moved_row.y0 == 0.55f, "dragged by its corner");
    CHECK(!ParseSettings("[hud]\nmove=0\n", false).move, "and can be turned off");
}

/// Whatever else runs out of room, the rider can see themselves. The cap used to be spent by
/// the centreline and the trail, and the rider's own marker is drawn last.
static void TheRidersMarkerIsNeverDropped() {
    Frame f;
    while (f.room()) Rect(f, 0, 0, 0.01f, 0.01f, kWhite);
    const size_t filled = f.quads.size();
    CHECK(filled == kMaxQuads - kReserve, "ordinary drawing stops at the reserve: %zu", filled);
    Rect(f, 0, 0, 0.01f, 0.01f, kWhite);
    CHECK(f.quads.size() == filled, "and stays stopped");
    {
        Reserved hold(f);
        Arrow(f, 0.5f, 0.5f, 1.0f, 0.0f, kWhite, 0.02f);
        CHECK(f.quads.size() == filled + 1, "the reserve draws the rider anyway");
    }
    Rect(f, 0, 0, 0.01f, 0.01f, kWhite);
    CHECK(f.quads.size() == filled + 1, "and the reserve closes again");
}


/// Right-drag a part to move it. Everything here is decided without Windows, so it is tested:
/// what is under the cursor, where it lands, and that hud.ini keeps what Coach wrote in it.
static void DraggingAPart() {
    Settings s = ParseSettings("[hud]\nsusp=1\n", false);
    // The cursor over each part finds that part, and empty screen finds none.
    const Box map = MapBoxAt(s), susp = SuspBoxAt(s), cue = CueBoxAt(s.cue_x, s.cue_y);
    CHECK(PartAt(s, (map.x0 + map.x1) * 0.5f, (map.y0 + map.y1) * 0.5f) == PART_MAP, "the map");
    CHECK(PartAt(s, (susp.x0 + susp.x1) * 0.5f, (susp.y0 + susp.y1) * 0.5f) == PART_SUSP, "the bars");
    CHECK(PartAt(s, (cue.x0 + cue.x1) * 0.5f, (cue.y0 + cue.y1) * 0.5f) == PART_CUE, "the cue");
    CHECK(PartAt(s, 0.5f, 0.5f) == PART_NONE, "nothing in the middle of the screen");
    // A part that isn't drawn can't be grabbed.
    Settings off = ParseSettings("[hud]\nsusp=0\nmap=0\n", false);
    CHECK(PartAt(off, (map.x0 + map.x1) * 0.5f, (map.y0 + map.y1) * 0.5f) == PART_NONE, "the map is off");

    // Dragged to the middle, it is there — and the cue keeps being stored by its centre.
    SetPartOrigin(s, PART_MAP, 0.40f, 0.20f);
    CHECK(MapBoxAt(s).x0 == 0.40f && MapBoxAt(s).y0 == 0.20f, "the map moved");
    SetPartOrigin(s, PART_CUE, 0.10f, 0.60f);
    const Box moved = CueBoxAt(s.cue_x, s.cue_y);
    CHECK(std::fabs(moved.x0 - 0.10f) < 1e-5f && std::fabs(moved.y0 - 0.60f) < 1e-5f, "the cue moved by its corner");

    // hud.ini: the moved keys are written, and everything else survives untouched.
    const std::string before = "[hud]\nenabled=1\ncue=1\nmap_x=0.010\ntrail=1\n\n[other]\nkeep=me\n";
    const std::string after  = WithHudKeys(before, PartKeys(s, PART_MAP));
    CHECK(after.find("map_x=0.400") != std::string::npos, "the key it had was rewritten: %s", after.c_str());
    CHECK(after.find("map_y=0.200") != std::string::npos, "the key it lacked was added");
    CHECK(after.find("trail=1") != std::string::npos && after.find("enabled=1") != std::string::npos, "Coach's keys kept");
    CHECK(after.find("[other]") != std::string::npos && after.find("keep=me") != std::string::npos, "other sections kept");
    CHECK(after.find("map_x=0.010") == std::string::npos, "the old value is gone, not left beside the new one");
    // Read back, it is what was written.
    const Settings round = ParseSettings(after, false);
    CHECK(round.map_x == 0.400f && round.map_y == 0.200f, "what was written is what is read");
    CHECK(round.trail, "and the rest of the file still means what it did");
    // No [hud] section at all: one is made rather than the keys being dropped.
    const std::string fresh = WithHudKeys("[other]\nkeep=me\n", PartKeys(s, PART_SUSP));
    CHECK(ParseSettings(fresh, false).susp_x == s.susp_x, "a new [hud] section carries them");
    CHECK(fresh.find("keep=me") != std::string::npos, "without losing what was there");
}

int main() {
    TheBytesAreExact();
    RefusesWhatTheAppDoesNotWrite();
    TextIsMadeSafeToDraw();
    NamesAndFits();
    TheGapToCoachsLap();
    TheClockRestartsAtTheLine();
    TheGhost();
    HudIni();
    TheCueBoxMoves();
    SuspensionTravel();
    TheMap();
    UpcomingCues();
    Stopped();
    SetupNameFromTheSession();
    TheLayout();
    TheSuspensionAndTheTrail();
    FromAFile();
    TheRiderPointsAndThePartsMove();
    TheRidersMarkerIsNeverDropped();
    DraggingAPart();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("coachhud: all checks passed\n");
    return 0;
}
