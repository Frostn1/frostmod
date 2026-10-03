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
// The view is everything multiplied onto GL_PROJECTION after the frustum (glMultMatrix,
// glTranslate, glRotate, glScale) until the game leaves GL_PROJECTION: v0.42.0 took only the
// first glMultMatrix, which a camera assembled from a rotation and then a translation defeats.
//
// A candidate is only trusted when (1) its aspect matches the window's, which rules out the cube
// faces and any mirror or preview, and (2) the camera it implies sits within kMaxCamDist of the
// rider's own bike under some fixed relation between the GL axes and the telemetry axes, the
// same relation kLockFrames frames running (AxesLock). That rules out a wrong pass and the
// TV/free cameras, and finds the axes rather than assuming them. Without it nothing is drawn.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
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

/// glTranslate / glRotate / glScale, as the GL 1.1 spec defines them, so a camera built from
/// them on GL_PROJECTION is followed as faithfully as one handed over whole.
inline Mat4 Translate(float x, float y, float z) {
    Mat4 t;
    t.m[12] = x, t.m[13] = y, t.m[14] = z;
    return t;
}
inline Mat4 Scale(float x, float y, float z) {
    Mat4 s;
    s.m[0] = x, s.m[5] = y, s.m[10] = z;
    return s;
}
inline Mat4 Rotate(float deg, float x, float y, float z) {
    Mat4        r;
    const float len = std::sqrt(x * x + y * y + z * z);
    if (!(len > 0) || !std::isfinite(deg)) return r;
    x /= len, y /= len, z /= len;
    const float a = deg * 0.017453292f, c = std::cos(a), s = std::sin(a), k = 1 - c;
    r.m[0] = x * x * k + c, r.m[4] = x * y * k - z * s, r.m[8] = x * z * k + y * s;
    r.m[1] = y * x * k + z * s, r.m[5] = y * y * k + c, r.m[9] = y * z * k - x * s;
    r.m[2] = x * z * k - y * s, r.m[6] = y * z * k + x * s, r.m[10] = z * z * k + c;
    return r;
}

/// How the game's GL world relates to the telemetry world (x east, y up, z north): GL axis i is
/// telemetry axis src[i] times sgn[i]. All 48 signed permutations are tried, the identity first,
/// so a y-up/z-up swap or a mirrored axis is found rather than assumed away. The first build
/// tried three of them and v0.42.0 in the game picked none.
struct Axes {
    int   src[3] = {0, 1, 2};
    float sgn[3] = {1, 1, 1};
    void apply(float x, float y, float z, float& gx, float& gy, float& gz) const {
        const float t[3] = {x, y, z};
        gx = sgn[0] * t[src[0]], gy = sgn[1] * t[src[1]], gz = sgn[2] * t[src[2]];
    }
};
constexpr int kNumAxes = 48;
/// The i-th signed permutation: identity first, then the sign flips of the identity, then the
/// other orders.
inline Axes AxesAt(int i) {
    static const int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {2, 1, 0}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}};
    Axes a;
    const int p = i / 8, s = i % 8;
    for (int k = 0; k < 3; ++k) {
        a.src[k] = perms[p][k];
        a.sgn[k] = (s >> k) & 1 ? -1.0f : 1.0f;
    }
    return a;
}
/// "x,y,z" or "-z,y,x": which telemetry axis feeds each GL axis, for the log.
inline std::string AxesName(const Axes& a) {
    std::string s;
    for (int k = 0; k < 3; ++k) {
        if (k) s += ',';
        if (a.sgn[k] < 0) s += '-';
        s += char('x' + a.src[k]);
    }
    return s;
}

struct Screen {
    float x = 0, y = 0;   // 0..1, origin top left
    float depth = 0;      // clip w: metres in front of the camera
    bool  ok    = false;  // in front of the camera
};

/// Telemetry world to screen fractions through projection `p` and view `v`. After MXBMRP3's
/// worldToScreen, but through the game's own matrices instead of a camera model of our own.
inline Screen Project(const Mat4& p, const Mat4& v, float x, float y, float z, const Axes& ax = {}) {
    Screen s;
    ax.apply(x, y, z, x, y, z);
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
/// Frames in a row the same axes must win before they are trusted, and then kept for the event.
constexpr int kLockFrames = 30;

/// A projection and view pair seen this frame, in the order the game issued them, with what the
/// diagnostic wants to know about how it was built.
struct Candidate {
    Mat4 proj, view;
    int  ops = 0;      // matrix calls that built the view after the projection
    int  n_mv = 0;     // GL_MODELVIEW loads seen after it, up to 4, by their translation
    float mv[4][3] = {};
};

/// Which candidate, if any, is the camera the rider is looking through, and under which axes.
/// `aspect`: the window's width over height. `locked`: the axes already settled on, or -1 to
/// try them all. The simplest axes that put a camera within kMaxCamDist of the bike win (the
/// identity before a flip, a flip before a swap), and among cameras, the last one issued (a
/// depth pre-pass and the colour pass share one). Nearest-wins would let a wrong swap beat the
/// right axes whenever the bike happens to sit near the world's origin.
struct Pick {
    int   index = -1;
    int   axes  = -1;
    float dist  = 0;
};
inline Pick PickCamera(const std::vector<Candidate>& cands, float aspect, float rx, float ry, float rz,
                       int locked = -1) {
    Pick out;
    if (!(aspect > 0) || !std::isfinite(rx) || !std::isfinite(ry) || !std::isfinite(rz)) return out;
    for (size_t i = 0; i < cands.size(); ++i) {
        const Candidate& c = cands[i];
        if (!ValidProjection(c.proj) || !ValidView(c.view)) continue;
        if (std::fabs(Aspect(c.proj) - aspect) > aspect * 0.08f) continue;
        float cx, cy, cz;
        CameraPos(c.view, cx, cy, cz);
        const int from = locked >= 0 ? locked : 0, to = locked >= 0 ? locked + 1 : kNumAxes;
        for (int k = from; k < to; ++k) {
            if (out.axes >= 0 && k > out.axes) break;
            float gx, gy, gz;
            AxesAt(k).apply(rx, ry, rz, gx, gy, gz);
            const float d2 = (cx - gx) * (cx - gx) + (cy - gy) * (cy - gy) + (cz - gz) * (cz - gz);
            if (d2 <= kMaxCamDist * kMaxCamDist) {
                out.index = int(i);
                out.axes  = k;
                out.dist  = std::sqrt(d2);
                break;
            }
        }
    }
    return out;
}

/// Settles the axes: the same ones must win kLockFrames frames running, and are then kept until
/// reset (a new event). Until then nothing is drawn, so one lucky frame cannot paint a line in
/// the wrong world.
class AxesLock {
public:
    void reset() { last_ = -1, streak_ = 0, locked_ = -1; }
    /// This frame's winner, or -1 for none. Returns true the frame the lock is taken.
    bool vote(int axes) {
        if (locked_ >= 0) return false;
        if (axes < 0) {
            streak_ = 0;
            return false;
        }
        streak_ = axes == last_ ? streak_ + 1 : 1;
        last_   = axes;
        if (streak_ >= kLockFrames) {
            locked_ = axes;
            return true;
        }
        return false;
    }
    int locked() const { return locked_; }

private:
    int last_ = -1, streak_ = 0, locked_ = -1;
};

// ---------------------------------------------------------------------------------------
// The camera from shader uniforms
//
// v0.42.1 in the game (WDR.MX.26.R02): three fixed-function cameras a frame, every one an
// identity rotation with z flipped, unchanged while the bike moved 70 m. The fixed-function
// matrices are not the camera; the game hands it to its shaders. So every glUniformMatrix4fv /
// glProgramUniformMatrix4fv upload is recorded (program, location, matrix), and the camera is
// the upload whose eye follows the bike:
//
//   * a view-projection (or projection-times-view) matrix: perspective in its bottom row. Its
//     eye is the one point it sends to x = y = w = 0, found by solving three linear equations,
//     and it can be drawn through as it stands;
//   * a rigid view matrix: its eye is -R^T t, and it is drawn through with a projection the
//     same frame uploaded, or the fixed-function frustum.
//
// Each upload is read as given and transposed (a shader may multiply row vectors), under all 48
// axes. The (program, location, layout, axes) that wins kLockFrames frames running is kept for
// the event.

/// One matrix uniform as uploaded, column-major as glUniformMatrix4fv takes it untransposed.
struct Upload {
    uint32_t program  = 0;
    int32_t  location = -1;
    Mat4     m;
};

inline Mat4 Transposed(const Mat4& a) {
    Mat4 t;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) t.m[c * 4 + r] = a.m[r * 4 + c];
    return t;
}

/// Has a perspective bottom row (it divides by depth): a projection is in it.
inline bool Perspective(const Mat4& a) {
    return std::fabs(a.m[3]) + std::fabs(a.m[7]) + std::fabs(a.m[11]) > 1e-3f;
}

