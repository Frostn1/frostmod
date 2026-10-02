// coachline.h - the Forza/GT-style line to take, painted on the track ground in the 3D view.
// Pure maths, no Win32 and no GL, so tests/coachline_test.cpp can run it. The GL capture and the
// draw live in mxbcoach.cpp. Design: DevHub notes/mxb-coach-ground-line-2026-10-01.md.
//
// READ-ONLY toward the game: it watches the matrices the game hands to OpenGL and draws one extra
// ribbon at swap time. It never writes to the game's memory, its physics or its state.
//
// Where the camera comes from. The plugin API has no camera, so it is taken from the game's own
// fixed-function GL calls. FrostMod's diagnostic (frostmod bf7b2c4; eleven runs on an RTX 5070 at
// 2560x1440) settled how MX Bikes sets it up, and it is not the textbook way:
//
//   glMatrixMode(GL_PROJECTION)
//   glFrustum(-0.018, 0.018, -0.010, 0.010, 0.023, 1000)   the world pass: 16:9, near 2.3 cm
//   glMultMatrixf(view)                                    the CAMERA, multiplied into PROJECTION
//   glMatrixMode(GL_MODELVIEW); glLoadMatrixf(model) ...   one per object: model matrices only
//
// plus six square 90-degree frusta with a 100 m far plane (an environment cube) and ortho passes
// for the HUD. Not one glLoadMatrixf into GL_PROJECTION was ever perspective (18,081 lines,
// `persp=0` throughout), so a capture that waits for one waits forever, and the first rigid
// GL_MODELVIEW load is some object's model matrix, not the camera. So a camera here is a
// glFrustum followed directly by a rigid glMultMatrix on GL_PROJECTION; a perspective
// glLoadMatrix on GL_PROJECTION followed by one is accepted too, should a build switch to it.
//
// A candidate is only trusted when (1) its aspect matches the window's, which rules out the cube
// faces and any mirror or preview, and (2) the camera it implies sits within kMaxCamDist of the
// rider's own bike, which rules out a wrong convention, a wrong pass and the TV/free cameras. A
// frame without such a camera draws nothing.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "coachcue.h"
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

/// The matrix glFrustum multiplies in, as the GL 1.1 spec defines it. False for arguments GL
/// itself would refuse (GL_INVALID_VALUE).
inline bool Frustum(double l, double r, double b, double t, double n, double f, Mat4& out) {
    if (!(n > 0) || !(f > n) || l == r || b == t) return false;
    Mat4 p;
    for (float& v : p.m) v = 0;
    p.m[0]  = float(2 * n / (r - l));
    p.m[5]  = float(2 * n / (t - b));
    p.m[8]  = float((r + l) / (r - l));
    p.m[9]  = float((t + b) / (t - b));
    p.m[10] = float(-(f + n) / (f - n));
    p.m[11] = -1.0f;
    p.m[14] = float(-2 * f * n / (f - n));
    if (!Finite(p.m, 16)) return false;
    out = p;
    return true;
}

/// A perspective projection as glFrustum / gluPerspective build it: m[11] = -1, m[15] = 0, a
/// vertical field of view of 20..140 degrees and a plausible aspect. Anything else (the game's
/// ortho HUD passes) is refused so it cannot pass for the camera.
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

/// Width over height of a projection's view (m[5] / m[0]).
inline float Aspect(const Mat4& p) { return p.m[0] > 0 ? p.m[5] / p.m[0] : 0.0f; }

/// A camera view: rotation and translation only (orthonormal 3x3, bottom row 0 0 0 1). A mirror
/// (determinant -1) is still orthonormal and is accepted: the game may flip an axis.
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
    // Columns at right angles too: a sheared model matrix is not a camera.
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b) {
            const float d = m[a * 4] * m[b * 4] + m[a * 4 + 1] * m[b * 4 + 1] + m[a * 4 + 2] * m[b * 4 + 2];
            if (std::fabs(d) > 0.02f) return false;
        }
    return true;
}

/// Where a rigid view puts the camera, in the coordinates it was built for: -R^T t.
inline void CameraPos(const Mat4& v, float& x, float& y, float& z) {
    const float* m = v.m;
    x = -(m[0] * m[12] + m[1] * m[13] + m[2] * m[14]);
    y = -(m[4] * m[12] + m[5] * m[13] + m[6] * m[14]);
    z = -(m[8] * m[12] + m[9] * m[13] + m[10] * m[14]);
}

