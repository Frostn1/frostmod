// Gear hints on the ground line (src/coachgear.h): the sheet's GEAR chunk, Coach's shifts, up and
// down detection and the target gear, the hysteresis that keeps a hint from flickering, crashes
// and the air, the glyph drawn on the line and the badge beside the cue box. Pure C++.
//
//   coachgear_test                 the checks
//   coachgear_test --dump <dir>    also writes the two scenes tools/gear_preview.py draws as a
//                                  rider-view PNG

#include "../src/coachgear.h"

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

using namespace coachgear;

// A lap of Coach's: points every metre along +x, 600 m round, in third until 200 m, fourth to
// 350 m, then third again. A two-metre blip into fifth at 100 m, and neutral at 199 and 200 m
// (the clutch in), which are not shifts.
struct Lap {
    std::vector<coachhud::RefPoint> ref;
    std::vector<uint8_t>            gear;
};
static Lap MakeLap() {
    Lap l;
    for (int i = 0; i < 600; ++i) {
        l.ref.push_back({i / 600.0f, i * 0.04f, float(i), 0.0f});
        int g = i < 200 ? 3 : i < 350 ? 4 : 3;
        if (i == 100 || i == 101) g = 5;
        if (i == 199 || i == 200) g = 0;
        l.gear.push_back(uint8_t(g));
    }
    return l;
}

// --- the sheet ----------------------------------------------------------------------------

