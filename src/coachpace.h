// coachpace.h - pace hints on Coach's ground line: is the rider coming in too fast or too slow
// for what is ahead, compared with Coach's own lap at the same spot?
//
// Two layers, as agreed with Sean. The line's own colours (coachline::LineColours: green gas,
// white coast, yellow light brake, red heavy brake) stay the "what to do" layer. Pace is drawn
// over it, separately:
//
//   TOO FAST  chevrons pointing back at the rider on the stretch ahead, up to the braking point,
//             and the braking colours of the look-ahead pulled toward the rider by about the
//             distance they would overshoot by (yellow and red come sooner).
//   TOO SLOW  chevrons pointing forward and the gas stretch a brighter green; and before a jump
//             lip that needs speed, a gate across the line at the lip and "MORE SPEED" on the HUD.
//
// What "too fast" means. Here: the rider's speed over Coach's (DRIV speed) at the same point,
// `rel`. Ahead: a braking-distance model. Coach braked from v_b at the start of the next braking
// zone down to v_c over d metres, a deceleration a = (v_b^2 - v_c^2) / 2d. Arriving at the zone
// still `rel` up on Coach and braking as hard as Coach did, the rider needs
// ((1+rel)^2 v_b^2 - v_c^2) / 2a metres: the excess is how far they will overshoot. Either one
// past its threshold (kEnterRel, kEnterOvershootM) counts. Too slow is `rel` alone, and is never
// said with a braking zone close ahead: "go faster" into a corner is the wrong thing to say.
//
// Never flickers: the score is smoothed (kTauS), a hint needs the score past its entry threshold
// for kHoldInS to start and back under the exit threshold (half of it) for kHoldOutS to end, it
// shows for at least kMinShowS, too fast and too slow can only follow each other through
// nothing, and what is drawn fades in and out at a limited rate.
//
// Pure maths, no Win32 and no GL: tests/coachpace_test.cpp runs it. mxbcoach.cpp feeds it and
// draws what it returns. Read-only toward the game, like the rest of the line.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "coachhud.h"
#include "coachline.h"