/// The eye of a camera matrix: for a projective one the point sent to x = y = w = 0, for a rigid
/// one -R^T t. False for anything else (a model matrix with scale, a singular projective one).
inline bool EyeOf(const Mat4& a, float& x, float& y, float& z, bool& perspective) {
    const float* m = a.m;
    if (!Finite(m, 16)) return false;
    perspective = Perspective(a);
    if (!perspective) {
        if (!ValidView(a)) return false;
        CameraPos(a, x, y, z);
        return true;
    }
    // Rows 0, 1 and 3 of M times (x, y, z, 1) = 0: A e = -b, by Cramer's rule.
    const float a00 = m[0], a01 = m[4], a02 = m[8], b0 = m[12];
    const float a10 = m[1], a11 = m[5], a12 = m[9], b1 = m[13];
    const float a20 = m[3], a21 = m[7], a22 = m[11], b2 = m[15];
    const float det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
    if (!(std::fabs(det) > 1e-12f)) return false;
    const float r0 = -b0, r1 = -b1, r2 = -b2;
    x = (r0 * (a11 * a22 - a12 * a21) - a01 * (r1 * a22 - a12 * r2) + a02 * (r1 * a21 - a11 * r2)) / det;
    y = (a00 * (r1 * a22 - a12 * r2) - r0 * (a10 * a22 - a12 * a20) + a02 * (a10 * r2 - r1 * a20)) / det;
    z = (a00 * (a11 * r2 - r1 * a21) - a01 * (a10 * r2 - r1 * a20) + r0 * (a10 * a21 - a11 * a20)) / det;
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

/// The helmet camera is the nearest a real camera sits to the bike's origin.
constexpr float kMinCamDist = 0.3f;

/// Which upload is the camera, read how, under which axes. `key` names it across frames.
struct UniformPick {
    int      index = -1;
    bool     transposed = false, perspective = false;
    int      axes = -1;
    float    dist = 0;
    uint64_t key  = 0;
};

inline uint64_t UniformKey(uint32_t program, int32_t location, bool transposed, int axes) {
    return (uint64_t(program) << 32) ^ (uint64_t(uint32_t(location)) << 8) ^ (uint64_t(transposed) << 7) ^
           uint64_t(axes & 0x7F);
}

/// A rigid matrix whose translation is the bike's position, under any axes: a model matrix
/// placing something at the bike, not a view.
inline bool AtTheBike(const Mat4& m, float rx, float ry, float rz) {
    for (int k = 0; k < kNumAxes; ++k) {
        float gx, gy, gz;
        AxesAt(k).apply(rx, ry, rz, gx, gy, gz);
        const float tx = m.m[12] - gx, ty = m.m[13] - gy, tz = m.m[14] - gz;
        if (tx * tx + ty * ty + tz * tz < kMaxCamDist * kMaxCamDist) return true;
    }
    return false;
}

/// The camera among this frame's uploads. Projective matrices first: their eye is the camera
/// itself, and an object's MVP puts it in that object's own space, near its origin and nowhere
/// near the bike. Only without one is a rigid view taken, and then not one whose translation is
/// the bike's position: that is a model matrix at the bike (the bike, its shadow, the rider),
/// whose -R^T t lands next to the bike under a mirror. Within a kind, the simplest axes first,
/// then the later upload. `locked`: a key already settled on, or 0 to look at everything.
inline UniformPick PickUniform(const std::vector<Upload>& ups, float rx, float ry, float rz, uint64_t locked = 0) {
    UniformPick out;
    if (!std::isfinite(rx) || !std::isfinite(ry) || !std::isfinite(rz)) return out;
    for (int pass = 0; pass < 2 && out.index < 0; ++pass) {
        const bool want_persp = pass == 0;
        for (size_t i = 0; i < ups.size(); ++i) {
            for (int t = 0; t < 2; ++t) {
                const Mat4 m = t ? Transposed(ups[i].m) : ups[i].m;
                float      ex, ey, ez;
                bool       persp = false;
                if (!EyeOf(m, ex, ey, ez, persp) || persp != want_persp) continue;
                for (int k = 0; k < kNumAxes; ++k) {
                    if (out.axes >= 0 && k > out.axes) break;
                    const uint64_t key = UniformKey(ups[i].program, ups[i].location, t != 0, k);
                    if (locked && key != locked) continue;
                    float gx, gy, gz;
                    AxesAt(k).apply(rx, ry, rz, gx, gy, gz);
                    const float d2 = (ex - gx) * (ex - gx) + (ey - gy) * (ey - gy) + (ez - gz) * (ez - gz);
                    if (!(d2 <= kMaxCamDist * kMaxCamDist) || d2 < kMinCamDist * kMinCamDist) continue;
                    if (!persp && AtTheBike(m, rx, ry, rz)) continue;
                    out.index       = int(i);
                    out.transposed  = t != 0;
                    out.perspective = persp;
                    out.axes        = k;
                    out.dist        = std::sqrt(d2);
                    out.key         = key;
                    break;
                }
            }
        }
    }
    return out;
}

/// The same uniform must win kLockFrames frames running; then it is kept until reset.
class KeyLock {
public:
    void reset() { last_ = 0, streak_ = 0, locked_ = 0; }
    bool vote(uint64_t key) {
        if (locked_) return false;
        if (!key) {
            streak_ = 0;
            return false;
        }
        streak_ = key == last_ ? streak_ + 1 : 1;
        last_   = key;
        if (streak_ >= kLockFrames) {
            locked_ = key;
            return true;
        }
        return false;
    }
    uint64_t locked() const { return locked_; }

private:
    uint64_t last_ = 0, locked_ = 0;
    int      streak_ = 0;
};

// ---------------------------------------------------------------------------------------
// The camera from the fixed-function modelview loads
//
// v0.42.2 in the game (755 Compound): no matrix uniform at all, and the projection is the
// frustum times a constant z flip. So the camera is baked into the GL_MODELVIEW matrices the
// game loads, one per object: each is view * model. For anything placed with an identity model
// matrix (the track, the terrain) the load *is* the view, and its eye -R^T t is the camera next
// to the bike. Every rigid modelview load is offered; the frame keeps the one with the simplest
// axes that put its eye 0.3-25 m from the bike (not one placing something at the bike), and the
// axes must hold kLockFrames frames running.

/// The best modelview load of a frame so far, by `ModelviewOffer`.
struct ModelviewPick {
    bool  ok   = false;
    Mat4  view;
    int   axes = -1;
    float dist = 0;
    float jump = 0;  // from the camera being followed, when one is (CameraFollow)
};

/// How far from the bike's origin the chase or helmet camera sits, at most, for the modelview
/// search: tighter than kMaxCamDist, since thousands of scenery loads a frame are offered.
constexpr float kMaxMvCamDist = 12.0f;

/// Offers one modelview load. `locked`: settled axes, or -1. Returns true when it became the
/// frame's best: the eye nearest the bike under any axes (0.3-12 m), not a model matrix placing
/// something at the bike. Nearest, not simplest axes: with thousands of objects a frame, some
/// object's eye lands within a few metres of a mirrored bike, and only the track's is nearest
/// frame after frame - which the lock then requires.
inline bool ModelviewOffer(ModelviewPick& best, const Mat4& m, float rx, float ry, float rz, int locked = -1) {
    if (!std::isfinite(rx) || !ValidView(m)) return false;
    float ex, ey, ez;
    CameraPos(m, ex, ey, ez);
    const int from = locked >= 0 ? locked : 0, to = locked >= 0 ? locked + 1 : kNumAxes;
    bool took = false;
    for (int k = from; k < to; ++k) {
        float gx, gy, gz;
        AxesAt(k).apply(rx, ry, rz, gx, gy, gz);
        const float d2 = (ex - gx) * (ex - gx) + (ey - gy) * (ey - gy) + (ez - gz) * (ez - gz);
        if (!(d2 <= kMaxMvCamDist * kMaxMvCamDist) || d2 < kMinCamDist * kMinCamDist) continue;
        const float d = std::sqrt(d2);
        // Nearest wins; within a centimetre (a rotated copy of the same view is exactly as near)
        // the simpler axes do, so a mesh placed with a quarter turn can't take the lock.
        if (best.ok && (d > best.dist + 0.01f || (d > best.dist - 0.01f && k >= best.axes))) continue;
        if (AtTheBike(m, rx, ry, rz)) return took;
        best.ok = true, best.view = m, best.axes = k, best.dist = d;
        took    = true;
    }
    return took;
}

// ---------------------------------------------------------------------------------------
// The fallback camera: no GL at all
//
// When no camera has been found for kFallbackMs, the helmet view is built from the plugin's own
// telemetry: the eye kHeadUp above the bike's origin and kHeadFwd ahead of it, looking along the
// direction of travel (the velocity, so no yaw or pitch sign convention has to be guessed) tilted
// kHeadDown below it. It is drawn in a right-handed GL world, telemetry (x, y, -z), through the
// projection the game set up. An approximation of the onboard camera only: the chase and TV
// cameras get no line from it, and it is drawn without a depth test, since the game's depth was
// not made through this camera.
constexpr float     kHeadUp      = 0.85f;
constexpr float     kHeadFwd     = 0.15f;
constexpr float     kHeadDown    = 6.0f;   // degrees below the direction of travel
constexpr unsigned  kFallbackMs  = 2000;

inline int FallbackAxes() {
    for (int k = 0; k < kNumAxes; ++k)
        if (AxesName(AxesAt(k)) == "x,y,-z") return k;
    return 0;
}

/// gluLookAt, y up.
inline Mat4 LookAtGL(float ex, float ey, float ez, float ax, float ay, float az) {
    float fx = ax - ex, fy = ay - ey, fz = az - ez;
    const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    Mat4 v;
    if (!(fl > 1e-6f)) return v;
    fx /= fl, fy /= fl, fz /= fl;
    float       sx = -fz, sz = fx;
    const float sl = std::sqrt(sx * sx + sz * sz);
    if (!(sl > 1e-6f)) return v;
    sx /= sl, sz /= sl;
    const float ux = -sz * fy, uy = sz * fx - sx * fz, uz = sx * fy;
    v.m[0] = sx, v.m[4] = 0, v.m[8] = sz;
    v.m[1] = ux, v.m[5] = uy, v.m[9] = uz;
    v.m[2] = -fx, v.m[6] = -fy, v.m[10] = -fz;
    v.m[12] = -(sx * ex + sz * ez);
    v.m[13] = -(ux * ex + uy * ey + uz * ez);
    v.m[14] = fx * ex + fy * ey + fz * ez;
    return v;
}

/// The helmet camera's view, in GL world (telemetry x, y, -z), from the bike's telemetry
/// position and velocity. False while the bike is too slow to say which way it is going.
inline bool OnboardView(float bx, float by, float bz, float vx, float vy, float vz, Mat4& view) {
    const float h = std::sqrt(vx * vx + vz * vz);
    if (!(h > 1.0f) || !std::isfinite(by) || !std::isfinite(vy)) return false;
    const float fx = vx / h, fz = vz / h;                  // heading, telemetry x/z
    const float pitch = std::atan2(vy, h) - kHeadDown * 0.017453292f;
    const float ex = bx + fx * kHeadFwd, ey = by + kHeadUp, ez = bz + fz * kHeadFwd;
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    // To GL: z negated.
    view = LookAtGL(ex, ey, -ez, ex + fx * cp * 10.0f, ey + sp * 10.0f, -(ez + fz * cp * 10.0f));
    return ValidView(view);
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
    float rgba[4];  // its colour, from RibbonExtras::rgba (LineColours) or the cue zones
    float m = 0;    // where on Coach's line it is, metres from the line's first point: fixed in
                    // the world, so a ground correction or a row stays put as the rider moves
};

constexpr float kStep         = 2.0f;   // metres between ribbon rows, on centreline heights
constexpr float kGroundRebuildM = 0.05f; // the rider's ground estimate moving this much rebuilds
constexpr float kRefOffAlpha    = 0.1f;  // how fast the reference lap's offset to the ground follows
constexpr float kStepTerrain  = 0.5f;   // and on the track's own ground, to follow its bumps
constexpr float kHalfWidth    = 0.35f;  // half the painted line's width
constexpr float kLift         = 0.06f;  // above the centreline height; polygon offset does the rest
constexpr float kLiftTerrain  = 0.02f;  // above the track's own ground: painted on, not floating
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
/// THROTTLE cue, at most kMaxBrakeZone long. The fallback when the sheet's lap can't say where
/// it slowed (Tone below): a sheet carries three or four cues, a lap has a corner every few
/// seconds.
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

// ---------------------------------------------------------------------------------------
// How Coach's lap was ridden along the line: its tone
//
// One continuous value per point, 0..3, read off a gradient: 0 green (on the gas), 1 white
// (coasting: off the gas, off the brakes), 2 yellow (light braking), 3 red (heavy braking).
// Made from two continuous channels, a throttle weight and a braking intensity, so a change
// shows as a fade, never a seam:
//
//     tone = 1 - throttle * (1 - brake) + 2 * brake
//
// With the sheet's "DRIV" chunk (throttle, brake, speed), coasting is a throttle under
// kThrottleCoast and full gas from kThrottleOn. Without it there is no throttle channel at all,
// and Sean's Ridgedale run showed what guessing one does: steady speed read as coasting, so the
// line went white where the rider was on the gas. So without DRIV the lap's own speed decides
// only braking; everything that isn't slowing is gas:
//
//     tone = min(1, brake / kCoastBand) + 2 * brake
//
// Deceleration counts toward braking either way. Then the tone is smoothed along the line with a
// Gaussian over time as well as distance (kToneSigmaS at the lap's own speed, never under
// kToneSigmaM), and the ribbon interpolates it between its rows, which GL shades smoothly.

constexpr float kDecelLight  = 1.5f;   // m/s^2: below this, not braking
constexpr float kDecelHard   = 8.0f;   // m/s^2: and from here, hard on the brakes
constexpr float kThrottleCoast = 0.05f; // DRIV throttle: under this, coasting
constexpr float kThrottleOn    = 0.25f; // and from this, on the gas (0.15 is the middle)
constexpr float kCoastBand     = 0.15f; // without DRIV: the first part of braking fades gas to white
constexpr float kToneSigmaS    = 0.12f; // the Gaussian in time: at 30 m/s, 3.6 m
constexpr float kSpeedWindowM = 4.0f;  // metres either side the speed and its change are taken over
constexpr float kToneSigmaM  = 1.5f;   // the Gaussian along the line: a fade over 3-5 m
constexpr float kZoneTone    = 2.0f;   // a braking zone is where the tone reaches yellow
constexpr float kZoneMinM    = 4.0f;   // and stays there this long
constexpr float kZoneMergeM  = 12.0f;  // two zones this close are one corner

/// The tone at each of Coach's points, 0..3. Empty when the lap can't say (fewer than a handful
/// of points, or time that doesn't advance).
inline std::vector<float> Tone(const std::vector<coachhud::RefPoint>& ref, const std::vector<float>& drive = {}) {
    const size_t n = ref.size();
    std::vector<float> out;
    if (n < 8) return out;
    std::vector<float> s(n, 0.0f);  // metres along the lap
    for (size_t i = 1; i < n; ++i) {
        const float d = std::hypot(ref[i].x - ref[i - 1].x, ref[i].z - ref[i - 1].z);
        s[i]          = s[i - 1] + (d < kMaxGapM ? d : 0.0f);
    }
    if (!(s.back() > 50.0f)) return out;
    auto span = [&](size_t i, float w, size_t& lo, size_t& hi) {
        lo = i, hi = i;
        while (lo > 0 && s[i] - s[lo] < w) --lo;
        while (hi + 1 < n && s[hi] - s[i] < w) ++hi;
    };
    const bool have_drive = drive.size() == n * 3;
    std::vector<float> v(n, -1.0f);
    for (size_t i = 0; i < n; ++i) {
        if (have_drive) {
            v[i] = drive[i * 3];
            continue;
        }
        size_t lo, hi;
        span(i, kSpeedWindowM, lo, hi);
        const float dt = ref[hi].t - ref[lo].t;
        if (dt > 1e-3f) v[i] = (s[hi] - s[lo]) / dt;
    }
    std::vector<float> raw(n, 1.0f);
    size_t             known = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t lo, hi;
        span(i, kSpeedWindowM, lo, hi);
        const float dt = ref[hi].t - ref[lo].t;
        if (!(dt > 1e-3f) || v[lo] < 0 || v[hi] < 0) continue;
        ++known;
        const float accel = (v[hi] - v[lo]) / dt;
        auto ramp = [](float x, float a, float b) { return (std::max)(0.0f, (std::min)(1.0f, (x - a) / (b - a))); };
        float brake = ramp(-accel, kDecelLight, kDecelHard);
        if (have_drive) {
            brake                = (std::max)(brake, drive[i * 3 + 2]);
            const float throttle = ramp(drive[i * 3 + 1], kThrottleCoast, kThrottleOn);
            raw[i]               = 1.0f - throttle * (1.0f - brake) + 2.0f * brake;
        } else {
            raw[i] = (std::min)(1.0f, brake / kCoastBand) + 2.0f * brake;
        }
    }
    if (known < n / 2) return out;
    // The Gaussian along the line, out to three sigma: sigma the distance covered in kToneSigmaS
    // at the lap's speed there, so a fast stretch blends over as long a time as a slow one.
    out.assign(n, 1.0f);
    for (size_t i = 0; i < n; ++i) {
        const float sigma = (std::max)(kToneSigmaM, (std::min)(4.0f, (v[i] > 0 ? v[i] : 0.0f) * kToneSigmaS));
        size_t      lo, hi;
        span(i, 3.0f * sigma, lo, hi);
        float sum = 0, wsum = 0;
        for (size_t k = lo; k <= hi; ++k) {
            const float d = (s[k] - s[i]) / sigma;
            const float w = std::exp(-0.5f * d * d);
            sum += w * raw[k], wsum += w;
        }
        out[i] = wsum > 0 ? sum / wsum : raw[i];
    }
    return out;
}

