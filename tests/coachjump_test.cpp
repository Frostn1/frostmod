// Jump calls (src/coachjump.h) and the marks they draw (src/coachmark.h), on synthetic terrain:
// singles, doubles, triples and quads in a rhythm section, a table jumped on and off, faces rolled,
// flights predicted for a sheet without the lap's air, and the AIRH chunk read from a sheet.
// Pure C++, runs anywhere.

#include "../src/coachjump.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
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

using namespace coachjump;

// ---------------------------------------------------------------------------------------
// Synthetic track: a straight along +x, a point every half metre, ridden at a steady speed.

// A supercross jump: a 3 m face up to `h`, a 5 m back side down again.
static float Mound(float s, float crest, float h = 1.6f) {
    if (s >= crest - 3.0f && s <= crest) return h * (s - (crest - 3.0f)) / 3.0f;
    if (s > crest && s <= crest + 5.0f) return h * (1.0f - (s - crest) / 5.0f);
    return 0.0f;
}
// A table: a 3 m face up to `h`, flat from `top0` to `top1`, a 4 m down side.
static float Table(float s, float top0, float top1, float h = 1.5f) {
    if (s >= top0 - 3.0f && s < top0) return h * (s - (top0 - 3.0f)) / 3.0f;
    if (s >= top0 && s <= top1) return h;
    if (s > top1 && s <= top1 + 4.0f) return h * (1.0f - (s - top1) / 4.0f);
    return 0.0f;
}

struct Air {
    float from, to;  // metres along: airborne from just after `from` to just before `to`
};

/// A sheet over `len` metres: ground from `g`, the lap in the air over `air`, at `v` m/s. With
/// `with_air` false there is no AIRH chunk (an older sheet); with `with_drive` false no DRIV.
static coachhud::Sheet Sheet(float len, const std::function<float(float)>& g, const std::vector<Air>& air, float v = 12.0f,
                             bool with_air = true, bool with_drive = true) {
    coachhud::Sheet s;
    s.track_len = len;
    const int n = int(len / 0.5f) + 1;
    for (int i = 0; i < n; ++i) {
        const float m = float(i) * 0.5f;
        s.ref.push_back({m / len, m / v, m, 0.0f});
    }
    s.terrain_k = 3, s.terrain_step = 0.5f;
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) s.terrain.push_back(g(float(i) * 0.5f));
    if (with_drive)
        for (int i = 0; i < n; ++i) s.drive.insert(s.drive.end(), {v, 1.0f, 0.0f});
    if (with_air)
        for (int i = 0; i < n; ++i) {
            const float m  = float(i) * 0.5f;
            bool        up = false;
            for (const Air& a : air) up = up || (m > a.from && m < a.to);
            const float above = up ? 2.0f : 0.6f;  // the bike's origin rides 0.6 m up
            s.air.insert(s.air.end(), {g(m) + above, above, up ? 1.0f : 0.0f});
        }
    return s;
}

static std::vector<Call> CallsOf(const coachhud::Sheet& s) { return Calls(MakeLine(s)); }

static std::vector<Call> Jumps(const std::vector<Call>& c) {
    std::vector<Call> out;
    for (const Call& x : c)
        if (x.kind != ROLL) out.push_back(x);
    return out;
}

