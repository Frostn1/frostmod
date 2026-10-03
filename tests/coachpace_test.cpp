// Pace hints on the ground line (src/coachpace.h): too fast and too slow against Coach's lap, the
// braking-distance model, jump lips, the hysteresis that keeps a hint from flickering on noisy
// speed, and what the hint does to the line's colours and the marks over it. Pure C++.
//
//   coachpace_test                 the checks
//   coachpace_test --dump <dir>    also writes the two preview scenes (tools/pace_preview.py
//                                  draws them as rider-view PNGs)

#include "../src/coachpace.h"

#include <cmath>
#include <cstdint>
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

using namespace coachpace;

// A synthetic lap of Coach's: points every metre along x, ridden at 25 m/s, braking hard from
// 200 m down to 10 m/s at 240 m, back on the gas to 25 m/s by 300 m; a jump with its lip at
// 450 m (a 2 m face over 8 m, then a drop), ridden at 20 m/s. 600 m in all.
struct Lap {
    std::vector<coachhud::RefPoint> ref;
    std::vector<float>              drive, height;
};
static float LapSpeed(float s) {
    if (s < 200) return 25.0f;
    if (s < 240) return std::sqrt(25.0f * 25.0f - (25.0f * 25.0f - 100.0f) * (s - 200.0f) / 40.0f);
    if (s < 300) return 10.0f + 15.0f * (s - 240.0f) / 60.0f;
    if (s < 400) return 25.0f;
    if (s < 430) return 25.0f - 5.0f * (s - 400.0f) / 30.0f;
    if (s < 470) return 20.0f;
    return 20.0f + 5.0f * (std::min)(1.0f, (s - 470.0f) / 40.0f);
}
static float LapHeight(float s) {
    if (s >= 442 && s <= 450) return 2.0f * (s - 442.0f) / 8.0f;            // the face
    if (s > 450 && s <= 458) return 2.0f - 2.5f * (s - 450.0f) / 8.0f;      // the drop
    if (s > 458 && s <= 470) return -0.5f + 0.5f * (s - 458.0f) / 12.0f;   // the landing
    if (s >= 100 && s <= 140) return 0.5f * std::sin(3.14159f * (s - 100.0f) / 40.0f);  // a roller
    return 0.0f;
}
static Lap MakeLap() {
    Lap   l;
    float t = 0;
    for (int i = 0; i <= 600; ++i) {
        const float s = float(i);
        if (i > 0) t += 1.0f / ((LapSpeed(s) + LapSpeed(s - 1)) * 0.5f);
        l.ref.push_back({s / 600.0f, t, s, 0.0f});
        const bool braking = s >= 200 && s < 240;
        l.drive.insert(l.drive.end(), {LapSpeed(s), braking ? 0.0f : 1.0f, braking ? 1.0f : 0.0f});
        l.height.push_back(LapHeight(s));
    }
    return l;
}

static Profile g_prof;

// Reads at `s` metres along the lap, riding at `speed`.
static Reading At(float s, float speed) {
    const int   i = int(s);
    const float f = s - float(i);
    return Read(g_prof, i, f, speed);
}

static void TestProfile() {
    CHECK(g_prof.ok(), "the profile builds");
    CHECK(std::fabs(g_prof.v[100] - 25.0f) < 0.01f, "DRIV speed taken: %f", g_prof.v[100]);
    CHECK(g_prof.lips.size() == 1, "one lip (the roller is not one): %zu", g_prof.lips.size());
    if (!g_prof.lips.empty()) CHECK(std::abs(int(g_prof.lips[0]) - 450) <= 2, "lip at 450 m: %zu", g_prof.lips[0]);
    // Without DRIV the speed comes from the sheet's times.
    Lap           l = MakeLap();
    const Profile p = BuildProfile(l.ref, {}, l.height);
    CHECK(p.ok() && std::fabs(p.v[100] - 25.0f) < 0.5f, "speed from times: %f", p.ok() ? p.v[100] : -1.0f);
    // A tabletop: a face, then flat at the top. Still a lip.
    std::vector<float> s, h, v;
    for (int i = 0; i < 100; ++i) {
        s.push_back(float(i));
        v.push_back(20.0f);
        h.push_back(i < 40 ? 0.0f : i < 48 ? 2.0f * float(i - 40) / 8.0f : 2.0f);
    }
    CHECK(FindLips(s, h, v).size() == 1, "a tabletop's edge is a lip");
    // Ridden slowly (a hairpin with a bump in it), not a jump.
    for (float& x : v) x = 5.0f;
    CHECK(FindLips(s, h, v).empty(), "not at walking pace");
    // Too short a sheet: nothing.
    CHECK(!BuildProfile({}, {}).ok(), "no sheet, no profile");
}