namespace coachpace {

// --- thresholds ---------------------------------------------------------------------------

constexpr float kEnterRel        = 0.08f;  // 8% over or under Coach's speed here
constexpr float kEnterOvershootM = 4.0f;   // or 4 m past Coach's slowest point of the next corner
constexpr float kExitFrac        = 0.5f;   // a hint ends back under half its entry threshold
constexpr float kTauS            = 0.4f;   // the score's smoothing
constexpr float kHoldInS         = 0.3f;   // past the threshold this long to start
constexpr float kHoldOutS        = 0.8f;   // and back under the exit this long to end
constexpr float kMinShowS        = 1.0f;   // a hint shows at least this long
constexpr float kFadeRate        = 2.5f;   // drawing strength per second, either way
constexpr float kMinSpeed        = 4.0f;   // m/s: slower than this, nothing is said
constexpr float kMinRefSpeed     = 5.0f;   // nor where Coach himself was this slow
constexpr float kLookS           = 3.0f;   // the stretch ahead looked at: three seconds of it,
constexpr float kLookMinM        = 20.0f;  //   at least this many metres
constexpr float kLookMaxM        = 60.0f;  //   and no further than the line is drawn
constexpr float kSlowQuietM      = 20.0f;  // no "faster" this close to a braking zone
constexpr float kQuietTone       = 1.5f;   // nor where Coach was off the gas and braking
constexpr float kMaxAdvanceM     = 25.0f;  // the braking colours come at most this much sooner
constexpr float kMinAdvanceM     = 6.0f;   // and at least this much, when too fast at all

// Jump lips, from the ground's height along the line (the sheet's TRRN): a face rising at least
// kLipFace over the kLipSpanM before it that breaks over by kLipBreak within the kLipSpanM after
// it (a tabletop's flat top counts as a break), at least kLipRiseM higher than kLipBackM back,
// ridden by Coach at kLipMinSpeed or more. Lips closer than kLipMergeM are one jump.
constexpr float kLipSpanM    = 4.0f;
constexpr float kLipFace     = 0.18f;
constexpr float kLipBreak    = 0.2f;
constexpr float kLipBackM    = 8.0f;
constexpr float kLipRiseM    = 0.6f;
constexpr float kLipMinSpeed = 8.0f;
constexpr float kLipMergeM   = 15.0f;
constexpr float kLipShowMinM = 3.0f;   // the MORE SPEED mark shows from kLipShowMaxM out
constexpr float kLipShowMaxM = 60.0f;  // until the rider is this close to the lip

// --- Coach's lap, once per sheet ------------------------------------------------------------

struct Profile {
    std::vector<float>  s;     // metres along Coach's line from its first point
    std::vector<float>  v;     // Coach's speed, m/s
    std::vector<float>  tone;  // coachline::Tone, 0..3
    std::vector<size_t> lips;  // jump lips, point indices in lap order
    float               lap_m = 0;
    bool ok() const { return s.size() >= 8 && v.size() == s.size() && tone.size() == s.size() && lap_m > 50.0f; }
    /// Metres from point i to the next, wrapping the lap (0 across a gap the sheet doesn't join).
    float seg(size_t i) const { return i + 1 < s.size() ? s[i + 1] - s[i] : lap_m - s.back(); }
};

/// Where the jump lips are, from the ground's height at each point (NaN where unknown).
inline std::vector<size_t> FindLips(const std::vector<float>& s, const std::vector<float>& h,
                                    const std::vector<float>& v) {
    std::vector<size_t> out;
    const size_t        n = s.size();
    if (h.size() != n || v.size() != n || n < 3) return out;
    // The height `d` metres from point i along the line (either way), interpolated; NaN off it.
    auto at = [&](size_t i, float d) {
        const float target = s[i] + d;
        size_t      j      = i;
        if (d >= 0) {
            while (j + 1 < n && s[j + 1] < target) ++j;
            if (j + 1 >= n) return NAN;
        } else {
            while (j > 0 && s[j] > target) --j;
            if (s[j] > target) return NAN;
        }
        const float span = s[j + 1] - s[j];
        const float f    = span > 1e-4f ? (target - s[j]) / span : 0.0f;
        return h[j] + (h[j + 1] - h[j]) * f;
    };
    // Candidates closer than kLipMergeM to the previous one are the same jump: keep its sharpest.
    float  best_break = 0, last_s = -1e9f;
    size_t best       = n;
    auto   flush      = [&] {
        if (best < n) out.push_back(best);
        best = n, best_break = 0;
    };
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(h[i])) continue;
        const float before = at(i, -kLipSpanM), after = at(i, kLipSpanM), back = at(i, -kLipBackM);
        if (!std::isfinite(before) || !std::isfinite(after) || !std::isfinite(back)) continue;
        const float face   = (h[i] - before) / kLipSpanM;
        const float brk    = face - (after - h[i]) / kLipSpanM;
        const bool  is_lip = face >= kLipFace && brk >= kLipBreak && h[i] - back >= kLipRiseM && v[i] >= kLipMinSpeed;
        if (!is_lip) continue;
        if (s[i] - last_s > kLipMergeM) flush();
        last_s = s[i];
        if (brk > best_break) best_break = brk, best = i;
    }
    flush();
    return out;
}

/// Coach's lap as pace hints need it: distances, speeds, tone and lips. `drive` is the sheet's
/// DRIV (speed, throttle, brake per point); without it the speed comes from the sheet's own
/// times. `heights` is the ground along the line per point (NaN unknown); empty, no lips.
inline Profile BuildProfile(const std::vector<coachhud::RefPoint>& ref, const std::vector<float>& drive,
                            const std::vector<float>& heights = {}) {
    Profile     p;
    const size_t n = ref.size();
    if (n < 8) return p;
    p.s.assign(n, 0.0f);
    for (size_t i = 1; i < n; ++i) {
        const float d = std::hypot(ref[i].x - ref[i - 1].x, ref[i].z - ref[i - 1].z);
        p.s[i]        = p.s[i - 1] + (d < coachline::kMaxGapM ? d : 0.0f);
    }
    const float close = std::hypot(ref[0].x - ref[n - 1].x, ref[0].z - ref[n - 1].z);
    p.lap_m           = p.s.back() + (close < coachline::kMaxGapM ? close : 0.0f);
    p.v.assign(n, -1.0f);
    if (drive.size() == n * 3) {
        for (size_t i = 0; i < n; ++i) p.v[i] = drive[i * 3];
    } else {
        for (size_t i = 0; i < n; ++i) {
            size_t lo = i, hi = i;
            while (lo > 0 && p.s[i] - p.s[lo] < coachline::kSpeedWindowM) --lo;
            while (hi + 1 < n && p.s[hi] - p.s[i] < coachline::kSpeedWindowM) ++hi;
            const float dt = ref[hi].t - ref[lo].t;
            if (dt > 1e-3f) p.v[i] = (p.s[hi] - p.s[lo]) / dt;
        }
        // A point the times can't speak for takes its neighbour's speed.
        for (size_t i = 1; i < n; ++i)
            if (p.v[i] < 0) p.v[i] = p.v[i - 1];
        for (size_t i = n - 1; i-- > 0;)
            if (p.v[i] < 0) p.v[i] = p.v[i + 1];
        if (p.v[0] < 0) return Profile{};
    }
    p.tone = coachline::Tone(ref, drive);
    if (p.tone.size() != n) return Profile{};
    if (heights.size() == n) p.lips = FindLips(p.s, heights, p.v);
    return p;
}