static void TestRhythm() {
    // One to four jumps eight metres apart, cleared in one flight each time.
    struct Case {
        int         jumps;
        float       land;
        Kind        want;
        const char* name;
    } cases[] = {{1, 55.0f, SINGLE, "single"}, {2, 63.0f, DOUBLE, "double"}, {3, 71.0f, TRIPLE, "triple"},
                 {4, 79.0f, QUAD, "quad"}};
    for (const Case& c : cases) {
        const int jumps = c.jumps;
        auto g = [jumps](float s) {
            float h = 0;
            for (int k = 0; k < jumps; ++k) h = (std::max)(h, Mound(s, 50.0f + 8.0f * float(k)));
            return h;
        };
        const auto calls = CallsOf(Sheet(150, g, {{50.0f, c.land}}));
        CHECK(calls.size() == 1, "%s: %zu calls", c.name, calls.size());
        if (calls.size() != 1) continue;
        CHECK(calls[0].kind == c.want, "%s: called %s", c.name, KindName(calls[0].kind));
        CHECK(calls[0].faces == c.jumps, "%s: %d faces", c.name, calls[0].faces);
        CHECK(std::fabs(calls[0].takeoff_s - 50.0f) < 0.6f, "%s: takeoff at %.1f", c.name, calls[0].takeoff_s);
        CHECK(std::fabs(calls[0].landing_s - c.land) < 0.6f, "%s: landing at %.1f", c.name, calls[0].landing_s);
        CHECK(!calls[0].predicted, "%s: from the lap's own air", c.name);
        CHECK(std::fabs(calls[0].speed - 12.0f) < 1e-3f, "%s: takeoff speed %.2f", c.name, calls[0].speed);
        CHECK(!calls[0].arc.empty() && calls[0].arc[2] > 1.0f, "%s: the flight's arc", c.name);
    }
    // A double landed short, on the face of the second jump: it cleared one.
    auto two = [](float s) { return (std::max)(Mound(s, 50.0f), Mound(s, 58.0f)); };
    const auto shrt = Jumps(CallsOf(Sheet(150, two, {{50.0f, 56.5f}})));
    CHECK(shrt.size() == 1 && shrt[0].kind == SINGLE, "cased double: %s", shrt.empty() ? "-" : KindName(shrt[0].kind));
    // The rhythm doubled then singled: two calls, in order.
    auto three = [](float s) { return (std::max)({Mound(s, 50.0f), Mound(s, 58.0f), Mound(s, 66.0f)}); };
    const auto ds = CallsOf(Sheet(150, three, {{50.0f, 63.0f}, {66.0f, 71.0f}}));
    CHECK(ds.size() == 2 && ds[0].kind == DOUBLE && ds[1].kind == SINGLE, "double-single: %zu calls", ds.size());
}

static void TestTable() {
    // A jump, then a table: doubled onto its top (JUMP ON), then off its far end (JUMP OFF).
    auto g = [](float s) { return (std::max)(Mound(s, 40.0f), Table(s, 47.0f, 60.0f)); };
    const auto calls = Jumps(CallsOf(Sheet(150, g, {{40.0f, 50.0f}, {60.0f, 67.0f}})));
    CHECK(calls.size() == 2, "table: %zu calls", calls.size());
    if (calls.size() == 2) {
        CHECK(calls[0].kind == JUMP_ON, "onto the table: %s", KindName(calls[0].kind));
        CHECK(calls[1].kind == JUMP_OFF, "off the table: %s", KindName(calls[1].kind));
    }
    // Jumped clean over the whole table: one face cleared, a single.
    auto t = [](float s) { return Table(s, 50.0f, 56.0f); };
    const auto over = Jumps(CallsOf(Sheet(150, t, {{50.0f, 61.0f}})));
    CHECK(over.size() == 1 && over[0].kind == TABLE, "over the table: %s", over.empty() ? "-" : KindName(over[0].kind));
}

