// coachgear.h - gear hints on Coach's ground line: where Coach's reference lap changes gear, and
// whether the rider is in the gear it changed to.
//
// The sheet's GEAR chunk (coachhud::Sheet::gear) says which gear the reference lap was in at each
// point. From it come the shifts - a change that holds for kMinSegM metres, so a blip through a
// gear on the way to another isn't one - each with the gear before and after. Then, every
// telemetry sample, one question: for the nearest shift ahead (or just behind), is the rider in a
// gear that is neither of the two? Ahead of the shift the rider may be in the old gear (they
// haven't got there yet) or the new one (early is fine); any other gear is a mismatch and the
// hint says the new one. At the shift point and for kPastM after it, only the new gear is right,
// so a rider still in the old gear is told to change. The hint is an arrow and the target gear
// ("▲3" shift up to 3, "▼2" down to 2), drawn on the line kLeadM before the shift (never closer
// than kMinPlaceM to the rider) as a small sign standing over it, facing the rider - flat on the
// ground a glyph two metres long is a few pixels tall from the saddle - and a small badge beside
// the cue box.
//
// Quiet on purpose: nothing while crashed, in the air (unless Coach's shift itself is in the air:
// a jump's shift is on the face or in the air, and the rider needs it there), crawling or in
// neutral, and a hint needs kHoldInS to start, kHoldOutS of not being wanted to end, shows at
// least kMinShowS, fades at a limited rate, and goes from up to down only through nothing.
//
// Pure maths, no Win32 and no GL: tests/coachgear_test.cpp runs it. mxbcoach.cpp feeds it and
// draws what it returns. Read-only toward the game, like the rest of the line.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "coachhud.h"
#include "coachline.h"