// --- one reading, from one telemetry sample --------------------------------------------------

struct Reading {
    bool  valid       = false;
    float rel         = 0;    // the rider's speed over Coach's here, less one
    float vref        = 0;    // Coach's speed here, m/s
    float overshoot_m = 0;    // how far past the next braking zone's slowest point they'd run
    float brake_m     = -1;   // metres to the next braking zone (0: in one); -1 none in view
    float lip_m       = -1;   // metres to the next jump lip; -1 none in view
    float score       = 0;    // signed, 1 = a too-fast hint's threshold, -1 a too-slow one's
};

/// The rider at sheet point `idx`, `frac` of the way to the next (coachline::AnchorPoint),
/// riding at `speed` m/s.
inline Reading Read(const Profile& p, int idx, float frac, float speed) {
    Reading r;
    if (!p.ok() || idx < 0 || size_t(idx) >= p.s.size() || !std::isfinite(speed) || !std::isfinite(frac)) return r;
    const size_t n  = p.s.size();
    const size_t i  = size_t(idx);
    const size_t i1 = (i + 1) % n;
    frac            = (std::max)(0.0f, (std::min)(1.0f, frac));
    r.vref          = p.v[i] + (p.v[i1] - p.v[i]) * frac;
    const float tone_here = p.tone[i] + (p.tone[i1] - p.tone[i]) * frac;
    if (speed < kMinSpeed || r.vref < kMinRefSpeed) return r;
    r.valid = true;
    r.rel   = speed / r.vref - 1.0f;

    // Walk ahead along Coach's line: the next braking zone (and its slowest point) and lip.
    const float look = (std::max)(kLookMinM, (std::min)(kLookMaxM, speed * kLookS));
    float       d    = (1.0f - frac) * p.seg(i);
    bool        in_zone = tone_here >= coachline::kZoneTone;
    if (in_zone) r.brake_m = 0;
    float zone_v = in_zone ? r.vref : -1.0f;  // Coach's speed where the zone starts
    float min_v = 1e9f, min_d = -1;
    bool  zone_done = false;
    size_t lip_k = 0;
    while (lip_k < p.lips.size() && p.lips[lip_k] <= i) ++lip_k;  // the first lip after the rider
    for (size_t k = 1; k < n && d <= look + kMaxAdvanceM; ++k) {
        const size_t j = (i + k) % n;
        if (r.lip_m < 0 && d <= look && !p.lips.empty()) {
            const size_t want = p.lips[lip_k % p.lips.size()];
            if (j == want) r.lip_m = d;
        }
        if (!zone_done) {
            const bool z = p.tone[j] >= coachline::kZoneTone;
            if (z && zone_v < 0 && d <= look) {
                zone_v    = p.v[j];
                r.brake_m = d;
            }
            if (zone_v >= 0) {
                if (p.v[j] < min_v) min_v = p.v[j], min_d = d;
                if (!z) zone_done = true;
            }
        }
        d += p.seg(j);
    }
    if (zone_v >= 0 && min_d >= 0) {
        const float zone_len = min_d - r.brake_m;
        if (zone_len > 2.0f && zone_v > min_v + 1.0f) {
            const float a      = (zone_v * zone_v - min_v * min_v) / (2.0f * zone_len);
            // In the zone already, their own speed now; else Coach's at its start, as far up or
            // down on him as they are now.
            const float arrive = in_zone ? speed : zone_v * (1.0f + r.rel);
            r.overshoot_m      = (std::max)(0.0f, (arrive * arrive - min_v * min_v) / (2.0f * a) - zone_len);
        }
    }
    const float fast = (std::max)(r.rel / kEnterRel, r.overshoot_m / kEnterOvershootM);
    const bool  quiet = tone_here >= kQuietTone || (r.brake_m >= 0 && r.brake_m < kSlowQuietM);
    if (fast > 0) r.score = fast;
    else if (!quiet) r.score = r.rel / kEnterRel;  // negative
    return r;
}