static void TestOutdoor() {
    auto one = [](const std::vector<Call>& c) { return c.size() == 1 ? KindName(c[0].kind) : "-"; };
    // A kicker up onto higher ground that goes on: STEP UP.
    auto up = [](float s) {
        float h = s < 47 ? 0.0f : s < 50 ? 0.8f * (s - 47) / 3 : s < 60 ? 0.8f - 0.8f * (s - 50) / 10 : 0.0f;
        if (s >= 55) h = (std::max)(h, (std::min)(2.5f, 2.5f * (s - 55) / 3));
        return h;
    };
    const auto su = Jumps(CallsOf(Sheet(200, up, {{50.0f, 62.0f}})));
    CHECK(su.size() == 1 && su[0].kind == STEP_UP, "step up: %s", one(su));
    // Off flat ground onto much lower ground that goes on (no table: it never rose): STEP DOWN.
    auto down = [](float s) { return s < 50 ? 3.0f : s < 53 ? 3.0f - (s - 50) : 0.0f; };
    const auto sd = Jumps(CallsOf(Sheet(200, down, {{50.0f, 60.0f}})));
    CHECK(sd.size() == 1 && sd[0].kind == STEP_DOWN, "step down: %s", one(sd));
    // A gap jump: a lip, a gap, a separate landing face. DOUBLE, not counted as a rhythm.
    auto gap = [](float s) { return (std::max)(Mound(s, 50.0f, 2.0f), Mound(s, 64.0f, 2.0f)); };
    const auto gd = Jumps(CallsOf(Sheet(200, gap, {{50.0f, 67.0f}})));
    CHECK(gd.size() == 1 && gd[0].kind == DOUBLE, "gap double: %s", one(gd));
    // Skipping off a gentle roller: 0.3 m of lip, 6 m in the air. Not a jump: nothing called.
    auto roller = [](float s) { return 0.3f * std::sin((s - 40.0f) / 20.0f * 3.14159f) * (s > 40 && s < 60); };
    CHECK(Jumps(CallsOf(Sheet(200, roller, {{50.0f, 56.0f}}))).empty(), "a skip off a roller is not called");
    // A short airtime (0.3 s) off a real lip is a hop too.
    auto lip = [](float s) { return Mound(s, 50.0f); };
    CHECK(Jumps(CallsOf(Sheet(200, lip, {{50.0f, 55.0f}}, 18.0f))).empty(), "0.28 s in the air is a hop");
}

static void TestRoll() {
    // Three jumps the lap stayed on the ground over: one ROLL call for the section, at its first lip.
    auto three = [](float s) { return (std::max)({Mound(s, 50.0f), Mound(s, 58.0f), Mound(s, 66.0f)}); };
    const auto calls = CallsOf(Sheet(150, three, {}));
    CHECK(calls.size() == 1 && calls[0].kind == ROLL, "rolled rhythm: %zu calls", calls.size());
    if (!calls.empty()) {
        CHECK(calls[0].faces == 3, "rolled %d faces", calls[0].faces);
        CHECK(std::fabs(calls[0].takeoff_s - 50.0f) < 0.6f, "ROLL at %.1f", calls[0].takeoff_s);
        CHECK(SpeedHint(calls[0]).empty(), "no speed hint on a roll");
    }
    // A bump too small to call: nothing.
    auto bump = [](float s) { return Mound(s, 50.0f, 0.7f); };
    CHECK(CallsOf(Sheet(150, bump, {})).empty(), "a small bump rolled is not called");
    // Doubled, then the third rolled: DOUBLE, and the third face alone is a ROLL.
    const auto mix = CallsOf(Sheet(150, three, {{50.0f, 63.0f}}));
    CHECK(mix.size() == 2 && mix[0].kind == DOUBLE && mix[1].kind == ROLL && mix[1].faces == 1,
          "double then roll: %zu calls", mix.size());
}

static void TestHopsAndBounces() {
    auto one = [](float s) { return Mound(s, 50.0f); };
    // Two metres in the air is a hop.
    CHECK(Jumps(CallsOf(Sheet(150, one, {{50.0f, 52.0f}}))).empty(), "a hop is not a jump");
    // Touched down for a metre and straight back up: one jump.
    auto two = [](float s) { return (std::max)(Mound(s, 50.0f), Mound(s, 58.0f)); };
    const auto b = Jumps(CallsOf(Sheet(150, two, {{50.0f, 56.0f}, {57.0f, 63.0f}})));
    CHECK(b.size() == 1 && b[0].kind == DOUBLE, "bounced double: %zu calls", b.size());
    // Without terrain under the line it's still a jump, just not counted.
    coachhud::Sheet s = Sheet(150, one, {{50.0f, 57.0f}});
    s.terrain.clear(), s.terrain_k = 0;
    const auto j = CallsOf(s);
    CHECK(j.size() == 1 && j[0].kind == JUMP && Label(j[0]) == "JUMP", "no terrain: %zu calls", j.size());
}

