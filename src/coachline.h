// coachline.h - the Forza/GT-style line to take, painted on the track ground in the 3D view.
// Pure maths, no Win32 and no GL, so tests/coachline_test.cpp can run it. The GL capture and the
// draw live in mxbcoach.cpp. Design: DevHub notes/mxb-coach-ground-line-2026-10-01.md.
//
// READ-ONLY: it reads the GL matrices the game already loads and draws one extra ribbon. It
// writes nothing to the game.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "coachhud.h"

namespace coachline {

/// Column-major, as OpenGL hands it over (m[col*4 + row]).
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

inline bool Finite(const float* m, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(m[i])) return false;
    return true;
}

/// Column-major a * b.
inline Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

/// A perspective projection as glFrustum / gluPerspective build it: m[11] = -1, m[15] = 0, a
/// vertical field of view of 20..140 degrees and a plausible aspect. Anything else (the game's
/// ortho HUD passes, a shadow map's projection) is refused so it cannot pass for the camera.
inline bool ValidProjection(const Mat4& p) {
    const float* m = p.m;
    if (!Finite(m, 16)) return false;
    if (std::fabs(m[11] + 1.0f) > 1e-3f || std::fabs(m[15]) > 1e-3f) return false;
    if (!(m[0] > 0) || !(m[5] > 0)) return false;
    const float fovy = 2.0f * std::atan(1.0f / m[5]) * 57.2957795f;
    if (fovy < 20.0f || fovy > 140.0f) return false;
    const float aspect = m[5] / m[0];
    return aspect > 0.5f && aspect < 4.0f;
}

/// A camera view: rotation and translation only (orthonormal 3x3, bottom row 0 0 0 1).
inline bool ValidView(const Mat4& v) {
    const float* m = v.m;
    if (!Finite(m, 16)) return false;
    if (std::fabs(m[3]) > 1e-4f || std::fabs(m[7]) > 1e-4f || std::fabs(m[11]) > 1e-4f ||
        std::fabs(m[15] - 1.0f) > 1e-4f)
        return false;
    for (int c = 0; c < 3; ++c) {
        const float len = std::sqrt(m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] + m[c * 4 + 2] * m[c * 4 + 2]);
        if (std::fabs(len - 1.0f) > 0.02f) return false;
    }
    return true;
}

struct Screen {
    float x = 0, y = 0;    // 0..1, origin top left
    float depth = 0;       // clip w: metres in front of the camera
    bool  ok    = false;   // in front of the camera
};

/// World to screen fractions through projection `p` and view `v`. After MXBMRP3's worldToScreen,
/// but through the game's own matrices instead of a camera model of our own.
inline Screen Project(const Mat4& p, const Mat4& v, float x, float y, float z) {
    Screen       s;
    const float* a  = v.m;
    const float  ex = a[0] * x + a[4] * y + a[8] * z + a[12];
    const float  ey = a[1] * x + a[5] * y + a[9] * z + a[13];
    const float  ez = a[2] * x + a[6] * y + a[10] * z + a[14];
    const float* b  = p.m;
    const float  cx = b[0] * ex + b[4] * ey + b[8] * ez + b[12];
    const float  cy = b[1] * ex + b[5] * ey + b[9] * ez + b[13];
    const float  cw = b[3] * ex + b[7] * ey + b[11] * ez + b[15];
    if (!(cw > 1e-4f)) return s;
    s.x     = 0.5f + 0.5f * cx / cw;
    s.y     = 0.5f - 0.5f * cy / cw;
    s.depth = cw;
    s.ok    = true;
    return s;
}

/// The rider's own bike should land on screen, in front of the camera, when the camera is its
/// chase or helmet view; a capture that puts it behind the camera or far off screen has the wrong
/// convention or the wrong pass. A loose test: a free camera legitimately looks away.
inline bool PlausibleCapture(const Mat4& p, const Mat4& v, float rx, float ry, float rz) {
    const Screen s = Project(p, v, rx, ry, rz);
    return s.ok && s.depth < 60.0f && s.x > -0.5f && s.x < 1.5f && s.y > -0.5f && s.y < 1.5f;
}

