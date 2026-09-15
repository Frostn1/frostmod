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
    CHECK(f.quads.size() > 300 && f.quads.size() <= kMaxQuads, "%zu quads", f.quads.size());

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

int main() {
    TheBytesAreExact();
    RefusesWhatTheAppDoesNotWrite();
    TextIsMadeSafeToDraw();
    NamesAndFits();
    TheGapToCoachsLap();
    TheClockRestartsAtTheLine();
    TheGhost();
    HudIni();
    TheMap();
    UpcomingCues();
    Stopped();
    SetupNameFromTheSession();
    TheLayout();
    FromAFile();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("coachhud: all checks passed\n");
    return 0;
}