static void TestPredicted() {
    // An older sheet: TRRN and DRIV, no AIRH. The flight comes from the lip and the speed.
    auto three = [](float s) { return (std::max)({Mound(s, 50.0f), Mound(s, 58.0f), Mound(s, 66.0f)}); };
    // Off a 28 degree face at 14 m/s: ~17 m, clearing all three.
    const auto fast = CallsOf(Sheet(150, three, {}, 14.0f, false));
    // A prediction places the jump but never names it: JUMP, not TRIPLE (or SINGLE).
    CHECK(!fast.empty() && fast[0].predicted && fast[0].kind == JUMP && Label(fast[0]) == "JUMP", "predicted, unnamed: %s",
          fast.empty() ? "-" : KindName(fast[0].kind));
    if (!fast.empty()) CHECK(fast[0].landing_s > 66.0f && fast[0].landing_s < 74.0f, "lands at %.1f", fast[0].landing_s);
    // At 9 m/s the same: a jump here, and no SINGLE said about it.
    const auto slow = CallsOf(Sheet(150, three, {}, 9.0f, false));
    CHECK(!slow.empty() && slow[0].kind == JUMP, "predicted at 9 m/s: %s", slow.empty() ? "-" : KindName(slow[0].kind));
    for (const Call& c : slow) CHECK(c.kind != SINGLE && c.kind != DOUBLE && c.kind != TRIPLE && c.kind != QUAD, "no counting from a prediction");
    // The same lips with the lap's real air are counted.
    const auto real = CallsOf(Sheet(150, three, {{50.0f, 70.0f}}));
    CHECK(!real.empty() && real[0].kind == TRIPLE && !real[0].predicted, "real air: %s", real.empty() ? "-" : KindName(real[0].kind));
    // A predicted landing runs short of the real one, so only a recorded flight draws its landing box.
    std::vector<coachline::Vert> verts;
    for (int i = -4; i <= 120; ++i) {
        const float x = float(i) * 0.5f;
        verts.push_back({x, 0.0f, 0.35f, x, {1, 1, 1, 1}, x});
        verts.push_back({x, 0.0f, -0.35f, x, {1, 1, 1, 1}, x});
    }
    Call c;
    c.kind = JUMP, c.takeoff = 0, c.landing = 1, c.takeoff_s = 20.0f, c.landing_s = 32.0f, c.airtime = 1.0f;
    const std::vector<float> ahead = {20.0f, 32.0f};
    std::vector<coachmark::Quad> shown, pred;
    Marks(shown, {c}, ahead, verts, false);
    c.predicted = true;
    Marks(pred, {c}, ahead, verts, false);
    CHECK(!pred.empty() && pred.size() < shown.size(), "a predicted jump has its lip but not a landing box: %zu vs %zu", pred.size(),
          shown.size());

    // Crawling: rolled, and with no record of the air, no ROLL is claimed either.
    CHECK(CallsOf(Sheet(150, three, {}, 4.0f, false)).empty(), "crawling: no calls");
    // Without DRIV the speed comes from the lap's own times.
    const auto times = CallsOf(Sheet(150, three, {}, 14.0f, false, false));
    CHECK(!times.empty() && times[0].kind == JUMP, "speed from the times: %s",
          times.empty() ? "-" : KindName(times[0].kind));
}

static void TestLabels() {
    Call c;
    c.kind  = TRIPLE;
    c.speed = 14.0f;
    CHECK(Label(c) == "TRIPLE", "label %s", Label(c).c_str());
    CHECK(SpeedHint(c) == "50 KMH", "hint %s", SpeedHint(c).c_str());
    c.speed = NAN;
    CHECK(SpeedHint(c).empty(), "no speed, no hint");
    // Every character a label can use is in the font.
    for (const char* t : {"SINGLE", "DOUBLE", "TRIPLE", "QUAD", "JUMP ON", "JUMP OFF", "ROLL", "JUMP", "0123456789 KMH"})
        for (const char* p = t; *p; ++p) CHECK(coachmark::Glyph(*p) != nullptr, "glyph '%c'", *p);
    CHECK(coachmark::Glyph('#') == nullptr, "no glyph for '#'");
    // An I is one bar top and bottom and a stem: 7 runs, 7 quads.
    std::vector<coachmark::Quad> q;
    const float base[3] = {0, 0, 0}, right[3] = {1, 0, 0}, col[4] = {1, 1, 1, 1};
    coachmark::Text(q, "I", base, right, 0.7f, col, 0);
    CHECK(q.size() == 7, "I: %zu quads", q.size());
    CHECK(std::fabs(coachmark::TextWidth("TRIPLE", 0.7f) - 3.5f) < 1e-4f, "width %.3f", coachmark::TextWidth("TRIPLE", 0.7f));
}