struct Vert {
    float x, y, z;  // world
    float s;        // metres along the line from the rider, for the fade
};

constexpr float kStep      = 2.0f;    // metres between ribbon rows
constexpr float kHalfWidth = 0.35f;   // half the painted line's width
constexpr float kLift      = 0.05f;   // above the surface; polygon offset does the rest
constexpr float kAhead     = 120.0f;  // metres drawn ahead
constexpr float kBehind    = 5.0f;
constexpr float kFadeNear  = 40.0f;
constexpr float kFadeFar   = 120.0f;
constexpr float kRebuildM  = 4.0f;    // rebuild once the rider has moved this far along the lap

inline float Fade(float d) {
    if (d <= kFadeNear) return 1.0f;
    if (d >= kFadeFar) return 0.0f;
    return 1.0f - (d - kFadeNear) / (kFadeFar - kFadeNear);
}

/// The ribbon cache. Rebuilt when the rider has moved kRebuildM along the lap or the sheet or
/// track changed, not per frame; the per-frame cost is one pass over the cached strip.
class Ribbon {
public:
    /// `ref`: Coach's points (lap position and world x/z). `track`: centreline heights.
    /// `pos`: the rider's lap fraction. `bias`: the rider's world y minus the centreline height
    /// under them. Returns true when verts() changed.
    bool update(const std::vector<coachhud::RefPoint>& ref, const coachhud::Track& track, float pos, float bias) {
        if (ref.size() < 2 || !track.ready() || track.length() <= 0 || !std::isfinite(pos) || !std::isfinite(bias)) {
            const bool had = !verts_.empty();
            verts_.clear();
            built_for_ = -1.0f;
            return had;
        }
        const float lapM = track.length();
        if (built_for_ >= 0 && ref_n_ == ref.size() && std::fabs(bias - bias_) < 0.05f) {
            float d = std::fabs(pos - built_for_) * lapM;
            d       = (std::min)(d, lapM - d);
            if (d < kRebuildM) return false;
        }
        built_for_ = pos;
        ref_n_     = ref.size();
        bias_      = bias;
        verts_.clear();
        // Walk Coach's polyline from just behind the rider to kAhead metres on, wrapping the lap.
        const size_t n = ref.size();
        size_t       i0 = 0;
        while (i0 < n && ref[i0].pos < pos) ++i0;
        float  travelled = 0, nextRow = 0;
        size_t i = i0 == 0 ? n - 1 : i0 - 1;  // the segment the rider is on
        for (size_t k = 0; k < n && travelled < kAhead; ++k) {
            const coachhud::RefPoint& a = ref[i];
            const coachhud::RefPoint& b = ref[(i + 1) % n];
            const float dx = b.x - a.x, dz = b.z - a.z;
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len > 1e-3f && len < 200.0f) {
                const float tx = dx / len, tz = dz / len;
                while (nextRow <= travelled + len && nextRow < kAhead) {
                    const float fc  = (std::max)(0.0f, (std::min)(1.0f, (nextRow - travelled) / len));
                    float       lap = a.pos + fc * ((b.pos >= a.pos ? b.pos : b.pos + 1.0f) - a.pos);
                    lap -= std::floor(lap);
                    float h = 0;
                    track.height_at_lap(lap, h);
                    const float cx = a.x + fc * dx, cz = a.z + fc * dz, cy = h + bias + kLift;
                    const float nx = -tz, nz = tx;  // left normal in x/z (y up, z north)
                    verts_.push_back({cx + nx * kHalfWidth, cy, cz + nz * kHalfWidth, nextRow});
                    verts_.push_back({cx - nx * kHalfWidth, cy, cz - nz * kHalfWidth, nextRow});
                    nextRow += kStep;
                }
                travelled += len;
            }
            i = (i + 1) % n;
        }
        return true;
    }
    /// Triangle-strip vertices, two per row (left, right).
    const std::vector<Vert>& verts() const { return verts_; }
    void clear() {
        verts_.clear();
        built_for_ = -1.0f;
        ref_n_     = 0;
    }

private:
    std::vector<Vert> verts_;
    float             built_for_ = -1.0f, bias_ = 0;
    size_t            ref_n_ = 0;
};

}  // namespace coachline