/// The braking zones in a tone profile, as point index ranges [from, to): where the tone holds
/// yellow or redder for kZoneMinM, zones closer than kZoneMergeM joined into one corner.
inline std::vector<std::pair<size_t, size_t>> ToneZones(const std::vector<coachhud::RefPoint>& ref,
                                                        const std::vector<float>& tone) {
    std::vector<std::pair<size_t, size_t>> out;
    if (tone.size() != ref.size()) return out;
    auto dist = [&](size_t a, size_t b) {
        float d = 0;
        for (size_t i = a; i < b && i + 1 < ref.size(); ++i) {
            const float s = std::hypot(ref[i + 1].x - ref[i].x, ref[i + 1].z - ref[i].z);
            d += s < kMaxGapM ? s : 0.0f;
        }
        return d;
    };
    for (size_t i = 0; i < tone.size();) {
        if (tone[i] < kZoneTone) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < tone.size() && tone[j] >= kZoneTone) ++j;
        if (!out.empty() && dist(out.back().second, i) < kZoneMergeM) out.back().second = j;
        else out.push_back({i, j});
        i = j;
    }
    std::vector<std::pair<size_t, size_t>> kept;
    for (const auto& z : out)
        if (dist(z.first, z.second) >= kZoneMinM) kept.push_back(z);
    return kept;
}

/// The gradient: green, white, yellow, red at tone 0, 1, 2, 3, linear between. Saturated a
/// little past the pure colours, so they read on brown dirt at the ribbon's 0.7 alpha.
inline void ToneColour(float t, float& r, float& g, float& b) {
    static const float stops[4][3] = {{0.15f, 0.85f, 0.20f},   // gas
                                      {0.97f, 0.97f, 0.97f},   // coast
                                      {1.00f, 0.86f, 0.05f},   // light brake
                                      {0.95f, 0.12f, 0.08f}};  // heavy brake
    t = (std::max)(0.0f, (std::min)(3.0f, t));
    const int   i = (std::min)(2, int(t));
    const float f = t - float(i);
    r = stops[i][0] + (stops[i + 1][0] - stops[i][0]) * f;
    g = stops[i][1] + (stops[i + 1][1] - stops[i][1]) * f;
    b = stops[i][2] + (stops[i + 1][2] - stops[i][2]) * f;
}

