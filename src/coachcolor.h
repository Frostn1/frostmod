// coachcolor.h - the colours of Coach's ground line: what the rider is doing along the lap,
// as a colour that blends and never cuts.
//
//   GAS = green    COAST (off the throttle, off the brakes) = white
//   LIGHT BRAKE = yellow    HEAVY BRAKE = red        (no blue anywhere)
//
// Pure maths (no Win32, no GL), so tests/coachcolor_test.cpp runs anywhere. The caller builds a
// Line once per lap from Coach's reference lap (Build) or, when the sheet has no channels, from
// the cue sheet's braking zones (BuildFromZones), then asks it for a colour at each ribbon vertex
// (Line::at) and where to put the vertices (Subdivide) so GL's per-vertex interpolation reads as
// one smooth wash.
//
// How: each sample's throttle and brake intensity are two continuous weights. Brake intensity is
// the larger of the brake input and the deceleration (derived from speed over distance) scaled
// to a hard stop. Both are smoothed along the line with a Gaussian (~4 m), then folded into one
// position on a gradient: 0 green, 1 white, 2 yellow, 3 red. Smoothing the weights, not the
// colours, is what keeps every transition monotone: gas to coast to brake walks green, white,
// yellow, red and back, and never detours through a colour that is not between the two.
//
// No dependency on coachline.h: BuildFromZones takes anything with from_m / to_m, so
// coachline::BrakeZones() output goes straight in and coachline.h can include this header.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace coachcolor {

/// One reference-lap sample. Distance in metres along the lap, speed in m/s, throttle and brake
/// 0..1. Samples must be by increasing `s`.
struct Sample {
    float s = 0, speed = 0, throttle = 0, brake = 0;
};

struct Rgba {
    float r = 0, g = 0, b = 0, a = 0;
};

struct Params {
    float sigma_m     = 4.0f;   // Gaussian smoothing along the line, 3 to 5 m reads well
    float alpha       = 0.7f;   // the dirt shows through a little
    float heavy_decel = 8.0f;   // m/s^2 that counts as a full, red brake from the speed trace
    float step_m      = 1.0f;   // spacing of the smoothed profile
    float max_delta   = 0.12f;  // Subdivide: biggest colour change between two vertices
    float min_step_m  = 0.4f;   // Subdivide: never closer than this
    float zone_brake  = 0.8f;   // fallback: brake intensity inside a cue-sheet braking zone
};

// The gradient stops: slightly saturated so they read on brown dirt. White stays white.
constexpr Rgba kGreen  = {0.12f, 0.82f, 0.18f, 1};
constexpr Rgba kWhite  = {0.98f, 0.98f, 0.98f, 1};
constexpr Rgba kYellow = {1.00f, 0.84f, 0.06f, 1};
constexpr Rgba kRed    = {0.92f, 0.08f, 0.06f, 1};

inline float Clamp01(float v) { return std::isfinite(v) ? (std::min)(1.0f, (std::max)(0.0f, v)) : 0.0f; }

/// The gradient: position 0 green, 1 white, 2 yellow, 3 red, linear between, clamped outside.
inline Rgba Gradient(float t, float alpha = 1.0f) {
    if (!std::isfinite(t)) t = 1.0f;
    t                         = (std::min)(3.0f, (std::max)(0.0f, t));
    const Rgba* stops[4]      = {&kGreen, &kWhite, &kYellow, &kRed};
    const int   i             = (std::min)(2, int(t));
    const float f             = t - float(i);
    const Rgba &a = *stops[i], &b = *stops[i + 1];
    return {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, alpha};
}

/// What the rider is doing at one point: gas 0..1 (zero while braking), brake intensity 0..1
/// (0.5 is a light brake, 1 a heavy one). Folded into a gradient position: 1 + 2*brake - gas.
inline float Position(float gas, float brake) { return 1.0f + 2.0f * Clamp01(brake) - Clamp01(gas); }