/// A ribbon along +x from x = 0, rows every half metre, `n` m long: the left edge at z = +0.35.
static std::vector<coachline::Vert> StraightRibbon(float len, float y = 1.0f) {
    std::vector<coachline::Vert> v;
    for (float s = 0; s <= len + 1e-3f; s += 0.5f) {
        v.push_back({s, y, coachline::kHalfWidth, s, {0.15f, 0.85f, 0.2f, 0.7f}});
        v.push_back({s, y, -coachline::kHalfWidth, s, {0.15f, 0.85f, 0.2f, 0.7f}});
    }
    return v;
}

static void TestMarks() {
    const auto        verts = StraightRibbon(60.0f);
    const coachmark::Spot p = coachmark::At(verts, 10.25f);
    CHECK(p.ok && std::fabs(p.x - 10.25f) < 1e-4f && std::fabs(p.z) < 1e-4f, "spot at %.2f,%.2f", p.x, p.z);
    CHECK(std::fabs(p.fx - 1) < 1e-4f && std::fabs(p.lz - 1) < 1e-4f, "frame f=(%.2f,%.2f) l=(%.2f,%.2f)", p.fx, p.fz, p.lx, p.lz);
    CHECK(!coachmark::At(verts, 61.0f).ok && !coachmark::At(verts, -1.0f).ok, "off the ribbon");

    // A double on the line: takeoff 30 m ahead, landing 42 m.
    auto two = [](float s) { return (std::max)(Mound(s, 50.0f), Mound(s, 58.0f)); };
    const coachhud::Sheet sh = Sheet(150, two, {{50.0f, 62.0f}});
    const auto calls = CallsOf(sh);
    CHECK(calls.size() == 1, "%zu calls", calls.size());
    // The rider 20 m along, so the sheet's point at 50 m is 30 m ahead.
    float frac = 0;
    const int ai = coachline::AnchorPoint(sh.ref, 20.0f, 0.0f, 20.0f / 150.0f, frac);
    const std::vector<float> ahead = AheadOf(sh.ref, ai, frac, 70.0f);
    CHECK(ai == 40 && std::fabs(ahead[100] - 30.0f) < 1e-3f, "anchor %d, the lip %.2f m ahead", ai, ahead[100]);
    std::vector<coachmark::Quad> m;
    Marks(m, calls, ahead, verts);
    // The takeoff bar: the dark one and the lit one, flat, across the line at 30 m.
    CHECK(m.size() > 20, "%zu quads", m.size());
    if (m.size() >= 2) {
        float cx = 0;
        for (int c = 0; c < 4; ++c) cx += m[1].p[c][0] * 0.25f;
        CHECK(std::fabs(cx - 30.0f) < 1e-3f, "bar at %.2f", cx);
        CHECK(m[1].p[0][1] > 1.0f && m[1].p[0][1] < 1.05f, "bar on the ribbon: y %.3f", m[1].p[0][1]);
        CHECK(std::fabs(m[1].p[0][2] - m[1].p[2][2]) > 2 * coachline::kHalfWidth, "bar wider than the line");
    }
    // The label stands over the lip, upright, facing back down the line: every corner of a text
    // quad at the lip's x (or its shadow, 6 cm behind), above the ground.
    int text = 0;
    for (const coachmark::Quad& q : m)
        if (q.p[0][1] > 2.0f && std::fabs(q.p[0][0] - q.p[2][0]) < 1e-4f) {
            ++text;
            CHECK(std::fabs(q.p[0][0] - 30.0f) < 0.1f, "label quad at x %.2f", q.p[0][0]);
        }
    CHECK(text > 40, "%d label quads", text);
    // The landing box at 42 m.
    bool landing = false;
    for (const coachmark::Quad& q : m) {
        float cx = 0;
        for (int c = 0; c < 4; ++c) cx += q.p[c][0] * 0.25f;
        if (std::fabs(cx - 42.0f) < 0.01f && q.p[0][1] < 1.1f) landing = true;
    }
    CHECK(landing, "landing box at 42 m");

    // Too close (3 m): no label, the bar still there. Too far (65 m): nothing at all.
    auto count_at = [&](float rider) {
        float f = 0;
        const int a = coachline::AnchorPoint(sh.ref, rider, 0.0f, rider / 150.0f, f);
        std::vector<coachmark::Quad> out;
        Marks(out, calls, AheadOf(sh.ref, a, f, 70.0f), verts);
        int labels = 0;
        for (const coachmark::Quad& q : out)
            labels += q.p[0][1] > 2.0f && q.rgba[3] > 0 && std::fabs(q.p[0][0] - q.p[2][0]) < 1e-4f;
        return std::make_pair(out.size(), labels);
    };
    const auto close = count_at(47.0f);
    CHECK(close.first > 0 && close.second == 0, "3 m out: %zu quads, %d label", close.first, close.second);
    CHECK(LabelAlpha(3.0f) == 0 && LabelAlpha(30.0f) == 1 && LabelAlpha(65.0f) == 0 && LabelAlpha(55.0f) > 0,
          "label fade");
    // Passed the lip, in the air: the landing box still shows, the label and bar don't.
    const auto mid = count_at(55.0f);
    CHECK(mid.first > 0 && mid.second == 0, "in the air: %zu quads", mid.first);
    // All alphas are sane.
    for (const coachmark::Quad& q : m) CHECK(q.rgba[3] >= 0 && q.rgba[3] <= 1, "alpha %.2f", q.rgba[3]);
}