/// How the game's GL world relates to the telemetry world (x east, y up, z north). Tried in
/// order; the first that puts the camera next to the rider's bike is the one in use. A flip is
/// applied to telemetry coordinates before they go through the game's matrices.
struct Flip {
    float sx = 1, sz = 1;
};
constexpr Flip kFlips[]  = {{1, 1}, {1, -1}, {-1, 1}};
constexpr int  kNumFlips = 3;

struct Screen {
    float x = 0, y = 0;   // 0..1, origin top left
    float depth = 0;      // clip w: metres in front of the camera
    bool  ok    = false;  // in front of the camera
};

/// Telemetry world to screen fractions through projection `p` and view `v`. After MXBMRP3's
/// worldToScreen, but through the game's own matrices instead of a camera model of our own.
inline Screen Project(const Mat4& p, const Mat4& v, float x, float y, float z, Flip fl = {}) {
    Screen s;
    x *= fl.sx, z *= fl.sz;
    const float* a  = v.m;
    const float  ex = a[0] * x + a[4] * y + a[8] * z + a[12];
    const float  ey = a[1] * x + a[5] * y + a[9] * z + a[13];
    const float  ez = a[2] * x + a[6] * y + a[10] * z + a[14];
    const float* b  = p.m;
    const float  cx = b[0] * ex + b[4] * ey + b[8] * ez + b[12];
    const float  cy = b[1] * ex + b[5] * ey + b[9] * ez + b[13];
    const float  cw = b[3] * ex + b[7] * ey + b[11] * ez + b[15];
    // In front of the camera by a margin: a point at w ~ 0 projects to infinity.
    if (!(cw > 0.05f)) return s;
    s.x     = 0.5f + 0.5f * cx / cw;
    s.y     = 0.5f - 0.5f * cy / cw;
    s.depth = cw;
    s.ok    = std::isfinite(s.x) && std::isfinite(s.y);
    return s;
}

/// The chase and helmet cameras ride with the bike; the TV and free cameras do not, and a wrong
/// convention or pass puts the "camera" somewhere unrelated. Either way: no line.
constexpr float kMaxCamDist = 25.0f;

/// A projection and view pair seen this frame, in the order the game issued them.
struct Candidate {
    Mat4 proj, view;
};