static void TestDetection() {
    // On Coach's pace, well before the corner: nothing.
    Reading r = At(120, 25.0f);
    CHECK(r.valid && std::fabs(r.rel) < 1e-3f && std::fabs(r.score) < 0.2f, "on pace: score %f", r.score);
    // 12% fast 50 m before the braking zone: too fast, and by the braking model they'd run past.
    r = At(150, 28.0f);
    CHECK(r.score >= 1.0f, "12%% fast: score %f", r.score);
    CHECK(r.brake_m > 35 && r.brake_m < 55, "braking zone in view: %f", r.brake_m);
    CHECK(r.overshoot_m > kEnterOvershootM, "overshoot %f m", r.overshoot_m);
    // Only 5% fast, but into the corner: the braking model says too fast, the speed alone wouldn't.
    r = At(170, 26.25f);
    CHECK(r.rel < kEnterRel && r.score >= 1.0f, "5%% into a corner: rel %f score %f overshoot %f", r.rel, r.score,
          r.overshoot_m);
    // The same 5% on an open straight (nothing in view): not enough.
    r = At(320, 26.25f);
    CHECK(r.brake_m < 0 && r.score > 0 && r.score < 1.0f, "5%% on a straight: score %f", r.score);
    // 15% slow on the straight: too slow.
    r = At(320, 21.25f);
    CHECK(r.score <= -1.0f, "15%% slow: score %f", r.score);
    // 15% slow just before the braking zone: never "faster" into a corner.
    r = At(190, 21.25f);
    CHECK(r.score == 0.0f, "slow before a corner says nothing: %f", r.score);
    // Below walking pace, or where Coach was barely moving: nothing.
    CHECK(!At(320, 2.0f).valid, "too slow to say");
    // The lip in view ahead.
    r = At(420, 16.0f);
    CHECK(r.lip_m > 25 && r.lip_m < 35, "lip 30 m ahead: %f", r.lip_m);
    CHECK(r.score <= -1.0f, "slow into the jump: %f", r.score);
}

// Runs a filter over `secs` seconds of readings at 50 Hz; returns how many times what it wants
// changed and how many times the drawn hint switched on or off.
struct Run {
    int  wants = 0, toggles = 0;
    Hint last;
};
template <class F>
static Run Feed(Filter& f, float secs, F reading) {
    Run   out;
    Kind  w     = f.wanted();
    bool  shown = f.wanted() != NONE;
    for (int k = 0; k < int(secs * 50); ++k) {
        out.last = f.step(reading(k), 0.02f);
        if (f.wanted() != w) ++out.wants, w = f.wanted();
        const bool now = out.last.kind != NONE;
        if (now != shown) ++out.toggles, shown = now;
    }
    return out;
}
static Reading Score(float s) {
    Reading r;
    r.valid = true;
    r.score = s;
    return r;
}