// The AIRH chunk, written as MXB Coach writes it, read by coachhud::Parse.
static void PutU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
static void PutF32(std::vector<uint8_t>& b, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    PutU32(b, u);
}
static std::vector<uint8_t> HudBytes(const std::vector<float>& airh, uint32_t n_in_chunk) {
    std::vector<uint8_t> b = {'M', 'X', 'H', 'D'};
    PutU32(b, 1), PutF32(b, 100.0f), PutU32(b, 3);
    for (int i = 0; i < 3; ++i) PutF32(b, float(i) / 3.0f), PutF32(b, float(i)), PutF32(b, float(i) * 10), PutF32(b, 0);
    PutU32(b, 0);  // sections
    PutU32(b, 0);  // flags
    b.insert(b.end(), {'A', 'I', 'R', 'H'});
    PutU32(b, uint32_t(4 + airh.size() * 4));
    PutU32(b, n_in_chunk);
    for (float f : airh) PutF32(b, f);
    return b;
}

static void TestParse() {
    const std::vector<float> airh = {10.6f, 0.6f, 0.0f, 12.0f, 2.0f, 1.0f, 10.6f, NAN, 0.5f};
    coachhud::Sheet s;
    std::vector<uint8_t> b = HudBytes(airh, 3);
    CHECK(coachhud::Parse(b.data(), b.size(), s), "parses");
    CHECK(s.air.size() == 9 && s.air[4] == 2.0f && s.air[5] == 1.0f && std::isnan(s.air[7]), "AIRH read: %zu", s.air.size());
    // A point count that isn't the sheet's, or an airborne share past 1, drops the chunk, not the sheet.
    b = HudBytes(airh, 2);
    CHECK(coachhud::Parse(b.data(), b.size(), s) && s.air.empty(), "wrong count dropped");
    std::vector<float> bad = airh;
    bad[2] = 2.0f;
    b = HudBytes(bad, 3);
    CHECK(coachhud::Parse(b.data(), b.size(), s) && s.air.empty() && s.ref.size() == 3, "bad share dropped");
    // And the settings: jump calls on unless hud.ini says otherwise.
    CHECK(coachhud::ParseSettings("", false).jumps, "jumps on by default");
    CHECK(!coachhud::ParseSettings("[hud]\njumps=0\n", false).jumps, "jumps=0");
}

