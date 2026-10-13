// coachcutmarks.h - the track's cut markers (coachcuts.h) standing on the track itself: a pair of
// thin posts either side of the centreline at each marker, a faint bar on the ground between
// them, and the marker's number over the middle. Built as coachmark quads in the telemetry world
// and drawn by mxbcoach.cpp through the same camera and state block as Coach's line.
//
// Subtle by design: white at low alpha, fading out with distance so only the next few stand up.
// The marker the rider is on is amber; one they have been cut at is red. Pure maths, no GL:
// pinned by tests/coachcuts_test.cpp.
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "coachcuts.h"
#include "coachmark.h"

namespace coachcutmarks {

/// A marker on the ground: its number, the ground under the centreline there (world x/y/z)
/// and the line's heading, degrees with 0 north (the SDK's m_fAngle convention).
struct Pose {
    int   id = 0;
    float x = 0, y = 0, z = 0, heading = 0;
};

constexpr float kHalfGate = 4.0f;   // posts this far either side of the centreline, m
constexpr float kPostW    = 0.12f;  // a post's width, m
constexpr float kPostH    = 1.6f;   // and height
constexpr float kBarD     = 0.08f;  // the ground bar's depth along the line
constexpr float kBarLift  = 0.05f;  // and how far over the ground it lies
constexpr float kTextH    = 0.7f;   // the number's cap height, m: readable from ~40 m
constexpr float kTextLift = 2.0f;   // the number's bottom over the ground
constexpr float kFull     = 45.0f;  // solid within this distance of the rider, m
constexpr float kFar      = 80.0f;  // gone past this
constexpr float kNear     = 3.0f;   // and faded out right under the bike, so it never blocks the view

/// How solid a marker `d` metres from the rider is drawn: 0 past kFar, 1 inside kFull, and
/// fading again inside kNear.
inline float Fade(float d) {
    if (!std::isfinite(d) || d >= kFar) return 0.0f;
    float k = d <= kFull ? 1.0f : (kFar - d) / (kFar - kFull);
    if (d < kNear) k *= (std::max)(0.0f, d / kNear);
    return k;
}

/// The colour of a marker: amber when the rider is on it, red where they were cut, else white.
inline void Colour(bool here, bool cut, float alpha, float out[4]) {
    if (here) out[0] = 1.0f, out[1] = 0.84f, out[2] = 0.36f, out[3] = 0.85f;
    else if (cut) out[0] = 1.0f, out[1] = 0.29f, out[2] = 0.29f, out[3] = 0.8f;
    else out[0] = out[1] = out[2] = 1.0f, out[3] = 0.35f;
    out[3] *= alpha;
}

/// One upright quad facing along the line: `w` wide across it, centred `across` metres right of
/// (x, z), from `y0` to `y1`.
inline void Upright(std::vector<coachmark::Quad>& out, const Pose& p, float rx, float rz, float across, float w, float y0,
                    float y1, const float rgba[4], float s) {
    coachmark::Quad q;
    const float cx = p.x + rx * across, cz = p.z + rz * across, h = 0.5f * w;
    const float xs[4] = {-h, +h, +h, -h}, ys[4] = {y0, y0, y1, y1};
    for (int c = 0; c < 4; ++c) {
        q.p[c][0] = cx + rx * xs[c];
        q.p[c][1] = ys[c];
        q.p[c][2] = cz + rz * xs[c];
        for (int k = 0; k < 4; ++k) q.rgba[k] = rgba[k];
    }
    q.s = s;
    out.push_back(q);
}

/// The gates near the rider at (rx, rz): `now_id` is the marker they are on, `tally` the cuts so
/// far (null: none). Appends to `out` and returns how many markers were drawn.
inline int Build(std::vector<coachmark::Quad>& out, const std::vector<Pose>& poses, float rider_x, float rider_z,
                 int now_id, const coachcuts::Tally* tally) {
    int drawn = 0;
    for (const Pose& p : poses) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(p.heading)) continue;
        const float d = std::hypot(p.x - rider_x, p.z - rider_z);
        const float k = Fade(d);
        if (!(k > 0.01f)) continue;
        float rgba[4];
        Colour(p.id == now_id, tally && tally->count(p.id) > 0, k, rgba);
        // Facing the rider riding along the line: right is forward turned a quarter clockwise
        // seen from above (forward = (sin h, cos h) in x/z).
        const float h = p.heading * 0.017453292f;
        const float fx = std::sin(h), fz = std::cos(h);
        const float rx = fz, rz = -fx;
        for (float side : {-kHalfGate, kHalfGate}) Upright(out, p, rx, rz, side, kPostW, p.y, p.y + kPostH, rgba, d);
        // The bar on the ground between the posts, fainter than they are.
        float bar[4] = {rgba[0], rgba[1], rgba[2], rgba[3] * 0.6f};
        coachmark::Quad q;
        const float sx[4] = {+1, +1, -1, -1}, sf[4] = {-1, +1, +1, -1};
        for (int c = 0; c < 4; ++c) {
            q.p[c][0] = p.x + rx * sx[c] * kHalfGate + fx * sf[c] * kBarD * 0.5f;
            q.p[c][1] = p.y + kBarLift;
            q.p[c][2] = p.z + rz * sx[c] * kHalfGate + fz * sf[c] * kBarD * 0.5f;
            for (int j = 0; j < 4; ++j) q.rgba[j] = bar[j];
        }
        q.s = d;
        out.push_back(q);
        const float base[3]  = {p.x, p.y + kTextLift, p.z};
        const float right[3] = {rx, 0.0f, rz};
        coachmark::Text(out, std::to_string(p.id), base, right, kTextH, rgba, d, coachhud::TEXT_BLOCK);
        ++drawn;
    }
    return drawn;
}

}  // namespace coachcutmarks