static void TestHysteresis() {
    Filter f;
    // A clear too-fast: on within a second, drawn at full strength soon after.
    Run run = Feed(f, 1.0f, [](int) { return Score(2.0f); });
    CHECK(f.wanted() == FAST && run.last.kind == FAST && run.last.level > 0.3f, "too fast shows: %d %f", int(f.wanted()),
          run.last.level);
    // Between the exit and the entry threshold: it stays.
    run = Feed(f, 5.0f, [](int) { return Score(0.7f); });
    CHECK(f.wanted() == FAST && run.wants == 0 && run.toggles == 0, "held between thresholds");
    // Back on pace: it goes, but not at once, and it fades rather than vanishing.
    run = Feed(f, 0.5f, [](int) { return Score(0.0f); });
    CHECK(f.wanted() == FAST, "not gone after half a second");
    run = Feed(f, 3.0f, [](int) { return Score(0.0f); });
    CHECK(f.wanted() == NONE && run.last.kind == NONE && run.last.level == 0.0f, "gone after the hold-out and fade");
    // A blip past the threshold shorter than the hold-in: nothing.
    f.reset();
    run = Feed(f, 2.0f, [](int k) { return Score(k < 10 ? 3.0f : 0.0f); });
    CHECK(run.wants == 0 && run.toggles == 0, "a 0.2 s blip is ignored: %d", run.wants);
    // Too fast to too slow: only through nothing, with the fast one faded out first.
    f.reset();
    Feed(f, 2.0f, [](int) { return Score(2.0f); });
    bool  overlap = false, saw_slow = false;
    Kind  prev    = FAST;
    for (int k = 0; k < 300; ++k) {
        const Hint h = f.step(Score(-2.0f), 0.02f);
        if (prev == FAST && h.kind == SLOW) overlap = true;  // never straight from one to the other
        if (h.kind == SLOW) saw_slow = true;
        if (h.kind != NONE || f.wanted() != NONE) prev = h.kind == NONE ? prev : h.kind;
        if (h.kind == NONE) prev = NONE;
    }
    CHECK(!overlap && saw_slow, "fast fades out before slow fades in");
    // Too slow starts the same way.
    f.reset();
    run = Feed(f, 1.0f, [](int) { return Score(-1.6f); });
    CHECK(f.wanted() == SLOW && run.last.kind == SLOW, "too slow shows");
}

// A deterministic noise source, so the test is the same every run.
static float Noise(uint32_t& st) {
    st = st * 1664525u + 1013904223u;
    return float(st >> 8) / float(1u << 24) * 2.0f - 1.0f;  // -1..1
}

static void TestNoFlicker() {
    // Speed jittering ±6% around 9% fast, before the corner, for 30 s: one hint, steady.
    {
        Filter   f;
        uint32_t st  = 1;
        Run      run = Feed(f, 30.0f, [&](int k) {
            const float s = 100.0f + float(k % 50);  // ride the same 50 m over and over
            return At(s, 25.0f * (1.09f + 0.06f * Noise(st)));
        });
        CHECK(run.wants == 1 && run.toggles == 1, "noisy fast: one hint (%d wants, %d toggles)", run.wants, run.toggles);
    }
    // Speed jittering ±7% around Coach's on a straight: no hint at all.
    {
        Filter   f;
        uint32_t st  = 7;
        Run      run = Feed(f, 30.0f, [&](int k) { return At(320.0f + float(k % 50), 25.0f * (1.0f + 0.07f * Noise(st))); });
        CHECK(run.wants == 0 && run.toggles == 0, "noisy on-pace: nothing (%d, %d)", run.wants, run.toggles);
    }
    // Hovering right at the threshold (8% ± 4%): it may start once, but never chatters.
    {
        Filter   f;
        uint32_t st  = 3;
        Run      run = Feed(f, 60.0f, [&](int) { return At(320.0f, 25.0f * (1.08f + 0.04f * Noise(st))); });
        CHECK(run.toggles <= 2, "at the threshold: %d toggles in a minute", run.toggles);
    }
    // A speed that swings from too fast to too slow every 0.3 s: smoothed away.
    {
        Filter f;
        Run    run = Feed(f, 20.0f, [&](int k) { return At(320.0f, (k / 15) % 2 ? 28.0f : 22.0f); });
        CHECK(run.toggles == 0, "a 0.3 s swing shows nothing: %d", run.toggles);
    }
    // The drawn level never jumps: at most the fade rate per sample.
    {
        Filter   f;
        uint32_t st   = 11;
        float    prev = 0, worst = 0;
        for (int k = 0; k < 3000; ++k) {
            const Hint h = f.step(At(320.0f, 25.0f * (1.0f + 0.2f * Noise(st) + (k / 500 % 2 ? 0.15f : -0.15f))), 0.02f);
            worst        = (std::max)(worst, std::fabs(h.level - prev));
            prev         = h.level;
        }
        CHECK(worst <= kFadeRate * 0.02f + 1e-5f, "level step %f", worst);
    }
}