struct Out {
    std::vector<uint8_t> b;
    void raw(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    void u32(uint32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
};

// A sheet as the app writes it: points, no sections, flags, then a DRIV chunk, then `gear` (a
// GEAR chunk claiming `claim` points) when given, then a chunk nobody knows.
static std::vector<uint8_t> Sheet(const Lap& l, const std::vector<uint8_t>* gear, uint32_t claim) {
    Out o;
    o.raw(coachhud::kMagic, 4);
    o.u32(coachhud::kVersion);
    o.f32(600.0f);
    o.u32(uint32_t(l.ref.size()));
    for (const coachhud::RefPoint& p : l.ref) o.f32(p.pos), o.f32(p.t), o.f32(p.x), o.f32(p.z);
    o.u32(0);  // sections
    o.u32(0);  // flags
    o.raw("DRIV", 4);
    o.u32(4 + uint32_t(l.ref.size()) * 12);
    o.u32(uint32_t(l.ref.size()));
    for (size_t i = 0; i < l.ref.size(); ++i) o.f32(20.0f), o.f32(0.5f), o.f32(0.0f);
    if (gear) {
        o.raw("GEAR", 4);
        o.u32(4 + uint32_t(gear->size()));
        o.u32(claim);
        o.raw(gear->data(), gear->size());
    }
    o.raw("ZZZZ", 4);
    o.u32(3);
    o.raw("abc", 3);
    return o.b;
}

static void TheSheetCarriesTheGear() {
    const Lap l = MakeLap();
    coachhud::Sheet s;
    auto            b = Sheet(l, &l.gear, uint32_t(l.gear.size()));
    CHECK(coachhud::Parse(b.data(), b.size(), s), "parse");
    CHECK(s.gear == l.gear, "the gear of every point comes back (%zu)", s.gear.size());
    CHECK(s.drive.size() == l.ref.size() * 3, "and the chunk before it is still read");

    // An older recorder's sheet, or an older Coach's: no GEAR, and nothing is lost.
    coachhud::Sheet old;
    b = Sheet(l, nullptr, 0);
    CHECK(coachhud::Parse(b.data(), b.size(), old) && old.gear.empty() && old.ref.size() == 600, "no GEAR, no gears");

    // One that disagrees with the points, or isn't gears, is dropped and the sheet stands.
    coachhud::Sheet bad;
    b = Sheet(l, &l.gear, 599);
    CHECK(coachhud::Parse(b.data(), b.size(), bad) && bad.gear.empty() && bad.drive.size() == 1800, "wrong count");
    std::vector<uint8_t> silly = l.gear;
    silly[10] = 200;
    coachhud::Sheet bad2;
    b = Sheet(l, &silly, uint32_t(silly.size()));
    CHECK(coachhud::Parse(b.data(), b.size(), bad2) && bad2.gear.empty(), "a gear of 200");
    // Truncated mid-chunk: the chunk is lost, not the sheet.
    coachhud::Sheet cut;
    b = Sheet(l, &l.gear, uint32_t(l.gear.size()));
    b.resize(b.size() - 100);
    CHECK(coachhud::Parse(b.data(), b.size(), cut) && cut.gear.empty() && cut.ref.size() == 600, "a short chunk");
}

static void TheSetting() {
    CHECK(!coachhud::ParseSettings("", false).gear, "off by default");
    CHECK(!coachhud::ParseSettings("[hud]\nenabled=1\n", false).gear, "and with a file that doesn't say");
    CHECK(coachhud::ParseSettings("[hud]\ngear=1\n", false).gear, "gear=1 turns it on");
    CHECK(!coachhud::ParseSettings("[hud]\ngear=0\nground=1\n", false).gear, "gear=0 keeps it off");
    CHECK(!coachhud::ParseSettings("[hud]\ntrail=1\npace=1\nground=1\n", false).gear, "nothing else brings it");
}

// --- Coach's shifts -------------------------------------------------------------------------

static void TheShifts() {
    const Lap l = MakeLap();
    const Profile p = BuildProfile(l.ref, l.gear);
    CHECK(p.ok(), "a profile");
    // 600 points a metre apart: 599 m between the first and the last, and the gap back to the
    // start is too long to be the lap closing, so the lap is 599.
    CHECK(std::fabs(p.lap_m - 599.0f) < 0.01f, "the lap is 599 m (%f)", double(p.lap_m));
    CHECK(p.shifts.size() == 2, "two shifts: the blip into fifth and the clutch in neutral are not (%zu)", p.shifts.size());
    if (p.shifts.size() == 2) {
        CHECK(p.shifts[0].from == 3 && p.shifts[0].to == 4 && std::fabs(p.shifts[0].m - 201.0f) < 0.01f,
              "up to fourth at 201 m, after the neutral (%d>%d at %f)", p.shifts[0].from, p.shifts[0].to,
              double(p.shifts[0].m));
        CHECK(p.shifts[1].from == 4 && p.shifts[1].to == 3 && std::fabs(p.shifts[1].m - 350.0f) < 0.01f, "down at 350 m");
    }
    // The lap is a circle: it ends in the gear it starts in, so nothing happens at the line...
    CHECK(p.shifts.empty() || p.shifts.front().m > 1.0f, "nothing at the line");
    // ...and one that does end in another gear shifts just after it.
    Lap w = MakeLap();
    for (int i = 0; i < 30; ++i) w.gear[size_t(i)] = 2;  // starts in second, ends in third
    const Profile pw = BuildProfile(w.ref, w.gear);
    bool          at_line = false;
    for (const Shift& s : pw.shifts) at_line |= s.from == 3 && s.to == 2 && s.m < 1.0f;
    CHECK(at_line, "ends in third, starts in second: a shift down at the line");

    // Without gears, or the wrong number of them, no profile.
    CHECK(!BuildProfile(l.ref, {}).ok(), "no gears");
    CHECK(!BuildProfile(l.ref, std::vector<uint8_t>(10, 3)).ok(), "wrong count");
    CHECK(!BuildProfile(l.ref, std::vector<uint8_t>(600, 4)).ok(), "one gear all lap: no shifts, nothing to say");
    CHECK(!BuildProfile(l.ref, std::vector<uint8_t>(600, 0)).ok(), "never in gear");

    // A shift made in the air: the reference bike 2 m over the ground there.
    std::vector<float> ground(600, 0.0f), refy(600, 0.6f);
    for (int i = 195; i < 210; ++i) refy[size_t(i)] = 2.5f;
    const Profile pa = BuildProfile(l.ref, l.gear, ground, refy);
    CHECK(pa.shifts.size() == 2 && pa.shifts[0].air && !pa.shifts[1].air, "the first shift is in the air, the second isn't");
    const Profile pn = BuildProfile(l.ref, l.gear);
    CHECK(!pn.shifts[0].air, "no ground known: not the air");
}

// --- up or down, and to what ----------------------------------------------------------------

static Reading At(const Profile& p, float phase, int gear, float speed = 20.0f, bool crashed = false, bool air = false) {
    return Read(p, phase, gear, speed, crashed, air);
}

static void UpOrDown() {
    const Lap l = MakeLap();
    const Profile p = BuildProfile(l.ref, l.gear);  // 3>4 at 201, 4>3 at 350
    // 30 m before the first shift (3 to 4).
    float ph = 171;
    CHECK(At(p, ph, 3).dir == NONE, "in the gear Coach is in: nothing yet");
    CHECK(At(p, ph, 4).dir == NONE, "already in the one he shifts to: early is fine");
    Reading r = At(p, ph, 2);
    CHECK(r.dir == UP && r.target == 4, "second where Coach goes to fourth: up to 4 (%d, %d)", int(r.dir), r.target);
    r = At(p, ph, 6);
    CHECK(r.dir == DOWN && r.target == 4, "sixth: down to 4 (%d, %d)", int(r.dir), r.target);
    r = At(p, ph, 5);
    CHECK(r.dir == DOWN && r.target == 4, "fifth: down to 4");
    CHECK(std::fabs(r.ahead_m - 30.0f) < 0.01f, "the shift is 30 m ahead (%f)", double(r.ahead_m));
    // Where it is drawn: kLeadM before the shift.
    CHECK(std::fabs(r.place_m - (ph + 30.0f - kLeadM)) < 0.01f, "6 m before the shift (%f)", double(r.place_m));

    // Only the nearest shift matters and nothing is said far from one.
    CHECK(At(p, 120, 2).dir == NONE, "80 m from the shift: too far to say");
    CHECK(At(p, 251, 6).dir == NONE, "100 m before the next: nothing");
    CHECK(At(p, 152, 2).dir == UP, "49 m ahead is in range");
    CHECK(At(p, 150, 2).dir == NONE, "51 m is not");

    // At and after the shift only the new gear is right: still in the old one, up to it.
    r = At(p, 204, 3);
    CHECK(r.dir == UP && r.target == 4 && r.ahead_m < 0, "3 m past the shift, still in third: up to 4");
    CHECK(At(p, 204, 4).dir == NONE, "in fourth: done");
    CHECK(At(p, 204, 2).dir == UP && At(p, 204, 6).dir == DOWN, "or the wrong side of it");
    CHECK(At(p, 201 + kPastM + 1, 3).dir == NONE, "far enough past, third is just the rider's choice");
    // The hint is never put behind the rider, or on top of them.
    CHECK(std::fabs(At(p, 204, 3).place_m - (204 + kMinPlaceM)) < 0.01f, "past the shift: kMinPlaceM ahead");
    CHECK(std::fabs(At(p, 195, 2).place_m - (195 + kMinPlaceM)) < 0.01f, "6 m ahead: kMinPlaceM ahead too");

    // The second shift, a downshift: fourth to third.
    r = At(p, 330, 5);
    CHECK(r.dir == DOWN && r.target == 3, "fifth where Coach goes down to third: down to 3");
    r = At(p, 330, 2);
    CHECK(r.dir == UP && r.target == 3, "second, below both of Coach's gears there: up to 3");
    r = At(p, 330, 4);
    CHECK(r.dir == NONE, "in fourth, which he is in until the shift: nothing yet");

    // The lap's end: a shift just past the line is found from before it.
    Lap w = MakeLap();
    for (int i = 0; i < 30; ++i) w.gear[size_t(i)] = 2;
    const Profile pw = BuildProfile(w.ref, w.gear);
    r = At(pw, 590, 3);  // 10 m before the line, in third: Coach goes to second
    CHECK(r.dir == NONE, "in third before a shift down to second: still on Coach's line");
    r = At(pw, 590, 5);
    CHECK(r.dir == DOWN && r.target == 2, "fifth: down to 2, found across the line");
    CHECK(r.place_m >= 0 && r.place_m < 600.0f, "and placed on the lap (%f)", double(r.place_m));

    // Nothing to say while crashed, crawling, in neutral, or without a gear reading.
    CHECK(At(p, ph, 2, 20.0f, true).dir == NONE, "crashed");
    CHECK(At(p, ph, 2, 1.0f).dir == NONE, "crawling");
    CHECK(At(p, ph, 0).dir == NONE, "in neutral");
    CHECK(At(p, ph, -1).dir == NONE && At(p, ph, 12).dir == NONE, "a gear that isn't one");
    CHECK(At(p, NAN, 2).dir == NONE, "no place on the line");
    CHECK(Read(Profile{}, ph, 2, 20, false, false).dir == NONE, "no profile");

    // In the air: only for a shift Coach made in the air.
    CHECK(At(p, ph, 2, 20.0f, false, true).dir == NONE, "in the air: nothing");
    std::vector<float> ground(600, 0.0f), refy(600, 0.6f);
    for (int i = 195; i < 210; ++i) refy[size_t(i)] = 2.5f;
    const Profile pa = BuildProfile(l.ref, l.gear, ground, refy);
    CHECK(At(pa, ph, 2, 20.0f, false, true).dir == UP, "in the air where Coach shifted in the air: up to 4");
    CHECK(At(pa, 330, 5, 20.0f, false, true).dir == NONE, "but not for the one on the ground");
    CHECK(At(pa, ph, 2, 20.0f, false, false).dir == UP, "and on the ground, the same hint");
}

// --- not flickering ------------------------------------------------------------------------

static Reading Want(Dir d, int target, float place = 100.0f) {
    Reading r;
    r.dir = d, r.target = target, r.place_m = place, r.shift = 0, r.ahead_m = 20;
    return r;
}

// Runs the filter for `secs` at 50 Hz on a reading; returns the last hint.
static Hint Run(Filter& f, const Reading& r, float secs, float* first_shown = nullptr, float* t = nullptr) {
    Hint h;
    float local = 0;
    if (!t) t = &local;
    for (int i = 0; i < int(secs * 50 + 0.5f); ++i) {
        h = f.step(r, 0.02f, 600.0f);
        *t += 0.02f;
        if (first_shown && *first_shown < 0 && h.level > 0) *first_shown = *t;
    }
    return h;
}

static void Hysteresis() {
    // It takes kHoldInS to start...
    Filter f;
    float  t = 0, shown = -1;
    Hint   h = Run(f, Want(UP, 4), 0.2f, &shown, &t);
    CHECK(h.level == 0 && h.dir == NONE, "0.2 s of wanting is not enough");
    h = Run(f, Want(UP, 4), 0.3f, &shown, &t);
    CHECK(h.dir == UP && h.target == 4 && h.level > 0, "0.5 s is");
    CHECK(shown >= kHoldInS - 0.001f && shown < 0.45f, "it started at %f s", double(shown));
    // ...fades in, rather than appearing.
    CHECK(h.level < 1.0f, "still fading in (%f)", double(h.level));
    h = Run(f, Want(UP, 4), 0.5f, nullptr, &t);
    CHECK(h.level == 1.0f, "fully in");

    // A hint that is wanted again and again for a moment never starts: a gear reading that
    // flickers, a rider sitting on the edge of the range.
    Filter g;
    int    starts = 0;
    bool   was = false;
    for (int i = 0; i < 500; ++i) {
        const bool want = (i / 7) % 2 == 0;  // 0.14 s on, 0.14 s off
        const Hint k = g.step(want ? Want(UP, 4) : Reading{}, 0.02f, 600.0f);
        if (k.level > 0 && !was) ++starts;
        was = k.level > 0;
    }
    CHECK(starts == 0, "a reading that comes and goes every 0.14 s shows nothing (%d starts)", starts);

    // Once showing, a dropout shorter than kHoldOutS doesn't end it.
    Filter e;
    Run(e, Want(UP, 4), 1.0f);
    Hint k = Run(e, Reading{}, 0.4f);
    CHECK(k.level > 0.99f && e.showing(), "0.4 s without it: still there");
    k = Run(e, Want(UP, 4), 0.1f);
    k = Run(e, Reading{}, 0.4f);
    CHECK(e.showing(), "and the dropout clock starts over");
    // Longer than that ends it and fades out at a limited rate, not at once.
    k = Run(e, Reading{}, 0.4f);
    CHECK(!e.showing(), "0.8 s: over");
    CHECK(k.level < 1.0f && k.level > 0.0f && k.dir == UP, "still fading (%f)", double(k.level));
    k = Run(e, Reading{}, 0.5f);
    CHECK(k.level == 0 && k.dir == NONE, "gone");

    // It shows at least kMinShowS: wanted for 0.3 s then not at all, it still lasts.
    Filter m;
    t = 0;
    Run(m, Want(UP, 4), 0.5f, nullptr, &t);  // starts at ~0.3 s
    float gone = -1;
    for (int i = 0; i < 400 && gone < 0; ++i) {
        m.step(Reading{}, 0.02f, 600.0f);
        t += 0.02f;
        if (!m.showing()) gone = t;
    }
    CHECK(gone - 0.3f >= kMinShowS - 0.05f && gone - 0.3f < kMinShowS + 0.2f, "shown for %f s", double(gone - 0.3f));

    // Up to down goes through nothing: the up hint ends, fades, and only then does down begin.
    Filter s;
    Run(s, Want(UP, 4), 1.5f);
    bool  both = false, seen_down = false;
    Dir   last = UP;
    for (int i = 0; i < 400; ++i) {
        const Hint z = s.step(Want(DOWN, 2), 0.02f, 600.0f);
        if (z.level > 0 && z.dir == DOWN) seen_down = true;
        if (z.level > 0 && z.dir == UP && seen_down) both = true;  // flipped back
        if (z.level > 0) last = z.dir;
    }
    CHECK(seen_down && !both && last == DOWN, "ends up down");
    Filter u;
    Run(u, Want(UP, 4), 1.5f);
    bool zero_seen = false, direct = false, down_later = false;
    for (int i = 0; i < 400; ++i) {
        const Hint z = u.step(Want(DOWN, 4), 0.02f, 600.0f);
        if (z.level == 0) zero_seen = true;
        if (z.level > 0 && z.dir == DOWN) (zero_seen ? down_later : direct) = true;
    }
    CHECK(!direct && down_later, "up to down only through nothing: the up hint fades to zero first");

    // The place follows the latest reading while it shows.
    Filter pl;
    Run(pl, Want(UP, 4, 100.0f), 1.0f);
    const Hint moved = Run(pl, Want(UP, 4, 112.0f), 0.1f);
    CHECK(std::fabs(moved.place_m - 112.0f) < 0.01f, "the hint tracks the reading (%f)", double(moved.place_m));
    CHECK(moved.lap_m == 600.0f, "and carries the lap");

    // A bad frame length (a pause, a long hitch) doesn't jump it.
    Filter hitch;
    Run(hitch, Want(UP, 4), 1.0f);
    const Hint hh = hitch.step(Reading{}, 30.0f, 600.0f);
    CHECK(hh.level > 0.5f, "30 s of hitch counts as a quarter second");
}

static void TheAir() {
    AirTracker a;
    CHECK(!a.update(false, 0, 0, 0.02f), "no shocks known: never in the air");
    for (int i = 0; i < 4; ++i) CHECK(!a.update(true, 0.01f, 0.01f, 0.02f), "0.08 s of it is not enough");
    bool air = false;
    for (int i = 0; i < 4; ++i) air = a.update(true, 0.01f, 0.01f, 0.02f);
    CHECK(air, "0.16 s fully extended: in the air");
    CHECK(a.update(true, 0.5f, 0.0f, 0.02f), "a touch of the front shock isn't the ground yet");
    for (int i = 0; i < 6; ++i) air = a.update(true, 0.5f, 0.3f, 0.02f);
    CHECK(!air, "0.12 s of load: landed");
    AirTracker b;
    for (int i = 0; i < 100; ++i) CHECK(!b.update(true, 0.01f, 0.30f, 0.02f), "the rear still loaded: on the ground");
}

// --- the glyph ------------------------------------------------------------------------------

// A ribbon along +x from `m0`, rows every half metre, left edge at z = +0.35, on flat ground at y.
static std::vector<coachline::Vert> Strip(float m0, float len, float y = 0.0f, float lap = 600.0f) {
    std::vector<coachline::Vert> v;
    for (float a = 0; a <= len; a += 0.5f) {
        coachline::Vert l, r;
        l.x = r.x = m0 + a;
        l.z       = 0.35f;
        r.z       = -0.35f;
        l.y = r.y = y;
        l.s = r.s = a;
        l.m = r.m = std::fmod(m0 + a, lap);
        for (int k = 0; k < 4; ++k) l.rgba[k] = r.rgba[k] = 1.0f;
        v.push_back(l);
        v.push_back(r);
    }
    return v;
}

// Area of triangles standing across the line (in y and z, the sign's plane).
static float Area(const std::vector<coachline::Vert>& t, size_t from, size_t to) {
    float a = 0;
    for (size_t i = from; i + 3 <= to; i += 3) {
        const auto &p = t[i], &q = t[i + 1], &r = t[i + 2];
        a += std::fabs((q.y - p.y) * (r.z - p.z) - (r.y - p.y) * (q.z - p.z)) * 0.5f;
    }
    return a;
}

static Hint Shown(Dir d, int target, float place) {
    Hint h;
    h.dir = d, h.target = target, h.level = 1.0f, h.place_m = place, h.lap_m = 600.0f;
    return h;
}

static void TheDigits() {
    // Seven segments, none on another: the area is the strokes'.
    const float w = 0.9f, h = 2.0f, t = 0.24f;
    for (int d = 0; d <= 9; ++d) CHECK(!DigitRects(d, w, h, t).empty(), "digit %d", d);
    CHECK(DigitRects(10, w, h, t).empty() && DigitRects(-1, w, h, t).empty(), "only 0 to 9");
    CHECK(DigitRects(1, w, h, t).size() == 2 && DigitRects(8, w, h, t).size() == 7 && DigitRects(7, w, h, t).size() == 3,
          "1 is two strokes, 8 seven, 7 three");
    for (int d = 0; d <= 9; ++d) {
        const std::vector<Rect2> rs = DigitRects(d, w, h, t);
        for (size_t i = 0; i < rs.size(); ++i) {
            CHECK(rs[i].x0 >= -1e-5f && rs[i].x1 <= w + 1e-5f && rs[i].y0 >= -1e-5f && rs[i].y1 <= h + 1e-5f, "digit %d in its box", d);
            CHECK(rs[i].x1 > rs[i].x0 && rs[i].y1 > rs[i].y0, "digit %d: a real rectangle", d);
            for (size_t j = i + 1; j < rs.size(); ++j) {
                const bool apart = rs[i].x1 <= rs[j].x0 + 1e-5f || rs[j].x1 <= rs[i].x0 + 1e-5f ||
                                   rs[i].y1 <= rs[j].y0 + 1e-5f || rs[j].y1 <= rs[i].y0 + 1e-5f;
                CHECK(apart, "digit %d: strokes %zu and %zu overlap", d, i, j);
            }
        }
    }
}

static void TheMarks() {
    const std::vector<coachline::Vert> strip = Strip(100.0f, 60.0f);
    CHECK(Marks(strip, Hint{}).empty(), "no hint, no marks");
    Hint gone = Shown(UP, 4, 130.0f);
    gone.level = 0;
    CHECK(Marks(strip, gone).empty(), "faded out");
    CHECK(Marks(strip, Shown(UP, 4, 300.0f)).empty(), "the place is off the ribbon");
    CHECK(Marks(strip, Shown(UP, 10, 130.0f)).empty() && Marks(strip, Shown(UP, 4, 130.0f)).size() % 3 == 0, "whole triangles only");
    CHECK(Marks({}, Shown(UP, 4, 130.0f)).empty(), "no ribbon");

    const std::vector<coachline::Vert> up = Marks(strip, Shown(UP, 4, 130.0f));
    const std::vector<coachline::Vert> dn = Marks(strip, Shown(DOWN, 4, 130.0f));
    // The plate (two triangles), the stem (two), the arrow (one) and the four's strokes (four
    // rectangles).
    const size_t kArrow = 4 * 3, kDigit = 5 * 3;
    CHECK(up.size() == (2 + 2 + 1 + 2 * DigitRects(4, kDigitW, kGlyphH, kStroke).size()) * 3, "triangles: %zu", up.size());
    CHECK(dn.size() == up.size(), "the same for down");
    // It stands on the line at 130 m: across the line within the sign's half-width, no further
    // along it, from the ground to the top of the sign.
    const float hz = (kArrowW + kGlyphGap + kDigitW) * 0.5f + kPlatePad + 0.01f;
    for (const coachline::Vert& v : up) {
        CHECK(std::fabs(v.x - 130.0f) < 1e-4f && std::fabs(v.z) <= hz, "vertex at %f, %f", double(v.x), double(v.z));
        CHECK(v.y >= kMarkLift - 1e-5f && v.y <= kSignBase + kGlyphH + kPlatePad + 1e-4f, "height %f", double(v.y));
        CHECK(v.rgba[3] > 0 && v.rgba[3] <= 1, "alpha");
    }
    // The plate first, black; the rest violet for up and orange for down.
    CHECK(up[0].rgba[0] == 0 && up[0].rgba[3] < up[kArrow].rgba[3], "a dark plate under it");
    CHECK(up[kArrow].rgba[0] == kUpColour[0] && up[kArrow].rgba[2] == kUpColour[2], "up is violet");
    CHECK(dn[kArrow].rgba[0] == kDownColour[0] && dn[kArrow].rgba[1] == kDownColour[1], "down is orange");
    // The arrow points up for up (one vertex above the other two) and down for down.
    auto above = [&](const std::vector<coachline::Vert>& t, int& hi, int& lo) {
        float mid = 0;
        for (size_t i = kArrow; i < kArrow + 3; ++i) mid += t[i].y / 3;
        hi = lo = 0;
        for (size_t i = kArrow; i < kArrow + 3; ++i) (t[i].y > mid ? hi : lo)++;
    };
    int hi, lo;
    above(up, hi, lo);
    CHECK(hi == 1 && lo == 2, "up: the tip is on top");
    above(dn, hi, lo);
    CHECK(hi == 2 && lo == 1, "down: the tip is at the bottom");
    // The arrow is on the rider's left (z > 0) and the digit on the right (z < 0).
    float az = 0, dz = 0;
    for (size_t i = kArrow; i < kArrow + 3; ++i) az += up[i].z / 3;
    size_t nd = 0;
    for (size_t i = kDigit; i < up.size(); ++i) dz += up[i].z, ++nd;
    CHECK(az > 0.2f && dz / float(nd) < -0.1f, "arrow left, digit right (%f, %f)", double(az), double(dz / float(nd)));
    // The digit's area is its strokes'.
    float strokes = 0;
    for (const Rect2& r : DigitRects(4, kDigitW, kGlyphH, kStroke)) strokes += (r.x1 - r.x0) * (r.y1 - r.y0);
    CHECK(std::fabs(Area(up, kDigit, up.size()) - strokes) < 1e-3f, "the digit is drawn as itself: %f vs %f",
          double(Area(up, kDigit, up.size())), double(strokes));
    // Every digit 1 to 9 draws.
    for (int d = 1; d <= 9; ++d) CHECK(Marks(strip, Shown(UP, d, 130.0f)).size() > kDigit, "digit %d", d);
    // The strength scales the alpha.
    Hint half = Shown(UP, 4, 130.0f);
    half.level = 0.5f;
    CHECK(Marks(strip, half)[kArrow].rgba[3] < up[kArrow].rgba[3] * 0.6f, "half strength, half the alpha");
    // Across the line: a place past the lap's end is found by its wrapped distance.
    const std::vector<coachline::Vert> wrap = Strip(580.0f, 60.0f, 0.0f, 600.0f);
    const std::vector<coachline::Vert> across = Marks(wrap, Shown(UP, 4, 5.0f));
    CHECK(!across.empty() && std::fabs(across[0].x - 605.0f) < 1e-3f, "found at 5 m, past the line, on the ribbon at x = 605");
    // Where the ground rises, the sign stands on it.
    const std::vector<coachline::Vert> high = Strip(100.0f, 60.0f, 3.0f);
    CHECK(std::fabs(Marks(high, Shown(UP, 4, 130.0f))[0].y - (3.0f + kSignBase - kPlatePad)) < 1e-4f, "stands at the ribbon's height");
    // Nothing that isn't a number gets through.
    std::vector<coachline::Vert> nan = strip;
    for (coachline::Vert& v : nan) v.y = NAN;
    CHECK(Marks(nan, Shown(UP, 4, 130.0f)).empty(), "NaN ground: nothing");
}

// --- the badge beside the cue box ------------------------------------------------------------

static void TheBadge() {
    coachcue::Cue cue;
    cue.text = "BRAKE";
    coachhud::View v;
    v.set.enabled = true;
    v.set.cue     = true;
    v.cue         = &cue;
    coachhud::Frame off;
    coachhud::Build(v, off);

    v.set.gear    = true;
    v.gear_dir    = 1;
    v.gear_target = 4;
    coachhud::Frame on;
    coachhud::Build(v, on);
    CHECK(on.quads.size() == off.quads.size() + 2, "a backing and an arrow (%zu vs %zu)", on.quads.size(), off.quads.size());
    CHECK(on.texts.size() == off.texts.size() + 1 && on.texts.back().s == "4", "and the gear");

    const coachhud::Box cue_box = coachhud::CueBoxAt(v.set.cue_x, v.set.cue_y);
    const coachhud::Quad& back = on.quads[off.quads.size()];
    CHECK(back.p[0][0] >= cue_box.x1 && back.p[0][1] == cue_box.y0, "beside the cue box, level with it");
    CHECK(on.texts.back().color == coachhud::kGearUp && on.quads.back().color == coachhud::kGearUp, "violet for up");

    v.gear_dir = -1;
    v.gear_target = 2;
    coachhud::Frame down;
    coachhud::Build(v, down);
    CHECK(down.texts.back().s == "2" && down.texts.back().color == coachhud::kGearDown, "orange for down");
    // The arrow's tip: up has one vertex above the other two, down one below.
    const coachhud::Quad& qa = on.quads.back();
    const coachhud::Quad& qd = down.quads.back();
    CHECK(qa.p[0][1] < qa.p[1][1] && qa.p[1][1] == qa.p[2][1], "up: tip on top");
    CHECK(qd.p[1][1] > qd.p[0][1] && qd.p[0][1] == qd.p[3][1], "down: tip at the bottom");

    // Off: nothing. Not asked for: nothing.
    v.set.gear = false;
    coachhud::Frame none;
    coachhud::Build(v, none);
    CHECK(none.quads.size() == off.quads.size() && none.texts.size() == off.texts.size(), "off until asked for");
    v.set.gear = true;
    v.gear_dir = 0;
    coachhud::Frame quiet;
    coachhud::Build(v, quiet);
    CHECK(quiet.quads.size() == off.quads.size(), "no hint, no badge");
    v.gear_dir = 1, v.gear_target = 0;
    coachhud::Frame bad;
    coachhud::Build(v, bad);
    CHECK(bad.quads.size() == off.quads.size(), "no such gear, no badge");
    // Dragged to the right edge, it stays on screen.
    v.gear_target = 4;
    v.set.cue_x   = 1.0f;
    coachhud::Frame edge;
    coachhud::Build(v, edge);
    for (const coachhud::Quad& q : edge.quads)
        for (int k = 0; k < 4; ++k) CHECK(q.p[k][0] >= 0 && q.p[k][0] <= 1.0f, "quad x %f on screen", double(q.p[k][0]));
}

// --- the preview scene -----------------------------------------------------------------------

// A rider coming up to Coach's shift from fifth gear: the line, the glyph and the badge, as text
// tools/gear_preview.py reads. The ground is flat and the lap straight; the viewer bends it.
static void Dump(const char* path, int gear, const char* cue_text) {
    const Lap     l = MakeLap();
    const Profile p = BuildProfile(l.ref, l.gear);
    const float   phase = 171.0f;  // 30 m before the shift up to fourth
    const Reading r = Read(p, phase, gear, 22.0f, false, false);
    Filter        f;
    Hint          h;
    for (int i = 0; i < 60; ++i) h = f.step(r, 0.02f, p.lap_m);
    const std::vector<coachline::Vert> strip = Strip(phase, 60.0f);
    const std::vector<coachline::Vert> marks = Marks(strip, h);
    FILE* o = std::fopen(path, "w");
    if (!o) return;
    std::fprintf(o, "# phase=%.0f gear=%d target=%d dir=%d ahead=%.0f\n", double(phase), gear, r.target, int(r.dir), double(r.ahead_m));
    for (size_t i = 0; i + 1 < strip.size(); i += 2) std::fprintf(o, "row %.2f %.3f\n", double(strip[i].x - phase), double(strip[i].y));
    for (size_t i = 0; i + 3 <= marks.size(); i += 3) {
        for (size_t k = 0; k < 3; ++k) {
            const coachline::Vert& v = marks[i + k];
            std::fprintf(o, "mark %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n", double(v.x - phase), double(v.y), double(v.z),
                         double(v.rgba[0]), double(v.rgba[1]), double(v.rgba[2]), double(v.rgba[3]));
        }
    }
    coachcue::Cue cue;
    cue.text = cue_text;
    coachhud::View v;
    v.set.enabled = v.set.cue = v.set.gear = true;
    v.cue         = &cue;
    v.gear_dir    = int(h.dir);
    v.gear_target = h.target;
    coachhud::Frame fr;
    coachhud::Build(v, fr);
    for (const coachhud::Quad& q : fr.quads)
        std::fprintf(o, "hq %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %08X\n", double(q.p[0][0]), double(q.p[0][1]), double(q.p[1][0]),
                     double(q.p[1][1]), double(q.p[2][0]), double(q.p[2][1]), double(q.p[3][0]), double(q.p[3][1]), q.color);
    for (const coachhud::Text& t : fr.texts)
        std::fprintf(o, "ht %.4f %.4f %.4f %d %08X %s\n", double(t.x), double(t.y), double(t.size), t.justify, t.color, t.s.c_str());
    std::fclose(o);
}

int main(int argc, char** argv) {
    TheSheetCarriesTheGear();
    TheSetting();
    TheShifts();
    UpOrDown();
    Hysteresis();
    TheAir();
    TheDigits();
    TheMarks();
    TheBadge();
    if (argc >= 3 && std::strcmp(argv[1], "--dump") == 0) {
        const std::string dir = argv[2];
        Dump((dir + "/gear-up.txt").c_str(), 2, "SHIFT UP");
        Dump((dir + "/gear-down.txt").c_str(), 6, "SHIFT DOWN");
    }
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("coachgear: all checks passed\n");
    return 0;
}