// ---------------------------------------------------------------------------------------
// The track's own ground, from its file
//
// MXB Coach reads the track's heightfield (the .trh inside its .pkz, through mxb-content) the
// moment it sees the track being ridden, and writes it beside the sheets as
// `<track>.ground`, so the line lies on the dirt from the first second of the first lap, with no
// lap needed:
//
//   0   char[4] "MXGR"
//   4   u32     version (1)
//   8   u32     W, columns (2..4096)
//   12  u32     H, rows (2..4096)
//   16  f32     step, metres between samples (0.05..10)
//   20  f32     x0, world x of column 0
//   24  f32     z0, world z of row 0
//   28  f32     base height, metres
//   32  f32     metres per height unit (> 0)
//   36  W x H   u16 row-major (row = z), height = base + unit * value; 0xFFFF where unknown
//
// The grid and the telemetry share the game's world frame; that has been checked against the
// track's placements and against laps (Coach's ground.rs), and is checked again here, once, from
// the rider's own samples (AlignCheck), logged as "ground: trh aligned dx,dz,dy".

constexpr char     kGridMagic[4] = {'M', 'X', 'G', 'R'};
constexpr uint32_t kGridMaxDim   = 4096;

class GroundGrid {
public:
    bool ready() const { return w_ >= 2 && h_ >= 2 && !hs_.empty(); }
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    float    step() const { return step_; }
    /// A fixed shift found by AlignCheck, applied to every lookup.
    void shift(float dx, float dz) { dx_ = dx, dz_ = dz; }
    void clear() {
        hs_.clear();
        w_ = h_ = 0;
        dx_ = dz_ = 0;
    }

    bool parse(const uint8_t* b, size_t n) {
        clear();
        using coachcue::F32;
        using coachcue::U32;
        if (!b || n < 36 || std::memcmp(b, kGridMagic, 4) != 0 || U32(b + 4) != 1) return false;
        const uint32_t w = U32(b + 8), h = U32(b + 12);
        const float    st = F32(b + 16), x0 = F32(b + 20), z0 = F32(b + 24), base = F32(b + 28), unit = F32(b + 32);
        if (w < 2 || h < 2 || w > kGridMaxDim || h > kGridMaxDim) return false;
        if (!std::isfinite(st) || st < 0.05f || st > 10 || !std::isfinite(x0) || !std::isfinite(z0) ||
            !std::isfinite(base) || !std::isfinite(unit) || !(unit > 0))
            return false;
        if (n - 36 < uint64_t(w) * h * 2) return false;
        hs_.resize(size_t(w) * h);
        for (size_t i = 0; i < hs_.size(); ++i) {
            const uint16_t q = uint16_t(b[36 + i * 2] | (b[36 + i * 2 + 1] << 8));
            hs_[i]           = q == 0xFFFF ? NAN : base + unit * float(q);
        }
        w_ = w, h_ = h, step_ = st, x0_ = x0, z0_ = z0;
        return true;
    }

    /// The ground at world (x, z), bilinear; false off the grid or where it is unknown.
    bool at(float x, float z, float& y) const {
        if (!ready() || !std::isfinite(x) || !std::isfinite(z)) return false;
        const float gx = (x + dx_ - x0_) / step_, gz = (z + dz_ - z0_) / step_;
        if (!(gx >= 0) || !(gz >= 0) || gx > float(w_ - 1) || gz > float(h_ - 1)) return false;
        const uint32_t c0 = (std::min)(uint32_t(gx), w_ - 2), r0 = (std::min)(uint32_t(gz), h_ - 2);
        const float    fx = gx - float(c0), fz = gz - float(r0);
        const float    a = hs_[size_t(r0) * w_ + c0], b = hs_[size_t(r0) * w_ + c0 + 1];
        const float    c = hs_[size_t(r0 + 1) * w_ + c0], d = hs_[size_t(r0 + 1) * w_ + c0 + 1];
        if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) || !std::isfinite(d)) return false;
        const float top = a + (b - a) * fx, bot = c + (d - c) * fx;
        y               = top + (bot - top) * fz;
        return true;
    }

private:
    uint32_t           w_ = 0, h_ = 0;
    float              step_ = 1, x0_ = 0, z0_ = 0, dx_ = 0, dz_ = 0;
    std::vector<float> hs_;
};

/// The one-time check that the grid and the rider's telemetry share a frame: over a couple of
/// hundred riding samples, the bike should sit the same height above the grid everywhere. Shifts
/// of up to kAlignSearchM either way are tried; the steadiest is kept (dx, dz), with the bike's
/// height above the ground (dy) and how much it wandered (spread). A spread over kAlignMaxSpread
/// means this grid isn't this track's ground (a renamed folder, a different layout), and the line
/// goes back to the depth snap.
///
/// Only samples with the grid under them count toward the verdict, a metre or more apart, on
/// the ground. Sean's Carson run (v0.45.0): a supercross gate stands off the track's terrain
/// (its .trh is 170 m square; the gate is at x=193), and the first 200 samples were all the run
/// out of it: 20 of them were on the grid, under the half needed, and the right ground was
/// thrown away as "NOT this track's ground (0 samples on the grid)" for the rest of the session. Counting only what
/// lands on the grid, the verdict waits for the track proper; a grid the rider never rides onto
/// at all (a different track) is still refused, after kAlignGiveUp samples off it.
constexpr int   kAlignSamples   = 200;
constexpr int   kAlignSearchM   = 2;
constexpr float kAlignMaxSpread = 1.2f;
constexpr float kAlignStepM     = 1.0f;   // m between the samples kept: 200 m of track, not 4 s of it
constexpr int   kAlignGiveUp    = 3000;   // samples off the grid (a minute of riding) before giving up on it
constexpr int   kAlignMinOnGrid = 50;     // and fewer than this on it by then: not this track's ground

struct Alignment {
    bool  done = false, ok = false;
    float dx = 0, dz = 0, dy = 0, spread = 0;
    int   on_grid = 0;
    int   off_grid = 0;  // samples that had no ground under them on the grid, and didn't count
};