static void TestMoreSpeed() {
    // Slow into the jump: too slow, with MORE SPEED once the lip is in view.
    Filter f;
    Hint   h;
    for (int k = 0; k < 75; ++k) h = f.step(At(400.0f + float(k) * 0.32f, 17.0f), 0.02f);
    CHECK(h.kind == SLOW && h.more_speed && h.lip_m > 0, "MORE SPEED before the lip: kind %d more %d lip %f", int(h.kind),
          int(h.more_speed), h.lip_m);
    // Past the lip it goes.
    for (int k = 0; k < 10; ++k) h = f.step(At(455.0f, 17.0f), 0.02f);
    CHECK(!h.more_speed, "not after the lip");
    // On pace into it: no mark.
    Filter g;
    for (int k = 0; k < 100; ++k) h = g.step(At(420.0f, 22.0f), 0.02f);
    CHECK(!h.more_speed && h.kind == NONE, "on pace: no mark");
}

static void TestColours() {
    Lap                      l    = MakeLap();
    const std::vector<float> base = coachline::LineColours(l.ref, l.drive);
    CHECK(base.size() == l.ref.size() * 4, "base colours");
    Hint h;
    h.kind      = FAST;
    h.level     = 1.0f;
    h.advance_m = 15.0f;
    // The rider at 160 m; the braking zone starts near 195 (smoothed).
    std::vector<float> c = Recolour(g_prof, base, 160, 0.0f, h);
    auto redder = [&](size_t i) { return c[i * 4] > base[i * 4] + 0.1f; };  // off the gas green, toward white/yellow/red
    CHECK(redder(185), "too fast: 10 m before the zone already turning");
    CHECK(!redder(150), "behind the rider untouched");
    CHECK(c[185 * 4 + 3] == base[185 * 4 + 3], "alpha kept");
    // The base layer itself is untouched where nothing is ahead within the advance.
    CHECK(!redder(170), "well before: still the base colour");
    // Too slow: the gas brighter and more solid; braking untouched.
    h.kind = SLOW;
    c      = Recolour(g_prof, base, 300, 0.0f, h);
    CHECK(c[320 * 4 + 1] >= base[320 * 4 + 1] && c[320 * 4 + 3] > base[320 * 4 + 3], "brighter green");
    c = Recolour(g_prof, base, 190, 0.0f, h);
    CHECK(std::fabs(c[220 * 4 + 0] - base[220 * 4 + 0]) < 1e-4f, "braking keeps its colour");
    // No hint: identical.
    h.kind = NONE;
    CHECK(Recolour(g_prof, base, 190, 0.0f, h) == base, "no hint, no change");
}

// A ribbon along +x from x0, rows every half metre, as coachline::Ribbon lays it out.
static std::vector<coachline::Vert> Strip(float x0) {
    std::vector<coachline::Vert> v;
    for (float s = 0; s <= 60.0f; s += 0.5f) {
        v.push_back({x0 + s, 0.0f, coachline::kHalfWidth, s, {1, 1, 1, 0.7f}});   // left (+z when heading +x)
        v.push_back({x0 + s, 0.0f, -coachline::kHalfWidth, s, {1, 1, 1, 0.7f}});  // right
    }
    return v;
}