/// Which candidate, if any, is the camera the rider is looking through, and under which flip.
/// `aspect`: the window's width over height. Among those that pass, the last one issued wins: a
/// depth pre-pass and the colour pass share a camera, and anything else drawn in perspective (a
/// cube face, a preview) has already been ruled out by its aspect or its distance.
struct Pick {
    int  index = -1;
    Flip flip;
};
inline Pick PickCamera(const std::vector<Candidate>& cands, float aspect, float rx, float ry, float rz) {
    Pick out;
    if (!(aspect > 0) || !std::isfinite(rx) || !std::isfinite(ry) || !std::isfinite(rz)) return out;
    for (size_t i = 0; i < cands.size(); ++i) {
        const Candidate& c = cands[i];
        if (!ValidProjection(c.proj) || !ValidView(c.view)) continue;
        if (std::fabs(Aspect(c.proj) - aspect) > aspect * 0.08f) continue;
        float cx, cy, cz;
        CameraPos(c.view, cx, cy, cz);
        for (int k = 0; k < kNumFlips; ++k) {
            const Flip  f  = kFlips[k];
            const float dx = cx - rx * f.sx, dy = cy - ry, dz = cz - rz * f.sz;
            if (dx * dx + dy * dy + dz * dz <= kMaxCamDist * kMaxCamDist) {
                out.index = int(i);
                out.flip  = f;
                break;
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// Height

/// The SDK's centreline heights (SPluginsTrackSegment_t::m_fHeight) are the track surface. The
/// rider's telemetry y is the bike's own origin, some way above it, so the gap between the two
/// is the bike's height plus whatever the ground does off the centreline. Averaged over riding
/// it says which case we are in: a gap of about a bike's height means the heights are the
/// surface and the ribbon lies straight on them; anything else (a track that ships every height
/// as 0) and the ribbon is moved by the gap, less a bike's height, so it still lies on the
/// ground. The first version added the whole gap, which floats the line at the bike's middle.
constexpr float kOriginGuess = 0.6f;   // metres from the ground to the bike's telemetry origin
constexpr float kSurfaceLo   = -0.5f;  // a gap in this range means the heights are the surface
constexpr float kSurfaceHi   = 1.5f;

class HeightBias {
public:
    void reset() {
        n_   = 0;
        ema_ = 0;
    }
    /// One sample of rider y minus the centreline height at the rider's lap position. Only fed
    /// while riding (moving, not crashed), so a crash in a ditch does not drag it down.
    void add(float gap) {
        if (!std::isfinite(gap) || std::fabs(gap) > 500.0f) return;
        ema_ = n_ == 0 ? gap : ema_ + kAlpha * (gap - ema_);
        ++n_;
    }
    bool     ready() const { return n_ >= kMinSamples; }
    float    gap() const { return ema_; }
    unsigned samples() const { return n_; }
    /// What to add to the centreline height to reach the ground.
    float offset() const {
        if (n_ == 0) return 0.0f;
        if (ema_ >= kSurfaceLo && ema_ <= kSurfaceHi) return 0.0f;
        return ema_ - kOriginGuess;
    }

private:
    static constexpr float    kAlpha      = 0.004f;  // ~5 s at 50 Hz
    static constexpr unsigned kMinSamples = 50;      // a second of riding
    unsigned                  n_          = 0;
    float                     ema_        = 0;
};

// ---------------------------------------------------------------------------------------
// The ribbon

struct Vert {
    float x, y, z;  // telemetry world
    float s;        // metres along the line from the rider, for the fade
    bool  brake;    // in one of Coach's braking zones
};

constexpr float kStep         = 2.0f;   // metres between ribbon rows
constexpr float kHalfWidth    = 0.35f;  // half the painted line's width
constexpr float kLift         = 0.06f;  // above the surface; polygon offset does the rest
constexpr float kAhead        = 60.0f;  // metres drawn ahead
constexpr float kFadeNear     = 25.0f;
constexpr float kFadeFar      = 60.0f;
constexpr float kRebuildM     = 2.0f;   // rebuild once the rider has moved this far along the lap
constexpr float kMaxBrakeZone = 60.0f;
constexpr float kMaxGapM      = 50.0f;  // two sheet points further apart are not one stretch

inline float Fade(float d) {
    if (d <= kFadeNear) return 1.0f;
    if (d >= kFadeFar) return 0.0f;
    return 1.0f - (d - kFadeNear) / (kFadeFar - kFadeNear);
}

struct Zone {
    float from_m, to_m;  // metres from the line; to_m may run past the lap length (wraps)
};

/// Coach's braking zones from its cue sheet: from each BRAKE cue to the next OFF_BRAKES or
/// THROTTLE cue, at most kMaxBrakeZone long. A sheet without brake cues has none, and the
/// ribbon is all blue.
inline std::vector<Zone> BrakeZones(const coachcue::Sheet& s, float lap_m) {
    std::vector<Zone> out;
    const size_t n = s.cues.size();
    for (size_t i = 0; i < n; ++i) {
        if (s.cues[i].kind != coachcue::BRAKE) continue;
        const float from = s.cues[i].at_m;
        float       to   = from + kMaxBrakeZone;
        for (size_t k = 1; k < n; ++k) {
            const coachcue::Cue& c = s.cues[(i + k) % n];
            if (c.kind != coachcue::OFF_BRAKES && c.kind != coachcue::THROTTLE) continue;
            float at = c.at_m;
            if (at <= from) at += lap_m;
            to = (std::min)(to, at);
            break;
        }
        out.push_back({from, to});
    }
    return out;
}

inline bool InZone(const std::vector<Zone>& zones, float m, float lap_m) {
    for (const Zone& z : zones)
        if ((m >= z.from_m && m < z.to_m) || (m + lap_m >= z.from_m && m + lap_m < z.to_m)) return true;
    return false;
}

/// The ribbon cache. Rebuilt when the rider has moved kRebuildM along the lap, or the height
/// offset or the zones changed, not per frame; the per-frame cost is one pass over the strip.
class Ribbon {
public:
    /// `ref`: Coach's points (lap position and world x/z). `track`: centreline heights.
    /// `pos`: the rider's lap fraction. `offset`: HeightBias::offset(). `zones`: braking zones.
    /// Returns true when verts() changed.
    bool update(const std::vector<coachhud::RefPoint>& ref, const coachhud::Track& track, float pos, float offset,
                const std::vector<Zone>& zones = {}) {
        if (ref.size() < 2 || !track.ready() || track.length() <= 0 || !std::isfinite(pos) || pos < 0 || pos > 1 ||
            !std::isfinite(offset)) {
            const bool had = !verts_.empty();
            clear();
            return had;
        }
        const float lapM = track.length();
        if (built_for_ >= 0 && ref_n_ == ref.size() && zones_n_ == zones.size() && std::fabs(offset - offset_) < 0.1f) {
            float d = std::fabs(pos - built_for_) * lapM;
            d       = (std::min)(d, lapM - d);
            if (d < kRebuildM) return false;
        }
        built_for_ = pos;
        ref_n_     = ref.size();
        zones_n_   = zones.size();
        offset_    = offset;
        verts_.clear();
        // The segment the rider is on: the last point at or behind them, wrapping the lap.
        const size_t n  = ref.size();
        size_t       i0 = 0;
        while (i0 < n && ref[i0].pos <= pos) ++i0;
        size_t i = i0 == 0 ? n - 1 : i0 - 1;
        // Start the walk at the rider, not at the start of their segment.
        float travelled = 0;
        {
            const coachhud::RefPoint& a = ref[i];
            const coachhud::RefPoint& b = ref[(i + 1) % n];
            float span = b.pos - a.pos, at = pos - a.pos;
            if (span <= 0) span += 1.0f;
            if (at < 0) at += 1.0f;
            const float fc = span > 0 ? (std::max)(0.0f, (std::min)(1.0f, at / span)) : 0.0f;
            travelled      = -fc * std::hypot(b.x - a.x, b.z - a.z);
        }
        float nextRow = 0;
        for (size_t k = 0; k < n && nextRow <= kAhead; ++k) {
            const coachhud::RefPoint& a = ref[i];
            const coachhud::RefPoint& b = ref[(i + 1) % n];
            const float dx = b.x - a.x, dz = b.z - a.z;
            const float len = std::sqrt(dx * dx + dz * dz);
            // A long jump between points is the sheet's two ends meeting across the infield.
            if (len >= kMaxGapM) break;
            if (len > 1e-3f) {
                const float tx = dx / len, tz = dz / len;
                while (nextRow <= travelled + len && nextRow <= kAhead) {
                    const float fc  = (std::max)(0.0f, (std::min)(1.0f, (nextRow - travelled) / len));
                    float       lap = a.pos + fc * ((b.pos >= a.pos ? b.pos : b.pos + 1.0f) - a.pos);
                    lap -= std::floor(lap);
                    float h = 0;
                    track.height_at_lap(lap, h);
                    const float cx = a.x + fc * dx, cz = a.z + fc * dz, cy = h + offset + kLift;
                    const float nx = -tz, nz = tx;  // the perpendicular in x/z (y up)
                    const bool  br = InZone(zones, lap * lapM, lapM);
                    verts_.push_back({cx + nx * kHalfWidth, cy, cz + nz * kHalfWidth, nextRow, br});
                    verts_.push_back({cx - nx * kHalfWidth, cy, cz - nz * kHalfWidth, nextRow, br});
                    nextRow += kStep;
                }
            }
            travelled += len;
            i = (i + 1) % n;
        }
        return true;
    }
    /// Triangle-strip vertices, two per row (one each side of the line).
    const std::vector<Vert>& verts() const { return verts_; }
    void clear() {
        verts_.clear();
        built_for_ = -1.0f;
        ref_n_ = zones_n_ = 0;
    }

private:
    std::vector<Vert> verts_;
    float             built_for_ = -1.0f, offset_ = 0;
    size_t            ref_n_ = 0, zones_n_ = 0;
};

/// One row of the ribbon on screen, for the tests and the offline preview: both edges, how far
/// along it is and how opaque. Rows behind the camera are left out.
struct ScreenRow {
    Screen l, r;
    float  s, alpha;
    bool   brake;
};
inline std::vector<ScreenRow> ProjectRibbon(const Mat4& p, const Mat4& v, Flip fl, const std::vector<Vert>& vs) {
    std::vector<ScreenRow> out;
    for (size_t i = 0; i + 1 < vs.size(); i += 2) {
        ScreenRow row;
        row.l     = Project(p, v, vs[i].x, vs[i].y, vs[i].z, fl);
        row.r     = Project(p, v, vs[i + 1].x, vs[i + 1].y, vs[i + 1].z, fl);
        row.s     = vs[i].s;
        row.alpha = Fade(vs[i].s);
        row.brake = vs[i].brake;
        if (row.l.ok && row.r.ok) out.push_back(row);
    }
    return out;
}

}  // namespace coachline