// The on-line text in the rider's style: bold widens each lit run, italic leans the rows, block is
// the font as drawn before. Off (line_text=0) draws no label at all.
static void TheTextStyles() {
    const float base[3] = {0, 0, 0}, right[3] = {1, 0, 0}, col[4] = {1, 1, 1, 1};
    coachhud::LineLook& look = coachline::Look();
    const coachhud::LineLook keep = look;
    auto span = [&](const std::vector<coachmark::Quad>& q) {
        float lo = 1e9f, hi = -1e9f;
        for (const auto& m : q)
            for (int c = 0; c < 4; ++c) lo = (std::min)(lo, m.p[c][0]), hi = (std::max)(hi, m.p[c][0]);
        return hi - lo;
    };
    std::vector<coachmark::Quad> block, bold, italic;
    look.text_style = coachhud::TEXT_BLOCK;
    coachmark::Text(block, "I", base, right, 0.7f, col, 0);
    look.text_style = coachhud::TEXT_BOLD;
    coachmark::Text(bold, "I", base, right, 0.7f, col, 0);
    look.text_style = coachhud::TEXT_ITALIC;
    coachmark::Text(italic, "I", base, right, 0.7f, col, 0);
    CHECK(block.size() == bold.size() && block.size() == italic.size(), "the same quads in every style");
    CHECK(span(bold) > span(block) + 0.03f, "bold is wider: %.3f vs %.3f", span(bold), span(block));
    // The I's stem: upright in block, its top further right than its bottom in italic.
    const coachmark::Quad& stem = italic[3];
    CHECK(stem.p[2][0] > stem.p[1][0] && stem.p[3][0] > stem.p[0][0], "italic leans forward");
    CHECK(block[3].p[2][0] == block[3].p[1][0], "block stands upright");
    look = keep;
}

// A jump call fixed on screen is left off the line and handed over; off, it is neither.
static void TheCallOnScreen() {
    const auto verts = StraightRibbon(60.0f);
    auto two = [](float s) { return (std::max)(Mound(s, 50.0f), Mound(s, 58.0f)); };
    const coachhud::Sheet sh = Sheet(150, two, {{50.0f, 62.0f}});
    const auto calls = CallsOf(sh);
    float frac = 0;
    const int ai = coachline::AnchorPoint(sh.ref, 20.0f, 0.0f, 20.0f / 150.0f, frac);
    const std::vector<float> ahead = AheadOf(sh.ref, ai, frac, 70.0f);
    coachhud::LineLook& look = coachline::Look();
    const coachhud::LineLook keep = look;
    std::vector<coachmark::Quad> on_line, fixed, off;
    ScreenLabel none, got, hidden;
    Marks(on_line, calls, ahead, verts, true, nullptr, &none);
    look.jump.screen = true;
    Marks(fixed, calls, ahead, verts, true, nullptr, &got);
    look.jump.on = false;
    Marks(off, calls, ahead, verts, true, nullptr, &hidden);
    look = keep;
    CHECK(!none.ok, "on the line nothing is handed over");
    CHECK(got.ok && !got.top.empty() && got.alpha > 0.9f && std::fabs(got.ahead - 30.0f) < 1e-3f, "the call is handed over: %s at %.1f", got.top.c_str(), got.ahead);
    CHECK(fixed.size() + 20 < on_line.size() && off.size() == fixed.size() && !hidden.ok, "off the line, and gone when off: %zu %zu %zu", on_line.size(), fixed.size(), off.size());
}