static void TestMarks() {
    Hint h;
    h.kind    = FAST;
    h.level   = 1.0f;
    h.brake_m = 30.0f;
    std::vector<coachline::Vert> m = Marks(Strip(100.0f), h, 100.0f);
    CHECK(!m.empty() && m.size() % 12 == 0, "chevrons: %zu verts", m.size());
    // Pointing back at the rider: each chevron's tip (on the centre line) is nearer than its arms.
    bool back = true, before_brake = true;
    for (size_t k = 0; k + 12 <= m.size(); k += 12) {
        const float tip = m[k].x, arm = m[k + 2].x;
        if (!(tip < arm)) back = false;
        if (m[k].s > h.brake_m + 4.0f) before_brake = false;
    }
    CHECK(back, "too fast points back");
    CHECK(before_brake, "chevrons stop at the braking point");
    CHECK(m[0].rgba[0] == kFastColour[0] && m[0].rgba[2] == kFastColour[2], "overlay colour, not a base one");
    // Fixed to the ground: the rider 2 m further on, the chevrons are where they were.
    std::vector<coachline::Vert> m2 = Marks(Strip(102.0f), h, 102.0f);
    bool fixed = !m2.empty();
    for (const coachline::Vert& a : m2) {
        if (a.rgba[3] <= 0) continue;
        const float tipx = std::fmod(a.x, kChevGapM);
        bool        any  = false;
        for (const coachline::Vert& b : m)
            if (std::fabs(std::fmod(b.x, kChevGapM) - tipx) < 1e-3f) any = true;
        if (!any) fixed = false;
    }
    CHECK(fixed, "chevrons stay put on the ground");
    // Too slow: forward, stopping short of a braking zone.
    h.kind    = SLOW;
    h.brake_m = 25.0f;
    m         = Marks(Strip(0.0f), h, 0.0f);
    bool fwd = !m.empty(), short_of = true;
    for (size_t k = 0; k + 12 <= m.size(); k += 12) {
        if (!(m[k].x > m[k + 2].x)) fwd = false;
        if (m[k].s > 20.0f + 1e-3f) short_of = false;
    }
    CHECK(fwd && short_of, "too slow points forward, short of the corner");
    // MORE SPEED: the gate at the lip.
    h.brake_m    = -1;
    h.more_speed = true;
    h.lip_m      = 30.0f;
    m            = Marks(Strip(0.0f), h, 0.0f);
    bool gate = false;
    for (const coachline::Vert& v : m)
        if (std::fabs(v.x - 30.0f) < 0.3f && std::fabs(v.z) > 0.9f) gate = true;
    CHECK(gate, "a gate across the line at the lip");
    // Nothing to draw.
    h.kind = NONE;
    CHECK(Marks(Strip(0.0f), h, 0.0f).empty(), "no hint, no marks");
}

static void TestHud() {
    coachhud::Settings s = coachhud::ParseSettings("", false);
    CHECK(!s.pace, "off until asked for");
    s = coachhud::ParseSettings("[hud]\nground=1\npace=1\n", false);
    CHECK(s.pace && s.ground, "pace=1");
    coachhud::View v;
    v.set        = s;
    v.more_speed = true;
    coachhud::Frame f;
    coachhud::Build(v, f);
    bool said = false;
    for (const auto& t : f.texts)
        if (t.s == "MORE SPEED") said = true;
    CHECK(said, "MORE SPEED on the HUD");
    v.set.pace = false;
    coachhud::Build(v, f);
    said = false;
    for (const auto& t : f.texts)
        if (t.s == "MORE SPEED") said = true;
    CHECK(!said, "not with pace off");
}