class AlignCheck {
public:
    void reset() {
        pts_.clear();
        off_    = 0;
        result_ = Alignment{};
    }
    /// One riding sample (not crashed, moving). `grounded`: a wheel on the ground, since in the air
    /// the bike's height says nothing about the ground under it. Returns true the call the verdict
    /// is reached.
    bool add(const GroundGrid& g, float x, float y, float z, bool grounded = true) {
        if (result_.done || !g.ready() || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
        if (!grounded) return false;
        float h;
        if (!g.at(x, z, h)) {
            // Off the grid (a gate or a pit lane past the terrain's edge, or another track's
            // grid altogether): no evidence either way, so it doesn't count toward the verdict.
            if (++off_ < kAlignGiveUp) return false;
            if (int(pts_.size()) >= kAlignMinOnGrid) {
                decide(g);
            } else {
                result_          = Alignment{};
                result_.done     = true;
                result_.spread   = INFINITY;
                result_.on_grid  = int(pts_.size());
                result_.off_grid = off_;
                pts_.clear();
            }
            return true;
        }
        if (!pts_.empty() && std::hypot(x - pts_.back().x, z - pts_.back().z) < kAlignStepM) return false;
        pts_.push_back({x, y, z});
        if (int(pts_.size()) < kAlignSamples) return false;
        decide(g);
        return true;
    }
    const Alignment& result() const { return result_; }

private:
    struct P {
        float x, y, z;
    };
    static float Median(std::vector<float>& v) {
        std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
        return v[v.size() / 2];
    }
    void decide(const GroundGrid& g) {
        Alignment best;
        best.done   = true;
        best.spread = INFINITY;
        GroundGrid probe = g;
        for (int dx = -kAlignSearchM; dx <= kAlignSearchM; ++dx)
            for (int dz = -kAlignSearchM; dz <= kAlignSearchM; ++dz) {
                probe.shift(float(dx), float(dz));
                std::vector<float> gaps;
                for (const P& p : pts_) {
                    float h;
                    if (probe.at(p.x, p.z, h)) gaps.push_back(p.y - h);
                }
                if (gaps.size() < pts_.size() / 2) continue;
                const float med = Median(gaps);
                std::vector<float> dev;
                for (float v : gaps) dev.push_back(std::fabs(v - med));
                const float mad = Median(dev);
                // A shift has to earn its place: the plain frame wins unless another is clearly steadier.
                const float score = mad + (dx || dz ? 0.05f : 0.0f);
                if (score < best.spread) {
                    best.spread  = score;
                    best.dx = float(dx), best.dz = float(dz), best.dy = med;
                    best.on_grid = int(gaps.size());
                }
            }
        if (std::isfinite(best.spread) && (best.dx != 0 || best.dz != 0)) best.spread -= 0.05f;
        best.ok = std::isfinite(best.spread) && best.spread <= kAlignMaxSpread && best.dy > -1.0f && best.dy < 3.0f;
        best.off_grid = off_;
        result_ = best;
        pts_.clear();
        pts_.shrink_to_fit();
    }
    std::vector<P> pts_;
    int            off_ = 0;
    Alignment      result_;
};

/// How far above the ground the line is painted: a couple of centimetres on the flat, more where
/// the ground is steep along or across it, where the grid's metre between samples cuts the tops
/// off bumps and lips and a flat 2 cm left the ribbon inside them ("glitching through the track").
constexpr float kLiftBase  = 0.05f;
constexpr float kLiftSlope = 0.12f;  // extra metres per unit of slope
constexpr float kLiftMax   = 0.15f;
inline float LiftFor(float slope_along, float slope_across) {
    const float s = (std::max)(std::fabs(slope_along), std::fabs(slope_across));
    return (std::min)(kLiftMax, kLiftBase + kLiftSlope * (std::isfinite(s) ? s : 0.0f));
}

// ---------------------------------------------------------------------------------------
// Crashes and resets
//
// Sean, v0.43.2: "when I crash it is completely messed up, not aligned anymore, or floating".
// Down, the camera swings to the crash view (its eye nowhere near where it was), the bike
// tumbles, and the game then puts rider and bike back on the track, often metres from where they
// fell. Every one of those broke something the line relies on: the camera being followed, the
// ribbon cache built from the old position, the ground snap's corrections. RiderState says when
// the line must be hidden and when everything built from the old position has to be started
// again (`generation` moves on).

constexpr float    kResetJumpM   = 15.0f;  // a move this far between two samples is the game putting the bike back
constexpr float    kSettleS      = 0.75f;  // after getting up, this long before the line returns
constexpr float    kEyeDriftM    = 1.5f;   // the camera's eye this much further from the bike than usual...
constexpr unsigned kEyeDriftMs   = 300;    // ...for this long, and it is not the camera any more

class RiderState {
public:
    enum Phase { RIDING, DOWN, SETTLING };
    void reset() {
        phase_ = SETTLING, since_ = -1, have_ = false;
        ++gen_;
    }
    /// One telemetry sample: track time, the crashed flag, world position.
    void sample(float t, bool crashed, float x, float y, float z) {
        const bool jump = have_ && std::isfinite(x) &&
                          std::sqrt((x - x_) * (x - x_) + (y - y_) * (y - y_) + (z - z_) * (z - z_)) > kResetJumpM;
        have_ = std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
        if (have_) x_ = x, y_ = y, z_ = z;
        if (crashed) {
            if (phase_ != DOWN) ++gen_, ++crashes_;
            phase_ = DOWN;
            return;
        }
        if (jump) {
            ++gen_, ++resets_;
            phase_ = SETTLING, since_ = t;
            return;
        }
        if (phase_ == DOWN) phase_ = SETTLING, since_ = t, ++gen_;
        if (phase_ == SETTLING) {
            if (since_ < 0 || t < since_) since_ = t;
            if (t - since_ >= kSettleS) phase_ = RIDING;
        }
    }
    bool     visible() const { return phase_ == RIDING; }
    Phase    phase() const { return phase_; }
    /// Moves on at every crash, every getting up and every reset: anything built from the old
    /// position (the ribbon, the ground snap, the camera being followed) is started again.
    unsigned generation() const { return gen_; }
    unsigned crashes() const { return crashes_; }
    unsigned resets() const { return resets_; }

private:
    Phase    phase_ = SETTLING;
    float    since_ = -1, x_ = 0, y_ = 0, z_ = 0;
    bool     have_ = false;
    unsigned gen_ = 0, crashes_ = 0, resets_ = 0;
};

/// The camera's eye against the bike: learns the usual distance (the chase or helmet camera
/// rides at a steady one) and says when it has been off by more than kEyeDriftM for kEyeDriftMs -
/// the crash camera, a replay view, or the wrong object followed - so the lock is dropped and found
/// again rather than drawn through.
class EyeWatch {
public:
    void reset() { n_ = 0, off_since_ = 0, off_ = false; }
    /// Returns false when the camera should be let go.
    bool ok(float dist, unsigned long long now) {
        if (!std::isfinite(dist)) return false;
        if (n_ < 30) {
            usual_ = n_ == 0 ? dist : usual_ + (dist - usual_) / float(n_ + 1);
            ++n_;
            return true;
        }
        if (std::fabs(dist - usual_) > kEyeDriftM) {
            if (!off_) off_ = true, off_since_ = now;
            return now - off_since_ < kEyeDriftMs;
        }
        off_   = false;
        usual_ += 0.01f * (dist - usual_);
        return true;
    }
    float usual() const { return usual_; }

private:
    int                n_ = 0;
    float              usual_ = 0;
    bool               off_ = false;
    unsigned long long off_since_ = 0;
};

/// The line's alpha: readable on dirt without hiding it.
constexpr float kLineAlpha = 0.7f;

/// Where the colours come from, for the log: "DRIV" (Coach's throttle and brake), "speed" (the
/// lap's own times), or "cues" (the brake cues alone, when the lap can't say).
inline const char* ColourSource(const std::vector<coachhud::RefPoint>& ref, const std::vector<float>& drive) {
    if (drive.size() == ref.size() * 3 && !ref.empty()) return "DRIV";
    return Tone(ref, drive).empty() ? "cues" : "speed";
}

/// The line's colour at each of Coach's points, RGBA (4 floats a point), or empty when the lap
/// can't say. Today the Tone gradient; the one place a different colouring plugs in.
inline std::vector<float> LineColours(const std::vector<coachhud::RefPoint>& ref, const std::vector<float>& drive = {}) {
    const std::vector<float> tone = Tone(ref, drive);
    std::vector<float>       out;
    out.reserve(tone.size() * 4);
    for (float t : tone) {
        float r, g, b;
        ToneColour(t, r, g, b);
        out.insert(out.end(), {r, g, b, kLineAlpha});
    }
    return out;
}

/// What the sheet adds to the ribbon beyond its points: the ground across the line ("TRRN") and
/// how the lap was ridden at each point (Tone). Built once per sheet; `version` changes with it.
struct RibbonExtras {
    uint32_t           version = 0;
    uint32_t           terrain_k = 0;
    float              terrain_step = 0;
    std::vector<float> terrain;  // per point x K, NaN off the grid
    // The line's colour at each point, RGBA, 4 floats a point; empty when unknown. Filled by
    // one function (LineColours today), so a better colouring is a one-line swap.
    std::vector<float> rgba;
    // The track's own ground grid (`<track>.ground`), when Coach has written it and it lines up
    // with the rider (AlignCheck); null otherwise. Takes precedence over everything else.
    const GroundGrid*  grid = nullptr;
    // The reference lap's own height at each point ("REFY": the bike's y as Coach recorded it),
    // empty without one. Its rise from point to point is the ground's along the line.
    std::vector<float> refy;
};

/// The track's ground `off` metres to the left of point `i` (negative: right), from its TRRN
/// row. False off the grid or without one.
inline bool TerrainAt(const RibbonExtras& ex, size_t i, float off, float& h) {
    const uint32_t k = ex.terrain_k;
    if (!k || ex.terrain.size() < (i + 1) * k) return false;
    float f = off / ex.terrain_step + float(k - 1) * 0.5f;
    if (f < 0 || f > float(k - 1)) return false;
    const size_t j  = (std::min)(size_t(f), size_t(k - 2));
    const float  fr = f - float(j);
    const float  a = ex.terrain[i * k + j], b = ex.terrain[i * k + j + 1];
    if (!std::isfinite(a) || !std::isfinite(b)) return false;
    h = a + (b - a) * fr;
    return true;
}

/// Where the rider is, to start the ribbon from. Sean's Ridgedale run (v0.43.2): the line
/// "started on the track, floating, then turned 90 degrees". Coach numbers its points by metres
/// along the lap it recorded, divided by the track's length, so on a lap ridden longer or shorter
/// than the centreline the sheet's lap positions drift from the game's - by the end of a lap, tens
/// of metres. Starting the ribbon at the sheet point with the rider's lap position put it that far
/// up or down the track, across the corner in front of the rider, with its heights and colours
/// from the wrong place too. So the ribbon starts at the sheet point nearest the rider's own world
/// position (among those whose lap position is within kAnchorLap of theirs, so an overpass or a
/// neighbouring straight can't take it), and its heights, without the track's own ground, are the
/// rider's own ground plus how the centreline rises from there.
struct Anchor {
    float x = NAN, z = NAN;  // the rider, world x/z
    float ground = NAN;      // the ground under the rider, metres (telemetry y less the bike's height)
};
constexpr float kAnchorLap  = 0.25f;  // of a lap, either way
constexpr float kAnchorMaxM = 30.0f;  // further than this from every point: not on Coach's line

/// The sheet point the ribbon starts from for a rider at (x, z) and lap position `pos`, and how
/// far along its segment the rider is (0..1). -1 when no point is near enough.
inline int AnchorPoint(const std::vector<coachhud::RefPoint>& ref, float x, float z, float pos, float& frac) {
    frac = 0;
    if (ref.size() < 2 || !std::isfinite(x) || !std::isfinite(z)) return -1;
    const size_t n    = ref.size();
    int          best = -1;
    float        bd   = kAnchorMaxM * kAnchorMaxM;
    for (size_t i = 0; i < n; ++i) {
        float dl = std::fabs(ref[i].pos - pos);
        dl       = (std::min)(dl, 1.0f - dl);
        if (dl > kAnchorLap) continue;
        const float d2 = (ref[i].x - x) * (ref[i].x - x) + (ref[i].z - z) * (ref[i].z - z);
        if (d2 < bd) bd = d2, best = int(i);
    }
    if (best < 0) return -1;
    // The segment the rider is on: from the nearest point forward, or the one before it when the
    // rider hasn't reached it yet.
    auto proj = [&](size_t a) {
        const coachhud::RefPoint& p = ref[a];
        const coachhud::RefPoint& q = ref[(a + 1) % n];
        const float dx = q.x - p.x, dz = q.z - p.z, l2 = dx * dx + dz * dz;
        return l2 > 1e-6f ? ((x - p.x) * dx + (z - p.z) * dz) / l2 : 0.0f;
    };
    size_t a = size_t(best);
    float  f = proj(a);
    if (f < 0) {
        const size_t b = (a + n - 1) % n;
        a              = b;
        f              = proj(b);
    }
    frac = (std::max)(0.0f, (std::min)(1.0f, f));
    return int(a);
}

/// The ribbon cache. Rebuilt when the rider has moved kRebuildM, or the height offset, the zones
/// or the sheet changed, not per frame; the per-frame cost is one pass.
class Ribbon {
public:
    /// `ref`: Coach's points (lap position and world x/z). `track`: centreline heights.
    /// `pos`: the rider's lap fraction. `offset`: HeightBias::offset(). `zones`: cue braking
    /// zones, used only when `ex` has no colours. `ex`: the sheet's ground and colours, or null.
    /// `at`: the rider's world position and ground (Anchor); without it the ribbon starts at the
    /// rider's lap position and stands on centreline heights, as before.
    /// Returns true when verts() changed.
    bool update(const std::vector<coachhud::RefPoint>& ref, const coachhud::Track& track, float pos, float offset,
                const std::vector<Zone>& zones = {}, const RibbonExtras* ex = nullptr, const Anchor* at = nullptr) {
        if (ref.size() < 2 || !track.ready() || track.length() <= 0 || !std::isfinite(pos) || pos < 0 || pos > 1 ||
            !std::isfinite(offset)) {
            const bool had = !verts_.empty();
            clear();
            return had;
        }
        const float    lapM = track.length();
        const uint32_t ver  = ex ? ex->version : 0;
        const bool anchored = at && std::isfinite(at->x) && std::isfinite(at->z);
        const float ground  = anchored && std::isfinite(at->ground) ? at->ground : NAN;
        if (built_for_ >= 0 && ref_n_ == ref.size() && zones_n_ == zones.size() && ver_ == ver && grid_ == (ex ? ex->grid : nullptr) &&
            std::fabs(offset - offset_) < 0.1f && (std::isfinite(ground) == std::isfinite(ground_)) &&
            !(std::isfinite(ground) && std::fabs(ground - ground_) > kGroundRebuildM)) {
            float d;
            if (anchored) {
                d = std::hypot(at->x - bx_, at->z - bz_);
            } else {
                d = std::fabs(pos - built_for_) * lapM;
                d = (std::min)(d, lapM - d);
            }
            if (d < kRebuildM) return false;
        }
        built_for_ = pos;
        grid_      = ex ? ex->grid : nullptr;
        bx_        = anchored ? at->x : NAN;
        bz_        = anchored ? at->z : NAN;
        ground_    = ground;
        ref_n_     = ref.size();
        zones_n_   = zones.size();
        ver_       = ver;
        offset_    = offset;
        // The rows already drawn, by where they are on the line, to measure how far a rebuild moves
        // them (jitter(): it should be nothing sideways and centimetres up or down).
        std::vector<Vert> old;
        old.swap(verts_);
        const bool  terrain = ex && ex->terrain_k && ex->terrain.size() == ref.size() * ex->terrain_k;
        const bool  coloured = ex && ex->rgba.size() == ref.size() * 4;
        // Rows every half metre either way: the ground's bumps need them, and so does the colour,
        // which fades over a few metres and must not be stepped.
        const float step    = kStepTerrain;
        // The segment the rider is on: by where they are when that is known (AnchorPoint), else
        // the last point at or behind their lap position, wrapping the lap.
        const size_t n  = ref.size();
        // Metres along Coach's line at each point, and round the whole of it.
        std::vector<float> arc(n, 0.0f);
        for (size_t k = 1; k < n; ++k) {
            const float d = std::hypot(ref[k].x - ref[k - 1].x, ref[k].z - ref[k - 1].z);
            arc[k]        = arc[k - 1] + (d < kMaxGapM ? d : 0.0f);
        }
        const float close = std::hypot(ref[0].x - ref[n - 1].x, ref[0].z - ref[n - 1].z);
        const float total = arc[n - 1] + (close < kMaxGapM ? close : 0.0f);
        const bool  refh  = ex && ex->refy.size() == n;
        float        afrac = 0;
        const int    ai    = anchored ? AnchorPoint(ref, at->x, at->z, pos, afrac) : -1;
        size_t       i0    = 0;
        while (i0 < n && ref[i0].pos <= pos) ++i0;
        size_t i = ai >= 0 ? size_t(ai) : (i0 == 0 ? n - 1 : i0 - 1);
        // Start the walk at the rider, not at the start of their segment.
        float travelled = 0;
        float base_h    = 0;
        if (std::isfinite(ground)) track.height_at_lap(pos, base_h);
        if (ai >= 0) {
            travelled = -afrac * std::hypot(ref[(i + 1) % n].x - ref[i].x, ref[(i + 1) % n].z - ref[i].z);
        } else {
            const coachhud::RefPoint& a = ref[i];
            const coachhud::RefPoint& b = ref[(i + 1) % n];
            float span = b.pos - a.pos, along = pos - a.pos;
            if (span <= 0) span += 1.0f;
            if (along < 0) along += 1.0f;
            const float fc = span > 0 ? (std::max)(0.0f, (std::min)(1.0f, along / span)) : 0.0f;
            travelled      = -fc * std::hypot(b.x - a.x, b.z - a.z);
        }
        // The rows sit at fixed places on the line - every `step` metres from its first point - so
        // a rebuild two metres on draws the same rows where they overlap, rather than new ones
        // half a step along that sample the ground and the colours somewhere else ("moves weirdly").
        const float m0      = arc[i] - travelled;
        float       nextRow = std::ceil(m0 / step - 1e-4f) * step - m0;
        // The reference lap's height where the rider is, for the rise to each row (REFY).
        // The reference lap's height is the bike's own, recorded: the ground is it less a steady
        // offset. That offset is learnt slowly from the rider's ground estimate at each rebuild,
        // so a rebuild moves the rows by a fraction of how the estimate moved, not all of it.
        float base_ry = NAN;
        if (refh && std::isfinite(ground)) {
            const size_t ib0 = (i + 1) % n;
            const float  sl  = std::hypot(ref[ib0].x - ref[i].x, ref[ib0].z - ref[i].z);
            const float  f0  = sl > 1e-3f ? (std::max)(0.0f, (std::min)(1.0f, -travelled / sl)) : 0.0f;
            base_ry          = ex->refy[i] + (ex->refy[ib0] - ex->refy[i]) * f0;
            const float want = ground - base_ry;
            ref_off_         = std::isfinite(ref_off_) && std::fabs(want - ref_off_) < 3.0f ? ref_off_ + kRefOffAlpha * (want - ref_off_) : want;
        }
        float prev_mid = NAN;  // the ground under the last row's centre, for the slope along
        for (size_t k = 0; k < n && nextRow <= kAhead; ++k) {
            const size_t              ib = (i + 1) % n;
            const coachhud::RefPoint& a  = ref[i];
            const coachhud::RefPoint& b  = ref[ib];
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
                    const float cx = a.x + fc * dx, cz = a.z + fc * dz;
                    float       yl, yr;
                    if (std::isfinite(ground) && std::isfinite(base_ry)) {
                        // The rider's own ground, and how Coach's own lap rose from there to this
                        // row: the ground along the line itself, not the centreline's.
                        const float ry = ex->refy[i] + (ex->refy[ib] - ex->refy[i]) * fc;
                        yl = yr = ry + ref_off_ + kLiftBase;
                    } else if (std::isfinite(ground)) {
                        // The rider's own ground, and the centreline's rise from where they are to
                        // this row - measured along the game's own lap, not the sheet's.
                        float lr = pos + nextRow / lapM;
                        lr -= std::floor(lr);
                        track.height_at_lap(lr, h);
                        yl = yr = ground + (h - base_h) + kLiftBase;
                    } else {
                        track.height_at_lap(lap, h);
                        yl = yr = h + offset + kLift;
                    }
                    const float nx = -tz, nz = tx;  // the left perpendicular in x/z (y up)
                    // On the track's own ground grid: each edge on the ground under it, lifted by
                    // how steep it is there along and across.
                    float gl, gr;
                    if (ex && ex->grid && ex->grid->at(cx + nx * kHalfWidth, cz + nz * kHalfWidth, gl) &&
                        ex->grid->at(cx - nx * kHalfWidth, cz - nz * kHalfWidth, gr)) {
                        const float mid   = (gl + gr) * 0.5f;
                        const float along = std::isfinite(prev_mid) ? (mid - prev_mid) / step : 0.0f;
                        const float lift  = LiftFor(along, (gl - gr) / (2 * kHalfWidth));
                        yl = gl + lift, yr = gr + lift;
                        prev_mid = mid;
                    } else if (terrain) {
                        float la, lb, ra, rb;
                        if (TerrainAt(*ex, i, kHalfWidth, la) && TerrainAt(*ex, ib, kHalfWidth, lb) &&
                            TerrainAt(*ex, i, -kHalfWidth, ra) && TerrainAt(*ex, ib, -kHalfWidth, rb)) {
                            const float l = la + (lb - la) * fc, r = ra + (rb - ra) * fc, mid = (l + r) * 0.5f;
                            const float along = std::isfinite(prev_mid) ? (mid - prev_mid) / step : 0.0f;
                            const float lift  = LiftFor(along, (l - r) / (2 * kHalfWidth));
                            yl = l + lift, yr = r + lift;
                            prev_mid = mid;
                        }
                    }
                    // The point colours, blended between the two points the row sits between;
                    // without any, the cue zones: white, red inside a brake cue's zone.
                    float col[4];
                    if (coloured) {
                        for (int c = 0; c < 4; ++c)
                            col[c] = ex->rgba[i * 4 + c] + (ex->rgba[ib * 4 + c] - ex->rgba[i * 4 + c]) * fc;
                    } else {
                        ToneColour(InZone(zones, lap * lapM, lapM) ? 3.0f : 1.0f, col[0], col[1], col[2]);
                        col[3] = kLineAlpha;
                    }
                    float mm = total > 0 ? std::fmod(m0 + nextRow, total) : m0 + nextRow;
                    if (mm < 0) mm += total;
                    verts_.push_back({cx + nx * kHalfWidth, yl, cz + nz * kHalfWidth, nextRow, {col[0], col[1], col[2], col[3]}, mm});
                    verts_.push_back({cx - nx * kHalfWidth, yr, cz - nz * kHalfWidth, nextRow, {col[0], col[1], col[2], col[3]}, mm});
                    nextRow += step;
                }
            }
            travelled += len;
            i = ib;
        }
        // How far the rows both builds share moved: the jitter a rider sees.
        if (!old.empty()) {
            for (size_t a = 0; a + 1 < verts_.size(); a += 2) {
                const long key = std::lround(verts_[a].m / step);
                for (size_t b = 0; b + 1 < old.size(); b += 2) {
                    if (std::lround(old[b].m / step) != key) continue;
                    max_dy_    = (std::max)(max_dy_, std::fabs(verts_[a].y - old[b].y));
                    max_shift_ = (std::max)(max_shift_, std::hypot(verts_[a].x - old[b].x, verts_[a].z - old[b].z));
                    break;
                }
            }
        }
        return true;
    }
    /// Triangle-strip vertices, two per row (left, then right).
    const std::vector<Vert>& verts() const { return verts_; }
    /// The most any shared row moved up or down, and sideways, between builds since the last
    /// call; then starts again.
    void jitter(float& dy, float& shift) {
        dy = max_dy_, shift = max_shift_;
        max_dy_ = max_shift_ = 0;
    }
    void clear() {
        verts_.clear();
        ref_off_   = NAN;
        built_for_ = -1.0f;
        ref_n_ = zones_n_ = 0;
        ver_ = 0;
    }