/// The smoothed line: one gradient position per profile step along the lap.
class Line {
public:
    /// Gradient position (0..3) at distance s along the lap. Outside the profile it holds the
    /// end values, or wraps when the lap length is known.
    float pos_at(float s) const {
        if (t_.empty()) return 1.0f;
        float u = (s - s0_) / step_;
        if (lap_ > 0) {
            const float n = float(t_.size());
            u             = std::fmod(u, n);
            if (u < 0) u += n;
            const size_t i = size_t(u) % t_.size(), j = (i + 1) % t_.size();
            const float  f = u - std::floor(u);
            return t_[i] + (t_[j] - t_[i]) * f;
        }
        if (u <= 0) return t_.front();
        if (u >= float(t_.size() - 1)) return t_.back();
        const size_t i = size_t(u);
        const float  f = u - float(i);
        return t_[i] + (t_[i + 1] - t_[i]) * f;
    }
    /// The ribbon colour (RGBA, alpha from Params) at distance s along the lap.
    Rgba at(float s) const { return Gradient(pos_at(s), alpha_); }

    bool                      empty() const { return t_.empty(); }
    const std::vector<float>& positions() const { return t_; }
    float                     start() const { return s0_; }
    float                     step() const { return step_; }

    std::vector<float> t_;  // gradient position per step
    float              s0_ = 0, step_ = 1, lap_ = 0, alpha_ = 0.7f;
};

namespace detail {

/// Gaussian smoothing of a uniformly spaced signal; wraps when `wrap`, else holds the ends.
inline std::vector<float> Smooth(const std::vector<float>& x, float step, float sigma, bool wrap) {
    const int n = int(x.size());
    if (n == 0 || sigma <= 0 || step <= 0) return x;
    const int          rad = (std::max)(1, int(std::ceil(3.0f * sigma / step)));
    std::vector<float> k(size_t(2 * rad + 1));
    float              sum = 0;
    for (int d = -rad; d <= rad; ++d) {
        const float z = float(d) * step / sigma;
        k[size_t(d + rad)] = std::exp(-0.5f * z * z);
        sum += k[size_t(d + rad)];
    }
    std::vector<float> out(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        float acc = 0;
        for (int d = -rad; d <= rad; ++d) {
            int j = i + d;
            if (wrap) {
                j %= n;
                if (j < 0) j += n;
            } else {
                j = (std::min)(n - 1, (std::max)(0, j));
            }
            acc += x[size_t(j)] * k[size_t(d + rad)];
        }
        out[size_t(i)] = acc / sum;
    }
    return out;
}

/// Linear interpolation of per-sample values `v` (parallel to smp) at s; `hint` only moves forward.
inline float Lerp(const std::vector<Sample>& smp, const std::vector<float>& v, float s, size_t& hint) {
    while (hint + 1 < smp.size() && smp[hint + 1].s <= s) ++hint;
    if (s <= smp.front().s) return v.front();
    if (hint + 1 >= smp.size()) return v.back();
    const float span = smp[hint + 1].s - smp[hint].s;
    const float f    = span > 0 ? (s - smp[hint].s) / span : 0.0f;
    return v[hint] + (v[hint + 1] - v[hint]) * f;
}

}  // namespace detail