// The two preview scenes as text: the ribbon (with pace colours) and the marks, for
// tools/pace_preview.py to draw in perspective from the rider's seat.
static void Dump(const char* path, float at_s, float speed) {
    Lap    l = MakeLap();
    Filter f;
    Hint   h;
    // Ridden at this pace for a couple of seconds up to `at_s`.
    for (int k = 0; k < 100; ++k) h = f.step(At(at_s - (100 - k) * 0.02f * speed, speed), 0.02f);
    const Reading r = At(at_s, speed);
    coachline::RibbonExtras ex;
    ex.version = 1;
    ex.rgba    = Recolour(g_prof, coachline::LineColours(l.ref, l.drive), int(at_s), at_s - std::floor(at_s), h);
    coachline::RibbonExtras base;
    base.version = 2;
    base.rgba    = coachline::LineColours(l.ref, l.drive);
    FILE* o = std::fopen(path, "w");
    if (!o) return;
    std::fprintf(o, "# rel=%.3f overshoot=%.1f brake=%.1f lip=%.1f kind=%d level=%.2f more=%d adv=%.1f\n", r.rel,
                 r.overshoot_m, r.brake_m, r.lip_m, int(h.kind), h.level, int(h.more_speed), h.advance_m);
    std::fprintf(o, "speed %.2f %.2f\n", speed, r.vref);
    // The ribbon rows, from the rider on, every half metre: both colour layers.
    std::vector<coachline::Vert> strip;
    for (float s = 0; s <= coachline::kAhead; s += coachline::kStepTerrain) {
        const float  x = at_s + s;
        const size_t i = (std::min)(size_t(x), l.ref.size() - 2);
        const float  u = x - float(i);
        float        c[4], b[4];
        for (int k = 0; k < 4; ++k) {
            c[k] = ex.rgba[i * 4 + k] + (ex.rgba[(i + 1) * 4 + k] - ex.rgba[i * 4 + k]) * u;
            b[k] = base.rgba[i * 4 + k] + (base.rgba[(i + 1) * 4 + k] - base.rgba[i * 4 + k]) * u;
        }
        const float y = LapHeight(x) + coachline::kLiftTerrain;
        strip.push_back({x, y, coachline::kHalfWidth, s, {c[0], c[1], c[2], c[3]}});
        strip.push_back({x, y, -coachline::kHalfWidth, s, {c[0], c[1], c[2], c[3]}});
        std::fprintf(o, "row %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n", s, x, y, c[0], c[1], c[2],
                     c[3] * coachline::Fade(s), b[0], b[1], b[2], b[3] * coachline::Fade(s));
    }
    for (const coachline::Vert& v : Marks(strip, h, PhaseAt(g_prof, int(at_s), at_s - std::floor(at_s))))
        std::fprintf(o, "mark %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n", v.x, v.y, v.z, v.rgba[0], v.rgba[1], v.rgba[2],
                     v.rgba[3]);
    for (float x = at_s - 5; x <= at_s + 120; x += 0.5f) std::fprintf(o, "ground %.3f %.3f\n", x, LapHeight(x));
    std::fclose(o);
}

// The line's look: its defaults are today's colours, and a colour or opacity the rider picks is
// what LineColours and the marks then use.
static void TestLook() {
    using namespace coachpace;
    coachhud::LineLook&      look = coachline::Look();
    const coachhud::LineLook keep = look;
    for (int c = 0; c < 3; ++c)
        CHECK(look.fast[c] == kFastColour[c] && look.slow[c] == kSlowColour[c], "pace defaults unchanged");
    CHECK(look.opacity == coachline::kLineAlpha, "opacity default is the line's alpha");
    float r, g, b;
    coachline::ToneColour(0, r, g, b);
    CHECK(r == 0.15f && g == 0.85f && b == 0.20f, "gas default unchanged");
    look.gas[0] = 0.0f, look.gas[1] = 0.0f, look.gas[2] = 1.0f;
    look.opacity = 0.4f;
    coachline::ToneColour(0, r, g, b);
    CHECK(r == 0.0f && g == 0.0f && b == 1.0f, "the rider's gas colour");
    const Lap l = MakeLap();
    const std::vector<float> rgba = coachline::LineColours(l.ref, l.drive);
    CHECK(!rgba.empty() && rgba[3] == 0.4f, "the rider's opacity");
    CHECK(coachline::HalfWidth() == coachline::kHalfWidth, "width default");
    look.width = 2.0f;
    CHECK(coachline::HalfWidth() == 2.0f * coachline::kHalfWidth, "twice as wide");
    look = keep;
}

int main(int argc, char** argv) {
    Lap l  = MakeLap();
    g_prof = BuildProfile(l.ref, l.drive, l.height);
    TestProfile();
    TestDetection();
    TestHysteresis();
    TestNoFlicker();
    TestMoreSpeed();
    TestColours();
    TestMarks();
    TestHud();
    TestLook();
    if (argc >= 3 && std::strcmp(argv[1], "--dump") == 0) {
        const std::string dir = argv[2];
        Dump((dir + "/pace-too-fast.txt").c_str(), 160.0f, 28.5f);   // 14% fast, 35 m from the corner
        Dump((dir + "/pace-too-slow.txt").c_str(), 415.0f, 16.0f);   // 20% slow, 35 m from the lip
    }
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("coachpace: all checks passed\n");
    return 0;
}