private:
    std::vector<Vert> verts_;
    float             built_for_ = -1.0f, offset_ = 0;
    float             bx_ = NAN, bz_ = NAN, ground_ = NAN;
    const GroundGrid* grid_ = nullptr;
    float             max_dy_ = 0, max_shift_ = 0;
    float             ref_off_ = NAN;  // the ground under the reference lap's bike, against it
    size_t            ref_n_ = 0, zones_n_ = 0;
    uint32_t          ver_ = 0;
};

// ---------------------------------------------------------------------------------------
// Keeping the camera steady
//
// v0.43.1 in the game: the source flapped between "modelview" and "searching" several times a
// second, and two frames in three that picked a camera drew nothing. A frame without a camera
// load near the bike (a menu overlay, a frame the track chunk isn't drawn) dropped everything.
// Now the camera found is followed from frame to frame: a frame's candidate must sit within
// kJumpM of the last camera to be taken (so a one-frame outlier is never drawn from), a miss is
// carried for kHoldMs before the search starts over, and the last camera may be redrawn for
// kStaleMs so a single miss doesn't blink the line.
constexpr float     kJumpM   = 3.0f;
constexpr unsigned  kHoldMs  = 500;
constexpr unsigned  kStaleMs = 100;

class CameraFollow {
public:
    void reset() { has_ = false; }
    /// The last camera's eye to stay near, or null to search freely.
    const float* anchor(unsigned long long now) const { return holding(now) ? eye_ : nullptr; }
    bool holding(unsigned long long now) const { return has_ && now - last_ms_ <= kHoldMs; }
    bool fresh(unsigned long long now) const { return has_ && now - last_ms_ <= kStaleMs; }
    void hit(const float eye[3], unsigned long long now) {
        eye_[0] = eye[0], eye_[1] = eye[1], eye_[2] = eye[2];
        last_ms_ = now;
        has_     = true;
    }

private:
    bool               has_ = false;
    float              eye_[3] = {0, 0, 0};
    unsigned long long last_ms_ = 0;
};