// --- the hint, filtered so it never flickers ----------------------------------------------

enum Kind { NONE = 0, FAST = 1, SLOW = 2 };

struct Hint {
    Kind  kind        = NONE;  // what is drawn (it fades out as itself)
    float level       = 0;     // how strongly, 0..1
    float advance_m   = 0;     // too fast: how much sooner the braking colours come
    float brake_m     = -1;    // as the last reading said
    float lip_m       = -1;
    bool  more_speed  = false; // too slow with a jump lip ahead: the gate and "MORE SPEED"
};

class Filter {
public:
    void reset() { *this = Filter{}; }
    /// One telemetry sample `dt` seconds after the last.
    Hint step(const Reading& r, float dt) {
        dt = (std::max)(0.0f, (std::min)(0.1f, std::isfinite(dt) ? dt : 0.0f));
        const float raw = r.valid ? r.score : 0.0f;
        score_ += dt / (kTauS + dt) * (raw - score_);
        adv_ += dt / (kTauS + dt) * ((r.valid ? r.overshoot_m : 0.0f) - adv_);
        since_ += dt;
        switch (want_) {
            case NONE:
                in_fast_ = score_ >= 1.0f ? in_fast_ + dt : 0.0f;
                in_slow_ = score_ <= -1.0f ? in_slow_ + dt : 0.0f;
                if (in_fast_ >= kHoldInS) go(FAST);
                else if (in_slow_ >= kHoldInS) go(SLOW);
                break;
            case FAST:
            case SLOW: {
                const float mag = want_ == FAST ? score_ : -score_;
                out_ = mag < kExitFrac ? out_ + dt : 0.0f;
                if (out_ >= kHoldOutS && since_ >= kMinShowS) go(NONE);
                break;
            }
        }
        // What is drawn follows what is wanted, but only by fading: out, switch, back in.
        float target = 0;
        if (want_ != NONE && (shown_ == want_ || level_ <= 0)) {
            shown_         = want_;
            const float m  = want_ == FAST ? score_ : -score_;
            target         = (std::max)(0.35f, (std::min)(1.0f, 0.35f + 0.65f * (m - kExitFrac) / (2.5f - kExitFrac)));
        }
        const float stepv = kFadeRate * dt;
        level_            = target > level_ ? (std::min)(target, level_ + stepv) : (std::max)(target, level_ - stepv);
        if (level_ <= 0) {
            level_ = 0;
            if (want_ == NONE) shown_ = NONE;
        }
        Hint h;
        h.kind       = level_ > 0 ? shown_ : NONE;
        h.level      = level_;
        h.brake_m    = r.brake_m;
        h.lip_m      = r.lip_m;
        h.advance_m  = h.kind == FAST ? (std::max)(kMinAdvanceM, (std::min)(kMaxAdvanceM, adv_)) : 0.0f;
        h.more_speed = h.kind == SLOW && level_ >= 0.2f && r.lip_m >= kLipShowMinM && r.lip_m <= kLipShowMaxM;
        return h;
    }
    Kind  wanted() const { return want_; }
    float score() const { return score_; }

private:
    void go(Kind k) {
        want_  = k;
        since_ = 0;
        in_fast_ = in_slow_ = out_ = 0;
    }
    Kind  want_ = NONE, shown_ = NONE;
    float score_ = 0, adv_ = 0, level_ = 0;
    float since_ = 0, in_fast_ = 0, in_slow_ = 0, out_ = 0;
};

// --- what it does to the line's colours -------------------------------------------------------

constexpr float kBrightGreen[3] = {0.30f, 1.00f, 0.35f};
constexpr float kSlowAlpha      = 0.88f;