/// Builds the line from the reference lap's channels. `lap_m` > 0 wraps the profile around the
/// lap (the ribbon crosses the line); 0 treats it as an open run. Empty for fewer than 2 samples.
inline Line Build(const std::vector<Sample>& smp, float lap_m, const Params& p = {}) {
    Line out;
    out.alpha_ = p.alpha;
    out.lap_   = lap_m > 0 ? lap_m : 0;
    if (smp.size() < 2 || !(p.step_m > 0)) return out;
    const float span = out.lap_ > 0 ? out.lap_ : (smp.back().s - smp.front().s);
    if (!(span > 0)) return out;
    const int n = (std::max)(2, int(std::ceil(span / p.step_m)));
    out.s0_     = out.lap_ > 0 ? 0.0f : smp.front().s;
    out.step_   = span / float(out.lap_ > 0 ? n : n - 1);

    // Per-sample channels: throttle, brake input, and deceleration v*dv/ds from the speed trace
    // (central differences; NaN speed or a zero-length gap derives none).
    const size_t       m = smp.size();
    std::vector<float> thr(m), brk_in(m), decel(m, 0.0f);
    for (size_t i = 0; i < m; ++i) {
        thr[i]       = Clamp01(smp[i].throttle);
        brk_in[i]    = Clamp01(smp[i].brake);
        const size_t a = i == 0 ? 0 : i - 1, b = (std::min)(i + 1, m - 1);
        const float  ds = smp[b].s - smp[a].s;
        if (ds > 1e-3f && std::isfinite(smp[b].speed) && std::isfinite(smp[a].speed) && std::isfinite(smp[i].speed))
            decel[i] = -(smp[b].speed - smp[a].speed) / ds * (std::max)(0.0f, smp[i].speed);
    }

    // Resample onto the profile grid and fold into the two weights: brake intensity, and gas
    // (which braking turns off), then smooth both.
    std::vector<float> gas(static_cast<size_t>(n), 0.0f), brk(static_cast<size_t>(n), 0.0f);
    size_t             h0 = 0, h1 = 0, h2 = 0;
    for (int i = 0; i < n; ++i) {
        const float s  = out.s0_ + float(i) * out.step_;
        const float t  = detail::Lerp(smp, thr, s, h0);
        const float bi = (std::max)(detail::Lerp(smp, brk_in, s, h1), Clamp01(detail::Lerp(smp, decel, s, h2) / p.heavy_decel));
        brk[size_t(i)] = bi;
        gas[size_t(i)] = t * (1.0f - bi);
    }
    const bool         wrap = out.lap_ > 0;
    const std::vector<float> g = detail::Smooth(gas, out.step_, p.sigma_m, wrap);
    const std::vector<float> b = detail::Smooth(brk, out.step_, p.sigma_m, wrap);
    out.t_.resize(size_t(n));
    for (int i = 0; i < n; ++i) out.t_[size_t(i)] = Position(g[size_t(i)], b[size_t(i)]);
    return out;
}

/// The fallback for a sheet with no channels: Coach's braking zones (coachline::BrakeZones, or
/// anything with from_m / to_m, which may run past the lap length and wrap). Outside a zone the
/// rider is taken to be on the gas, inside it braking at p.zone_brake; the same smoothing blends
/// the two, so the zones fade in and out rather than cutting.
template <class ZoneT>
inline Line BuildFromZones(const std::vector<ZoneT>& zones, float lap_m, const Params& p = {}) {
    if (!(lap_m > 0) || !(p.step_m > 0)) {
        Line e;
        e.alpha_ = p.alpha;
        return e;
    }
    const int           n    = (std::max)(2, int(std::ceil(lap_m / p.step_m)));
    const float         step = lap_m / float(n);
    std::vector<Sample> smp(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float m  = float(i) * step;
        bool        in = false;
        for (const ZoneT& z : zones)
            if ((m >= z.from_m && m < z.to_m) || (m + lap_m >= z.from_m && m + lap_m < z.to_m)) in = true;
        smp[size_t(i)].s        = m;
        smp[size_t(i)].throttle = in ? 0.0f : 1.0f;
        smp[size_t(i)].brake    = in ? p.zone_brake : 0.0f;
    }
    return Build(smp, lap_m, p);
}

inline float MaxChannelDelta(const Rgba& a, const Rgba& b) {
    return (std::max)((std::max)(std::fabs(a.r - b.r), std::fabs(a.g - b.g)),
                      (std::max)(std::fabs(a.b - b.b), std::fabs(a.a - b.a)));
}

/// Where to put ribbon vertices between s_from and s_to: `base_step` apart where the colour is
/// steady, closer where it changes fast, so no two neighbours differ by more than p.max_delta
/// (down to p.min_step_m). Includes both ends; strictly increasing.
inline std::vector<float> Subdivide(const Line& line, float s_from, float s_to, float base_step,
                                    const Params& p = {}) {
    std::vector<float> out;
    if (!(base_step > 0) || !(s_to >= s_from)) return out;
    const float floor_step = (std::min)(p.min_step_m, base_step);
    float       s          = s_from;
    out.push_back(s);
    while (s < s_to) {
        float      step = base_step;
        const Rgba here = line.at(s);
        while (step > floor_step && MaxChannelDelta(here, line.at((std::min)(s + step, s_to))) > p.max_delta)
            step *= 0.5f;
        step = (std::max)(step, floor_step);
        s    = (std::min)(s + step, s_to);
        out.push_back(s);
    }
    return out;
}

}  // namespace coachcolor