/// ModelviewOffer for a camera that has to stay where the last one was: the eye must also be
/// within kJumpM of `prev`, and the nearest to it wins.
inline bool ModelviewFollow(ModelviewPick& best, const Mat4& m, float rx, float ry, float rz, int axes,
                            const float prev[3]) {
    if (!ValidView(m) || axes < 0) return false;
    float ex, ey, ez;
    CameraPos(m, ex, ey, ez);
    const float j2 = (ex - prev[0]) * (ex - prev[0]) + (ey - prev[1]) * (ey - prev[1]) + (ez - prev[2]) * (ez - prev[2]);
    if (!(j2 <= kJumpM * kJumpM)) return false;
    float gx, gy, gz;
    AxesAt(axes).apply(rx, ry, rz, gx, gy, gz);
    const float d2 = (ex - gx) * (ex - gx) + (ey - gy) * (ey - gy) + (ez - gz) * (ez - gz);
    if (!(d2 <= kMaxMvCamDist * kMaxMvCamDist) || d2 < kMinCamDist * kMinCamDist) return false;
    const float j = std::sqrt(j2);
    if (best.ok && j >= best.jump) return false;
    best.ok = true, best.view = m, best.axes = axes, best.dist = std::sqrt(d2), best.jump = j;
    return true;
}

// ---------------------------------------------------------------------------------------
// Snapping to the ground the game drew
//
// Without the track's own ground in the sheet, the ribbon's heights are an estimate (the rider's
// ground plus the centreline's rise), and an estimate is either in the air or under the dirt -
// where the depth test hides it. So the visible surface is read back: a pixel of the game's depth
// buffer under the ribbon at a few distances ahead (kSnapAt), unprojected through the camera to
// the world point the game drew there, compared with the ribbon's own height at that point along
// it, and the difference smoothed into a correction by distance ahead that every vertex takes.

/// The inverse of a 4x4 (cofactors). False when it is singular.
inline bool Inverse(const Mat4& a, Mat4& out) {
    const float* m = a.m;
    float        inv[16];
    inv[0]  = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4]  = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8]  = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1]  = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5]  = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9]  = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2]  = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6]  = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3]  = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7]  = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!(std::fabs(det) > 1e-20f) || !std::isfinite(det)) return false;
    for (int i = 0; i < 16; ++i) out.m[i] = inv[i] / det;
    return Finite(out.m, 16);
}

/// GL coordinates back to telemetry: the inverse of Axes::apply.
inline void Unapply(const Axes& a, float gx, float gy, float gz, float& x, float& y, float& z) {
    float       t[3] = {0, 0, 0};
    const float g[3] = {gx, gy, gz};
    for (int k = 0; k < 3; ++k) t[a.src[k]] = g[k] * a.sgn[k];
    x = t[0], y = t[1], z = t[2];
}