/// The takeoff bar's world x as the rider rides toward it, with the ribbon rebuilt every two metres
/// the way the plugin does (rows fixed to the line by `m`, their `s` the distance from the rider at
/// the rebuild). Returns the spread (max - min) of the bar's position over the ride, in metres, and
/// the standard deviation of the frame-to-frame step less the rider's own.
struct Wobble {
    float spread = 0, sd = 0;
};
static Wobble BarWobble(bool anchored) {
    auto two = [](float s) { return (std::max)(Mound(s, 50.0f), Mound(s, 58.0f)); };
    const coachhud::Sheet sh    = Sheet(150, two, {{50.0f, 62.0f}});
    const auto            calls = CallsOf(sh);
    std::vector<float>    xs;
    float                 built = -1e9f;
    std::vector<coachline::Vert> verts;
    for (float rider = 0.0f; rider < 44.0f; rider += 0.2f) {
        if (rider - built >= 2.0f) {  // a rebuild: rows every half metre from just behind the rider
            built = rider;
            verts.clear();
            const float x0 = std::floor((rider - 1.0f) * 2.0f) / 2.0f;
            for (float x = x0; x <= rider + 70.0f; x += 0.5f) {
                const coachline::Vert l = {x, 1.0f, coachline::kHalfWidth, x - rider, {1, 1, 1, 0.7f}, x};
                const coachline::Vert r = {x, 1.0f, -coachline::kHalfWidth, x - rider, {1, 1, 1, 0.7f}, x};
                verts.push_back(l);
                verts.push_back(r);
            }
        }
        float     frac = 0;
        const int ai   = coachline::AnchorPoint(sh.ref, rider, 0.0f, rider / 150.0f, frac);
        Frame     fr;
        fr.rider_m = rider;
        fr.total   = 150.0f;
        std::vector<coachmark::Quad> m;
        Marks(m, calls, AheadOf(sh.ref, ai, frac, 70.0f), verts, true, anchored ? &fr : nullptr);
        if (m.size() < 2) continue;
        float cx = 0;
        for (int c = 0; c < 4; ++c) cx += m[1].p[c][0] * 0.25f;  // the lit bar
        xs.push_back(cx);
    }
    Wobble w;
    if (xs.empty()) return w;
    const auto mm = std::minmax_element(xs.begin(), xs.end());
    w.spread      = *mm.second - *mm.first;
    float mean = 0;
    for (float x : xs) mean += x / float(xs.size());
    float var = 0;
    for (float x : xs) var += (x - mean) * (x - mean) / float(xs.size());
    w.sd = std::sqrt(var);
    return w;
}

static void TestNoWobble() {
    const Wobble before = BarWobble(false), after = BarWobble(true);
    std::printf("takeoff bar position over the approach: placed by distance ahead: spread %.3f m, sd %.3f m;"
                " placed by metre on the line: spread %.4f m, sd %.4f m\n",
                before.spread, before.sd, after.spread, after.sd);
    CHECK(before.spread > 1.0f, "the old placement does wobble (%.3f m)", before.spread);
    CHECK(after.spread < 0.01f && after.sd < 0.005f, "anchored to its metre on the line: spread %.4f m", after.spread);
}

static void TestNearFade() {
    using coachline::NearFade;
    CHECK(NearFade(-3, 8) == 0 && NearFade(0, 8) == 0, "gone at and behind the rider");
    CHECK(NearFade(8, 8) == 1 && NearFade(30, 8) == 1, "solid from N metres ahead");
    CHECK(NearFade(4, 8) > 0.4f && NearFade(4, 8) < 0.6f, "half way at half the distance: %.2f", NearFade(4, 8));
    float last = -1;
    bool  up   = true;
    for (float d = 0; d <= 8; d += 0.25f) {
        up = up && NearFade(d, 8) >= last;
        last = NearFade(d, 8);
    }
    CHECK(up, "rises steadily");
    CHECK(NearFade(-3, 0) == 1 && NearFade(0, 0) == 1 && NearFade(2, -1) == 1, "off at 0");
    CHECK(coachline::WrapAhead(2.0f, 148.0f, 150.0f) > 3.9f && coachline::WrapAhead(2.0f, 148.0f, 150.0f) < 4.1f,
          "a metre across the start line is ahead");
}

int main() {
    TestRhythm();
    TestTable();
    TestOutdoor();
    TestRoll();
    TestHopsAndBounces();
    TestPredicted();
    TestLabels();
    TestMarks();
    TestParse();
    TheTextStyles();
    TheCallOnScreen();
    TestNoWobble();
    TestNearFade();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("coachjump: all checks passed\n");
    return 0;
}