/// The line's colours (coachline::LineColours, `base`, RGBA per point) with the hint applied to
/// the stretch ahead of the rider at `idx`/`frac`. Too fast: each point takes the more urgent of
/// its own tone and the tone `advance_m` further on, so yellow and red come that much sooner.
/// Too slow: the gas a brighter, more solid green. Points behind the rider keep their colour.
inline std::vector<float> Recolour(const Profile& p, const std::vector<float>& base, int idx, float frac,
                                   const Hint& h) {
    std::vector<float> out = base;
    const size_t       n   = p.s.size();
    if (!p.ok() || base.size() != n * 4 || idx < 0 || size_t(idx) >= n || h.kind == NONE || h.level <= 0) return out;
    const size_t i     = size_t(idx);
    const float  reach = coachline::kAhead + 5.0f;
    const float  adv   = h.kind == FAST ? h.advance_m * h.level : 0.0f;
    // The points ahead, with their distance from the rider, out to `reach` plus the advance.
    std::vector<std::pair<float, size_t>> ahead;
    float d = -frac * p.seg(i);
    for (size_t k = 0; k < n && d <= reach + adv; ++k) {
        const size_t j = (i + k) % n;
        ahead.push_back({d, j});
        d += p.seg(j);
    }
    size_t q = 0;
    for (const auto& a : ahead) {
        if (a.first < 0 || a.first > reach) continue;
        const size_t j = a.second;
        float*       c = &out[j * 4];
        if (h.kind == FAST) {
            // The tone at a.first + adv, interpolated between the two points either side.
            const float want = a.first + adv;
            while (q + 1 < ahead.size() && ahead[q + 1].first < want) ++q;
            float t = p.tone[ahead[q].second];
            if (q + 1 < ahead.size()) {
                const float span = ahead[q + 1].first - ahead[q].first;
                const float f    = span > 1e-4f ? (std::max)(0.0f, (std::min)(1.0f, (want - ahead[q].first) / span)) : 0.0f;
                t += (p.tone[ahead[q + 1].second] - t) * f;
            }
            if (t > p.tone[j]) coachline::ToneColour(t, c[0], c[1], c[2]);
        } else {
            const float k = h.level * (std::max)(0.0f, 1.0f - p.tone[j]);
            for (int ch = 0; ch < 3; ++ch) c[ch] += (kBrightGreen[ch] - c[ch]) * k * 0.85f;
            c[3] += (kSlowAlpha - c[3]) * k;
        }
    }
    return out;
}

// --- the chevrons and the gate, over the ribbon --------------------------------------------

constexpr float kChevGapM    = 5.0f;   // one chevron every 5 m of Coach's line, fixed to the ground
constexpr float kChevFromM   = 6.0f;   // not under the bike
constexpr float kChevToM     = 45.0f;
constexpr float kChevLenM    = 1.6f;   // tip to tail
constexpr float kChevHalfW   = 0.6f;   // wider than the ribbon, so the arms read as edge marks too
constexpr float kChevThickM  = 0.5f;   // thick: lying on the ground, it is seen edge-on
constexpr float kMarkLift    = 0.015f; // over the ribbon
constexpr float kGateHalfW   = 1.0f;
constexpr float kGateDepthM  = 0.45f;
constexpr float kFastColour[3] = {1.00f, 0.25f, 0.80f};  // magenta: on no base colour
constexpr float kSlowColour[3] = {0.20f, 0.85f, 1.00f};  // cyan: nor this
constexpr float kMarkAlpha   = 0.92f;