/// The telemetry world point drawn at screen fraction (sx, sy) (origin top left) with window
/// depth `d` (0..1, the default depth range), through projection `p`, view `v` and axes `ax`.
inline bool Unproject(const Mat4& p, const Mat4& v, const Axes& ax, float sx, float sy, float d, float& x, float& y,
                      float& z, float dnear = 0.0f, float dfar = 1.0f) {
    Mat4 inv;
    if (!Inverse(Mul(p, v), inv) || !(std::fabs(dfar - dnear) > 1e-6f)) return false;
    // Window depth back to NDC through the depth range glDepthRange set (0..1 unless the game
    // splits it between passes).
    const float nx = sx * 2 - 1, ny = 1 - sy * 2, nz = (d - dnear) / (dfar - dnear) * 2 - 1;
    const float* m  = inv.m;
    const float  wx = m[0] * nx + m[4] * ny + m[8] * nz + m[12];
    const float  wy = m[1] * nx + m[5] * ny + m[9] * nz + m[13];
    const float  wz = m[2] * nx + m[6] * ny + m[10] * nz + m[14];
    const float  ww = m[3] * nx + m[7] * ny + m[11] * nz + m[15];
    if (!(std::fabs(ww) > 1e-9f)) return false;
    Unapply(ax, wx / ww, wy / ww, wz / ww, x, y, z);
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

/// Distances ahead, along the ribbon, the depth buffer is read at.
constexpr int   kSnapN               = 4;
constexpr float kSnapAt[kSnapN]      = {6.0f, 12.0f, 20.0f, 32.0f};
constexpr float kSnapMaxM            = 3.0f;   // a correction bigger than this is something else
constexpr float kSnapLateralM        = 2.0f;   // the hit must be this close to the line across it
constexpr float kSnapAlpha           = 0.25f;  // each reading's weight: one droplet in the depth can't move it
constexpr float kSnapJumpM           = 0.6f;   // a reading this far from the correction already held is an outlier
constexpr unsigned kSnapEveryMs      = 200;

/// The height correction by distance ahead.
class DepthSnap {
public:
    void reset() {
        for (int k = 0; k < kSnapN; ++k) have_[k] = false, corr_[k] = 0, outliers_[k] = 0;
    }
    /// A reading for probe `k`: the game's surface is `dy` above (negative: below) the ribbon.
    /// False when it is refused as implausible (a bike, a rider, the sky, a far hillside).
    bool feed(int k, float dy) {
        if (k < 0 || k >= kSnapN || !std::isfinite(dy) || std::fabs(dy) > kSnapMaxM) return false;
        // Rain, a rider crossing, a flag: a reading far from what is held is ignored, unless the
        // same far value keeps coming (the ground really is there), which the slow blend follows.
        if (have_[k] && std::fabs(dy - corr_[k]) > kSnapJumpM) {
            if (++outliers_[k] < 3) return false;
        }
        outliers_[k] = 0;
        corr_[k] = have_[k] ? corr_[k] + kSnapAlpha * (dy - corr_[k]) : dy;
        corr_[k] = (std::max)(-kSnapMaxM, (std::min)(kSnapMaxM, corr_[k]));
        have_[k] = true;
        return true;
    }
    bool any() const {
        for (bool h : have_)
            if (h) return true;
        return false;
    }
    /// The correction `s` metres ahead: between the probes linearly, flat beyond them, from the
    /// probes that have a reading.
    float at(float s) const {
        if (!std::isfinite(s)) return 0;
        int   lo = -1, hi = -1;
        for (int k = 0; k < kSnapN; ++k) {
            if (!have_[k]) continue;
            if (kSnapAt[k] <= s) lo = k;
            if (kSnapAt[k] >= s && hi < 0) hi = k;
        }
        if (lo < 0 && hi < 0) return 0;
        if (lo < 0) return corr_[hi];
        if (hi < 0 || hi == lo) return corr_[lo];
        const float f = (s - kSnapAt[lo]) / (kSnapAt[hi] - kSnapAt[lo]);
        return corr_[lo] + (corr_[hi] - corr_[lo]) * f;
    }
    float value(int k) const { return have_[k] ? corr_[k] : NAN; }

private:
    bool  have_[kSnapN]     = {false, false, false, false};
    float corr_[kSnapN]     = {0, 0, 0, 0};
    int   outliers_[kSnapN] = {0, 0, 0, 0};
};

/// One probe's reading from a depth read back at the ribbon's centre `s` metres ahead: the world
/// point the game drew there (`hx, hy, hz`), against the ribbon (`verts`, already corrected by
/// `snap` as drawn). Returns the height difference at the hit's place along the ribbon, or NaN
/// when the hit isn't the ground beside the line: too far across it, near the rider's bike, or off
/// the end of the ribbon.
inline float SnapReading(const std::vector<Vert>& verts, const DepthSnap& snap, float hx, float hy, float hz,
                         float rider_x, float rider_z) {
    if (verts.size() < 4) return NAN;
    if (std::hypot(hx - rider_x, hz - rider_z) < 2.5f) return NAN;  // the bike, or the rider's own legs
    // The ribbon row nearest the hit, in x/z, and how far across the line the hit is.
    size_t best = 0;
    float  bd   = INFINITY;
    for (size_t i = 0; i + 1 < verts.size(); i += 2) {
        const float cx = (verts[i].x + verts[i + 1].x) * 0.5f, cz = (verts[i].z + verts[i + 1].z) * 0.5f;
        const float d2 = (cx - hx) * (cx - hx) + (cz - hz) * (cz - hz);
        if (d2 < bd) bd = d2, best = i;
    }
    if (!(bd <= kSnapLateralM * kSnapLateralM)) return NAN;
    if (best == 0 || best + 2 >= verts.size()) return NAN;  // at an end: likely past it
    const float ry = (verts[best].y + verts[best + 1].y) * 0.5f + snap.at(verts[best].s);
    return hy - ry;
}

// ---------------------------------------------------------------------------------------
// The ground snap, v0.43.5
//
// Sean on v0.43.4 (755, no grid): the line "goes through the ground, or jumps very high", and
// the log's corrections ran to +/-2 m. Two things were wrong. DepthSnap kept its corrections by
// distance ahead of the rider, so the terrain slid through them as he rode and the same spot on
// the track got a different correction every rebuild; and nothing checked that a depth reading
// meant what it was taken to mean (the game may split its depth range between passes, or leave
// only the near pass in the buffer). Now the corrections are kept by place on Coach's line
// (LineSnap, bins along it, fixed in the world), move a few centimetres a reading at most, are
// clamped to +/-0.3 m, and are only taken while SnapCheck finds the depth under the ground just
// ahead of the bike agreeing with the bike's own height.

constexpr float kLineSnapMaxM  = 0.3f;   // the most the snap may move the line, either way
constexpr float kLineSnapBinM  = 2.0f;   // one correction per this much of the line
constexpr float kLineSnapStepM = 0.04f;  // the most one reading moves a bin

class LineSnap {
public:
    void reset(float total_m = 0) {
        total_ = total_m;
        bins_.assign(total_m > 0 ? size_t(std::ceil(total_m / kLineSnapBinM)) + 1 : 0, NAN);
    }
    float total() const { return total_; }
    /// A reading at `m` metres along the line: the drawn surface is `dy` above the line there.
    bool feed(float m, float dy) {
        const long k = bin(m);
        if (k < 0 || !std::isfinite(dy) || std::fabs(dy) > 3.0f) return false;
        float& c = bins_[size_t(k)];
        const float cur = std::isfinite(c) ? c : 0.0f;
        if (std::fabs(dy) > kLineSnapMaxM + 0.6f && std::fabs(dy - cur) > 0.6f) return false;  // not this ground
        const float step = (std::max)(-kLineSnapStepM, (std::min)(kLineSnapStepM, 0.25f * (dy - cur)));
        c                = (std::max)(-kLineSnapMaxM, (std::min)(kLineSnapMaxM, cur + step));
        return true;
    }
    /// The correction at `m`, between bins linearly; 0 where nothing was read.
    float at(float m) const {
        if (bins_.empty() || !std::isfinite(m)) return 0;
        float f = m / kLineSnapBinM;
        if (f < 0) f = 0;
        const size_t a = (std::min)(size_t(f), bins_.size() - 1), b = (std::min)(a + 1, bins_.size() - 1);
        const float  ca = std::isfinite(bins_[a]) ? bins_[a] : 0.0f, cb = std::isfinite(bins_[b]) ? bins_[b] : 0.0f;
        return ca + (cb - ca) * (f - float(a));
    }
    /// The largest correction held, for the log.
    float largest() const {
        float l = 0;
        for (float c : bins_)
            if (std::isfinite(c)) l = (std::max)(l, std::fabs(c));
        return l;
    }

private:
    long bin(float m) const {
        if (bins_.empty() || !std::isfinite(m) || m < 0) return -1;
        const long k = long(std::lround(m / kLineSnapBinM));
        return k < long(bins_.size()) ? k : -1;
    }
    float              total_ = 0;
    std::vector<float> bins_;
};

/// Whether depth readings can be trusted: the ground a few metres ahead of the bike, read back
/// through the same unprojection, has to come out at the bike's own height less its usual height
/// over the ground (learnt from the first readings, between kCheckLiftLo and kCheckLiftHi) to
/// within kCheckTolM, and close to where it was looked for. Seven of the last ten checks must
/// agree, or the snap is off and the line keeps the ground it was built on.
constexpr float kCheckLiftLo = 0.1f, kCheckLiftHi = 1.4f;
constexpr float kCheckTolM   = 0.15f;
constexpr float kCheckNearM  = 2.0f;

class SnapCheck {
public:
    void reset() {
        n_ = 0, hist_ = 0, count_ = 0, lift_ = NAN, warm_.clear();
        why_ = "no check yet";
    }
    /// One check: the bike's y, the read-back hit, and the point the ray was aimed at (x, z).
    void add(float bike_y, float hx, float hy, float hz, float aim_x, float aim_z) {
        bool good = false;
        if (!std::isfinite(hy) || !std::isfinite(hx) || !std::isfinite(hz)) {
            why_ = "the depth read back gave no point";
        } else if (std::hypot(hx - aim_x, hz - aim_z) > kCheckNearM) {
            why_ = "the depth read back lands " + std::to_string(std::hypot(hx - aim_x, hz - aim_z)) +
                   " m from where it was aimed";
        } else {
            const float lift = bike_y - hy;
            if (!std::isfinite(lift_)) {
                warm_.push_back(lift);
                if (warm_.size() >= 5) {
                    std::vector<float> w = warm_;
                    std::nth_element(w.begin(), w.begin() + 2, w.end());
                    if (w[2] >= kCheckLiftLo && w[2] <= kCheckLiftHi) lift_ = w[2];
                    else why_ = "the ground read back is " + std::to_string(w[2]) + " m under the bike";
                    warm_.clear();
                }
            } else if (std::fabs(lift - lift_) <= kCheckTolM) {
                good = true;
            } else {
                why_ = "the ground read back is " + std::to_string(lift) + " m under the bike, usually " +
                       std::to_string(lift_);
            }
        }
        hist_  = ((hist_ << 1) | (good ? 1u : 0u)) & 0x3FFu;
        count_ = (std::min)(10, count_ + 1);
        ++n_;
    }
    bool on() const {
        int g = 0;
        for (int k = 0; k < count_; ++k) g += (hist_ >> k) & 1u;
        return count_ >= 10 && g >= 7;
    }
    const std::string& why() const { return why_; }
    float lift() const { return lift_; }

private:
    int                n_ = 0, count_ = 0;
    unsigned           hist_ = 0;
    float              lift_ = NAN;
    std::vector<float> warm_;
    std::string        why_ = "no check yet";
};

/// One probe's reading against the ribbon, by place on the line: like SnapReading, but returns
/// the row's place `m` too, and compares with the ribbon as drawn (`snap` applied).
inline float SnapReadingAt(const std::vector<Vert>& verts, const LineSnap& snap, float hx, float hy, float hz,
                           float rider_x, float rider_z, float& m) {
    m = NAN;
    if (verts.size() < 4 || !std::isfinite(hx) || !std::isfinite(hy) || !std::isfinite(hz)) return NAN;
    if (std::hypot(hx - rider_x, hz - rider_z) < 2.5f) return NAN;
    size_t best = 0;
    float  bd   = INFINITY;
    for (size_t i = 0; i + 1 < verts.size(); i += 2) {
        const float cx = (verts[i].x + verts[i + 1].x) * 0.5f, cz = (verts[i].z + verts[i + 1].z) * 0.5f;
        const float d2 = (cx - hx) * (cx - hx) + (cz - hz) * (cz - hz);
        if (d2 < bd) bd = d2, best = i;
    }
    if (!(bd <= kSnapLateralM * kSnapLateralM) || best == 0 || best + 2 >= verts.size()) return NAN;
    m              = verts[best].m;
    const float ry = (verts[best].y + verts[best + 1].y) * 0.5f + snap.at(m);
    return hy - ry;
}

/// One row of the ribbon on screen, for the tests and the offline preview: both edges, how far
/// along it is, how opaque (the fade) and its colour. Rows behind the camera are left out.
struct ScreenRow {
    Screen l, r;
    float  s, alpha;
    float  rgba[4];
};
inline std::vector<ScreenRow> ProjectRibbon(const Mat4& p, const Mat4& v, const Axes& fl,
                                            const std::vector<Vert>& vs) {
    std::vector<ScreenRow> out;
    for (size_t i = 0; i + 1 < vs.size(); i += 2) {
        ScreenRow row;
        row.l     = Project(p, v, vs[i].x, vs[i].y, vs[i].z, fl);
        row.r     = Project(p, v, vs[i + 1].x, vs[i + 1].y, vs[i + 1].z, fl);
        row.s     = vs[i].s;
        row.alpha = Fade(vs[i].s);
        for (int c = 0; c < 4; ++c) row.rgba[c] = vs[i].rgba[c];
        if (row.l.ok && row.r.ok) out.push_back(row);
    }
    return out;
}

}  // namespace coachline