namespace coachgear {

// --- thresholds ---------------------------------------------------------------------------

constexpr float kMinSegM    = 5.0f;   // a reference gear must hold this long to count as a shift
constexpr float kLookM      = 50.0f;  // a shift this far ahead of the rider is the one looked at
constexpr float kPastM      = 12.0f;  // and one this far behind, while the rider isn't in its gear
constexpr float kLeadM      = 6.0f;   // the hint is drawn this far before the shift...
constexpr float kMinPlaceM  = 8.0f;   // ...but never closer than this to the rider
constexpr float kMinSpeed   = 3.0f;   // m/s: slower than this (a start, a stall), nothing is said
constexpr float kRefAirM    = 1.3f;   // the reference bike this far over the track's ground: airborne
constexpr float kHoldInS    = 0.30f;  // wanted this long to start
constexpr float kHoldOutS   = 0.70f;  // and not wanted this long to end
constexpr float kMinShowS   = 1.20f;  // a hint shows at least this long
constexpr float kFadeRate   = 4.0f;   // drawing strength per second, either way
constexpr int   kMaxGear    = 9;

// The rider is in the air when both shocks are fully extended: under kAirEnter used for
// kAirEnterS to start, over kAirExit on either for kAirExitS to end.
constexpr float kAirEnter  = 0.04f;
constexpr float kAirExit   = 0.10f;
constexpr float kAirEnterS = 0.12f;
constexpr float kAirExitS  = 0.10f;

enum Dir : int { NONE = 0, UP = 1, DOWN = -1 };

// Drawn colours: on none of the line's own (green, white, yellow, red) and not the pace hints'
// (magenta, cyan).
constexpr float kUpColour[3]   = {0.65f, 0.45f, 1.00f};  // violet
constexpr float kDownColour[3] = {1.00f, 0.55f, 0.10f};  // orange
constexpr float kPlateColour[3] = {0.0f, 0.0f, 0.0f};
constexpr float kMarkAlpha     = 0.95f;
constexpr float kPlateAlpha    = 0.50f;
constexpr float kMarkLift      = 0.02f;  // the stem's foot, over the ribbon

// --- Coach's lap, once per sheet ------------------------------------------------------------

struct Shift {
    size_t idx = 0;      // the first sheet point in the new gear
    float  m   = 0;      // metres along Coach's line (the ribbon's Vert::m) where it happens
    int    from = 0, to = 0;
    bool   air  = false; // the reference bike was in the air there
};

struct Profile {
    std::vector<float>   arc;      // metres along Coach's line at each point (as the ribbon counts them)
    std::vector<uint8_t> gear;     // Coach's gear at each point, 0 = unknown / neutral
    std::vector<Shift>   shifts;   // by m
    float                lap_m = 0;
    bool ok() const { return arc.size() >= 8 && gear.size() == arc.size() && lap_m > 50.0f && !shifts.empty(); }
};

/// Metres along Coach's line at each sheet point, and round the whole of it: counted exactly as
/// coachline::Ribbon counts them, so a Vert::m is a place on this.
inline std::vector<float> Arc(const std::vector<coachhud::RefPoint>& ref, float& lap_m) {
    const size_t       n = ref.size();
    std::vector<float> arc(n, 0.0f);
    lap_m = 0;
    if (n < 2) return arc;
    for (size_t k = 1; k < n; ++k) {
        const float d = std::hypot(ref[k].x - ref[k - 1].x, ref[k].z - ref[k - 1].z);
        arc[k]        = arc[k - 1] + (d < coachline::kMaxGapM ? d : 0.0f);
    }
    const float close = std::hypot(ref[0].x - ref[n - 1].x, ref[0].z - ref[n - 1].z);
    lap_m             = arc[n - 1] + (close < coachline::kMaxGapM ? close : 0.0f);
    return arc;
}

/// The shifts in Coach's lap. `gear` is the sheet's GEAR (one per point, 0 = unknown); `ground`
/// is the track's own height at each point and `refy` Coach's bike height (either may be empty:
/// then nothing is called airborne). Neutral points are skipped over, not shifts. The lap is a
/// circle: the gear it ends in is the one it starts in.
inline Profile BuildProfile(const std::vector<coachhud::RefPoint>& ref, const std::vector<uint8_t>& gear,
                            const std::vector<float>& ground = {}, const std::vector<float>& refy = {}) {
    Profile      p;
    const size_t n = ref.size();
    if (n < 8 || gear.size() != n) return p;
    p.arc  = Arc(ref, p.lap_m);
    p.gear = gear;
    for (uint8_t& g : p.gear)
        if (g > kMaxGear) g = 0;
    // The gear the lap comes into the line in: the last one it had.
    int cur = 0;
    for (size_t i = n; i-- > 0;)
        if (p.gear[i]) {
            cur = p.gear[i];
            break;
        }
    if (!cur) return p;
    const bool air_known = ground.size() == n && refy.size() == n;
    for (size_t i = 0; i < n; ++i) {
        const int g = p.gear[i];
        if (!g || g == cur) continue;
        // Does the new gear hold? Look on, across neutral points, for kMinSegM metres; a point in
        // another gear ends the look and this one isn't a shift.
        bool  holds = false;
        float run   = 0;
        for (size_t k = i + 1; k <= i + n; ++k) {
            const size_t j = k % n;
            const float  d = p.arc[j] >= p.arc[(k - 1) % n] ? p.arc[j] - p.arc[(k - 1) % n] : 0.0f;
            run += d;
            const int gj = p.gear[j];
            if (gj && gj != g) break;
            if (run >= kMinSegM) {
                holds = true;
                break;
            }
        }
        if (!holds) continue;
        Shift s;
        s.idx  = i;
        s.m    = p.arc[i];
        s.from = cur;
        s.to   = g;
        s.air  = air_known && std::isfinite(ground[i]) && std::isfinite(refy[i]) && refy[i] - ground[i] > kRefAirM;
        p.shifts.push_back(s);
        cur = g;
    }
    return p;
}

/// Shortest signed distance from `from` to `to` round a lap of `lap` metres: (-lap/2, lap/2].
inline float Wrap(float to, float from, float lap) {
    float d = std::fmod(to - from, lap);
    if (d > lap * 0.5f) d -= lap;
    if (d <= -lap * 0.5f) d += lap;
    return d;
}

/// The rider's place on Coach's line: point `idx`, `frac` of the way to the next.
inline float PhaseAt(const Profile& p, const std::vector<coachhud::RefPoint>& ref, int idx, float frac) {
    if (!p.ok() || idx < 0 || size_t(idx) >= p.arc.size() || ref.size() != p.arc.size()) return NAN;
    const size_t n = ref.size(), a = size_t(idx), b = (a + 1) % n;
    const float  len = std::hypot(ref[b].x - ref[a].x, ref[b].z - ref[a].z);
    return p.arc[a] + (std::max)(0.0f, (std::min)(1.0f, frac)) * (len < coachline::kMaxGapM ? len : 0.0f);
}

// --- what the rider needs to hear -------------------------------------------------------------

/// One look at the rider against Coach's shifts.
struct Reading {
    Dir   dir     = NONE;
    int   target  = 0;      // the gear to be in
    int   shift   = -1;     // which of Profile::shifts
    float place_m = 0;      // where on Coach's line the hint is drawn
    float ahead_m = 0;      // the shift's distance ahead of the rider (negative: just passed)
};

/// The shift the rider should be told about, or none. `gear` is the rider's (0 = neutral or
/// unknown), `phase_m` their place on Coach's line (PhaseAt).
inline Reading Read(const Profile& p, float phase_m, int gear, float speed, bool crashed, bool in_air) {
    Reading r;
    if (!p.ok() || crashed || gear <= 0 || gear > kMaxGear || !std::isfinite(phase_m) || !(speed >= kMinSpeed)) return r;
    // Nearest first: the shifts from kPastM behind to kLookM ahead, in the order the rider meets them.
    struct Cand {
        float  d;
        size_t i;
    };
    std::vector<Cand> c;
    for (size_t i = 0; i < p.shifts.size(); ++i) {
        const float d = Wrap(p.shifts[i].m, phase_m, p.lap_m);
        if (d >= -kPastM && d <= kLookM) c.push_back({d, i});
    }
    std::sort(c.begin(), c.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
    for (const Cand& k : c) {
        const Shift& s = p.shifts[k.i];
        if (in_air && !s.air) continue;  // in the air, only a shift Coach made in the air
        // Ahead of the shift either gear is on Coach's line; at it and past it, only the new one.
        const bool wrong = k.d > 0 ? (gear != s.from && gear != s.to) : gear != s.to;
        if (!wrong) continue;
        r.dir     = s.to > gear ? UP : DOWN;
        r.target  = s.to;
        r.shift   = int(k.i);
        r.ahead_m = k.d;
        r.place_m = phase_m + (std::max)(k.d - kLeadM, kMinPlaceM);
        r.place_m = std::fmod(r.place_m, p.lap_m);
        if (r.place_m < 0) r.place_m += p.lap_m;
        return r;
    }
    return r;
}

/// What is drawn this sample.
struct Hint {
    Dir   dir     = NONE;  // kept while it fades out
    int   target  = 0;
    float level   = 0;     // 0..1 drawing strength
    float place_m = 0;     // where on Coach's line
    float lap_m   = 0;
};

/// Turns readings into a hint that doesn't flicker.
class Filter {
public:
    void reset() { *this = Filter{}; }
    /// `lap_m` is Profile::lap_m, for the hint to carry. Call once per telemetry sample.
    Hint step(const Reading& r, float dt, float lap_m) {
        dt = (std::max)(0.0f, (std::min)(dt, 0.25f));
        if (showing_) {
            shown_t_ += dt;
            if (r.dir == dir_ && r.target == target_) {
                absent_t_ = 0;
                place_m_  = r.place_m;
            } else {
                absent_t_ += dt;
                if (absent_t_ >= kHoldOutS && shown_t_ >= kMinShowS) showing_ = false;
            }
        }
        if (!showing_) {
            if (r.dir != NONE && r.dir == pend_dir_ && r.target == pend_target_) {
                pend_t_ += dt;
                pend_place_ = r.place_m;
                // Up to down (or a new target) only after the old hint has faded out.
                if (pend_t_ >= kHoldInS && level_ <= 0.0f) {
                    showing_  = true;
                    dir_      = r.dir;
                    target_   = r.target;
                    place_m_  = r.place_m;
                    shown_t_  = absent_t_ = 0;
                    pend_t_   = 0;
                }
            } else {
                pend_dir_    = r.dir;
                pend_target_ = r.target;
                pend_t_      = 0;
                pend_place_  = r.place_m;
            }
        }
        const float to = showing_ ? 1.0f : 0.0f;
        level_ += (std::max)(-kFadeRate * dt, (std::min)(kFadeRate * dt, to - level_));
        Hint h;
        if (level_ > 0.0f) {
            h.dir     = dir_;
            h.target  = target_;
            h.level   = level_;
            h.place_m = place_m_;
            h.lap_m   = lap_m;
        } else {
            dir_ = NONE, target_ = 0;
        }
        return h;
    }
    bool showing() const { return showing_; }

private:
    bool  showing_ = false;
    Dir   dir_ = NONE, pend_dir_ = NONE;
    int   target_ = 0, pend_target_ = 0;
    float place_m_ = 0, pend_place_ = 0;
    float level_ = 0, shown_t_ = 0, absent_t_ = 0, pend_t_ = 0;
};

/// Whether the rider is in the air, from how much of each shock's travel is in use (0 fully
/// extended). Needs both shocks to be known; a bike without them is never in the air.
class AirTracker {
public:
    void reset() { *this = AirTracker{}; }
    bool update(bool known, float front_used, float rear_used, float dt) {
        if (!known) {
            reset();
            return false;
        }
        dt = (std::max)(0.0f, (std::min)(dt, 0.25f));
        if (!air_) {
            t_ = (front_used < kAirEnter && rear_used < kAirEnter) ? t_ + dt : 0.0f;
            if (t_ >= kAirEnterS) air_ = true, t_ = 0;
        } else {
            t_ = (front_used > kAirExit || rear_used > kAirExit) ? t_ + dt : 0.0f;
            if (t_ >= kAirExitS) air_ = false, t_ = 0;
        }
        return air_;
    }
    bool in_air() const { return air_; }

private:
    bool  air_ = false;
    float t_   = 0;
};

// --- the glyph, a sign standing on the line ------------------------------------------------------

constexpr float kSignBase   = 0.5f;  // metres from the ground to the foot of the sign
constexpr float kGlyphH     = 1.8f;   // the sign's height
constexpr float kArrowW     = 1.2f;   // the arrow, across the line
constexpr float kDigitW     = 0.9f;
constexpr float kGlyphGap   = 0.4f;
constexpr float kStroke     = 0.24f;  // the digit's stroke
constexpr float kPlatePad   = 0.2f;
constexpr float kStemW      = 0.06f;

struct Rect2 {
    float x0, y0, x1, y1;  // x to the right, y up, in the digit's own frame
};

/// The rectangles of a seven-segment digit `w` x `h`, none overlapping another (so a blended
/// colour doesn't double up where two meet). Empty for anything but 0..9.
inline std::vector<Rect2> DigitRects(int digit, float w, float h, float t) {
    static const uint8_t kSeg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};  // bit 0 = a .. 6 = g
    std::vector<Rect2>   out;
    if (digit < 0 || digit > 9) return out;
    const uint8_t m = kSeg[digit];
    const float   mid = h * 0.5f;
    if (m & 0x01) out.push_back({0, h - t, w, h});                    // a: top
    if (m & 0x02) out.push_back({w - t, mid + t * 0.5f, w, h - t});   // b: upper right
    if (m & 0x04) out.push_back({w - t, t, w, mid - t * 0.5f});       // c: lower right
    if (m & 0x08) out.push_back({0, 0, w, t});                        // d: bottom
    if (m & 0x10) out.push_back({0, t, t, mid - t * 0.5f});           // e: lower left
    if (m & 0x20) out.push_back({0, mid + t * 0.5f, t, h - t});       // f: upper left
    if (m & 0x40) out.push_back({0, mid - t * 0.5f, w, mid + t * 0.5f});  // g: middle
    return out;
}

/// The hint as triangles (three Verts each; the dark plate first): a sign standing on the line
/// at `h.place_m`, facing the rider, on a thin stem from the ground - found by the ribbon's own
/// Vert::m, so it stays put in the world as the rider rides up to it. The arrow is on the left as
/// the rider sees it and the digit on the right. An arrow up points up, an arrow down points down.
inline std::vector<coachline::Vert> Marks(const std::vector<coachline::Vert>& strip, const Hint& h) {
    std::vector<coachline::Vert> out;
    const size_t rows = strip.size() / 2;
    if (h.dir == NONE || h.level <= 0 || rows < 2 || !(h.lap_m > 0) || h.target < 0 || h.target > 9) return out;
    // The row pair the place lies between, by Vert::m (rows run in order of m, wrapping once).
    struct Frame {
        float cx, cz, nx, nz, y, s;
    };
    Frame f{};
    bool  found = false;
    for (size_t r = 0; r + 1 < rows && !found; ++r) {
        const coachline::Vert &L0 = strip[r * 2], &R0 = strip[r * 2 + 1], &L1 = strip[(r + 1) * 2],
                              &R1 = strip[(r + 1) * 2 + 1];
        const float d0 = Wrap(L0.m, h.place_m, h.lap_m), d1 = Wrap(L1.m, h.place_m, h.lap_m);
        if (!(d0 <= 0 && d1 >= 0) || d1 - d0 > 3.0f) continue;  // a row pair spans half a metre; more is a gap
        const float u = d1 - d0 > 1e-5f ? -d0 / (d1 - d0) : 0.0f;
        const float lx = L0.x + (L1.x - L0.x) * u, lz = L0.z + (L1.z - L0.z) * u;
        const float rx = R0.x + (R1.x - R0.x) * u, rz = R0.z + (R1.z - R0.z) * u;
        float       nx = lx - rx, nz = lz - rz;
        const float nl = std::hypot(nx, nz);
        if (nl < 1e-4f) continue;
        f.cx = (lx + rx) * 0.5f, f.cz = (lz + rz) * 0.5f;
        f.nx = nx / nl, f.nz = nz / nl;
        f.y  = ((L0.y + R0.y + L1.y + R1.y) * 0.25f);
        f.s  = L0.s + (L1.s - L0.s) * u;
        found = true;
    }
    if (!found || !std::isfinite(f.y) || !std::isfinite(f.cx) || !std::isfinite(f.cz)) return out;
    const float* col   = h.dir == UP ? kUpColour : kDownColour;
    const float  alpha = kMarkAlpha * h.level * (std::max)(0.6f, coachline::Fade(f.s));
    const float  plate = kPlateAlpha * h.level;
    // A point `up` metres over the ground and `across` to the left of the line.
    auto put = [&](float up, float across, const float* c, float a) {
        coachline::Vert v;
        v.x = f.cx + f.nx * across;
        v.z = f.cz + f.nz * across;
        v.y = f.y + up;
        v.s = f.s;
        v.rgba[0] = c[0], v.rgba[1] = c[1], v.rgba[2] = c[2], v.rgba[3] = a;
        v.m = h.place_m;
        out.push_back(v);
    };
    auto quad = [&](float a0, float c0, float a1, float c1, const float* c, float a) {  // axis-aligned in (up, across)
        put(a0, c0, c, a), put(a1, c0, c, a), put(a1, c1, c, a);
        put(a0, c0, c, a), put(a1, c1, c, a), put(a0, c1, c, a);
    };
    const float W   = kArrowW + kGlyphGap + kDigitW;
    const float bot = kSignBase;
    // The plate under it all, and the stem it stands on.
    quad(bot - kPlatePad, -W * 0.5f - kPlatePad, bot + kGlyphH + kPlatePad, W * 0.5f + kPlatePad, kPlateColour, plate);
    quad(kMarkLift, -kStemW * 0.5f, bot - kPlatePad, kStemW * 0.5f, col, alpha);
    // The arrow, at the left (across > 0): a triangle as wide as kArrowW and 60% of the sign tall.
    const float ah = kGlyphH * 0.6f, ac = bot + kGlyphH * 0.5f;
    const float l0 = W * 0.5f, l1 = W * 0.5f - kArrowW, lm = (l0 + l1) * 0.5f;
    if (h.dir == UP)
        put(ac + ah * 0.5f, lm, col, alpha), put(ac - ah * 0.5f, l0, col, alpha), put(ac - ah * 0.5f, l1, col, alpha);
    else
        put(ac - ah * 0.5f, lm, col, alpha), put(ac + ah * 0.5f, l1, col, alpha), put(ac + ah * 0.5f, l0, col, alpha);
    // The digit, at the right. Its x runs left to right as the rider sees it, which is across
    // falling from the digit's left edge.
    const float left = -W * 0.5f + kDigitW;
    for (const Rect2& r : DigitRects(h.target, kDigitW, kGlyphH, kStroke))
        quad(bot + r.y0, left - r.x0, bot + r.y1, left - r.x1, col, alpha);
    return out;
}

}  // namespace coachgear