/// The marks for the hint over the ribbon `strip` (coachline::Ribbon::verts(): rows of left then
/// right, `s` metres from the rider), as triangles (three Verts each). `phase_m` is how far the
/// rider is along Coach's line (Profile::s at them), so the chevrons stay put on the ground as the
/// rider rides over them instead of travelling with the bike.
inline std::vector<coachline::Vert> Marks(const std::vector<coachline::Vert>& strip, const Hint& h, float phase_m) {
    std::vector<coachline::Vert> out;
    const size_t rows = strip.size() / 2;
    if (h.kind == NONE || h.level <= 0 || rows < 2 || !std::isfinite(phase_m)) return out;
    // The rider's colours (coachhud::LineLook), kFastColour and kSlowColour until they pick others.
    const float* col = h.kind == FAST ? coachline::Look().fast : coachline::Look().slow;
    // The ribbon at `s` metres: its centre, the unit vector across it to the left, the unit
    // vector along it, and the ground height across it (left and right edge).
    struct Frame {
        float cx, cz, nx, nz, tx, tz, yl, yr, s, hw;
    };
    auto frame_at = [&](float s, Frame& f) {
        if (s < strip[0].s || s > strip[(rows - 1) * 2].s) return false;
        size_t r = 0;
        while (r + 2 < rows && strip[(r + 1) * 2].s < s) ++r;
        const coachline::Vert &L0 = strip[r * 2], &R0 = strip[r * 2 + 1], &L1 = strip[(r + 1) * 2],
                              &R1 = strip[(r + 1) * 2 + 1];
        const float span = L1.s - L0.s;
        const float u    = span > 1e-4f ? (std::max)(0.0f, (std::min)(1.0f, (s - L0.s) / span)) : 0.0f;
        const float lx = L0.x + (L1.x - L0.x) * u, lz = L0.z + (L1.z - L0.z) * u, ly = L0.y + (L1.y - L0.y) * u;
        const float rx = R0.x + (R1.x - R0.x) * u, rz = R0.z + (R1.z - R0.z) * u, ry = R0.y + (R1.y - R0.y) * u;
        f.cx = (lx + rx) * 0.5f, f.cz = (lz + rz) * 0.5f;
        float nx = lx - rx, nz = lz - rz;
        const float nl = std::hypot(nx, nz);
        if (nl < 1e-4f) return false;
        f.nx = nx / nl, f.nz = nz / nl;
        f.hw = 0.5f * nl;
        // Along the line: the left perpendicular turned back, (x, z) -> (z, -x) of the left vector.
        f.tx = f.nz, f.tz = -f.nx;
        f.yl = ly, f.yr = ry, f.s = s;
        return true;
    };
    // A point `along` metres down the line and `across` to the left of the centre, on the ground.
    auto put = [&](const Frame& f, float along, float across, float a) {
        coachline::Vert v;
        v.x = f.cx + f.tx * along + f.nx * across;
        v.z = f.cz + f.tz * along + f.nz * across;
        const float w = (across / f.hw + 1.0f) * 0.5f;  // 0 right edge, 1 left
        v.y = f.yr + (f.yl - f.yr) * w + kMarkLift;
        v.s = f.s;
        v.rgba[0] = col[0], v.rgba[1] = col[1], v.rgba[2] = col[2], v.rgba[3] = a;
        out.push_back(v);
    };
    auto quad = [&](const Frame& f, float a0, float c0, float a1, float c1, float a2, float c2, float a3, float c3,
                    float alpha) {
        put(f, a0, c0, alpha), put(f, a1, c1, alpha), put(f, a2, c2, alpha);
        put(f, a0, c0, alpha), put(f, a2, c2, alpha), put(f, a3, c3, alpha);
    };
    float to = kChevToM;
    if (h.brake_m >= 0) to = (std::min)(to, h.kind == FAST ? h.brake_m + 4.0f : h.brake_m - 5.0f);
    if (h.kind == SLOW && h.more_speed) to = (std::min)(to, h.lip_m - 2.0f);
    const float dir = h.kind == FAST ? -1.0f : 1.0f;  // which way the tip points along the line
    float       s0  = kChevGapM - std::fmod(phase_m, kChevGapM);
    if (s0 >= kChevGapM) s0 -= kChevGapM;
    for (float s = s0; s <= to; s += kChevGapM) {
        if (s < kChevFromM) continue;
        Frame f;
        if (!frame_at(s, f)) continue;
        const float alpha = kMarkAlpha * h.level * coachline::Fade(s);
        if (alpha <= 0.01f) continue;
        const float tip = dir * kChevLenM * 0.5f, tail = -tip;
        const float th  = dir * kChevThickM;
        // Two arms, each a quad from the tip back to one side.
        for (float side : {1.0f, -1.0f})
            quad(f, tip, 0.0f, tip - th, 0.0f, tail - th, side * kChevHalfW, tail, side * kChevHalfW, alpha);
    }
    if (h.more_speed) {
        Frame f;
        const float alpha = kMarkAlpha * h.level;
        // The gate: two bars across the line, at the lip and just before it.
        for (float back : {0.0f, 1.0f})
            if (frame_at(h.lip_m - back, f))
                quad(f, -kGateDepthM * 0.5f, -kGateHalfW, kGateDepthM * 0.5f, -kGateHalfW, kGateDepthM * 0.5f, kGateHalfW,
                     -kGateDepthM * 0.5f, kGateHalfW, alpha * (back > 0 ? 0.6f : 1.0f));
    }
    return out;
}

/// The rider's distance along Coach's line at sheet point `idx`, `frac` of the way on: the
/// chevrons' phase.
inline float PhaseAt(const Profile& p, int idx, float frac) {
    if (!p.ok() || idx < 0 || size_t(idx) >= p.s.size()) return 0.0f;
    return p.s[size_t(idx)] + (std::max)(0.0f, (std::min)(1.0f, frac)) * p.seg(size_t(idx));
}

}  // namespace coachpace
