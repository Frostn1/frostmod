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
        if (best.ok && d >= best.dist) continue;
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
    float heat;     // 0 on the throttle .. 0.5 lifting or light braking .. 1 hard braking
};

constexpr float kStep         = 2.0f;   // metres between ribbon rows, on centreline heights
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
/// it slowed (Heat below): a sheet carries three or four cues, a lap has a corner every few
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
// Where Coach's lap slowed down

constexpr float kDecelLight = 1.5f;  // m/s^2: below this it is still on the gas, or near enough
constexpr float kDecelHard  = 8.0f;  // m/s^2: and from here it is hard on the brakes
constexpr float kHeatWindowM = 4.0f; // metres either side the speed and its change are taken over
constexpr float kZoneHeat    = 0.5f; // a braking zone is where the heat reaches this
constexpr float kZoneMinM    = 4.0f; // and stays there this long
constexpr float kZoneMergeM  = 12.0f;// two zones this close are one corner

/// How hard Coach's lap was slowing at each of its points, 0..1. From the sheet's "DRIV" chunk
/// when it has one (speed and the brake itself), otherwise from the lap's own points: speed is
/// distance over time between points kHeatWindowM either side, deceleration the change in that
/// speed over the time between. Either way smoothed over kHeatWindowM, so the colour runs along
/// the ribbon instead of flickering from point to point. Empty when the lap can't say (fewer than
/// a handful of points, or time that doesn't advance).
inline std::vector<float> Heat(const std::vector<coachhud::RefPoint>& ref, const std::vector<float>& drive = {}) {
    const size_t n = ref.size();
    std::vector<float> out;
    if (n < 8) return out;
    // Distance along the lap at each point.
    std::vector<float> s(n, 0.0f);
    for (size_t i = 1; i < n; ++i) {
        const float d = std::hypot(ref[i].x - ref[i - 1].x, ref[i].z - ref[i - 1].z);
        s[i]          = s[i - 1] + (d < kMaxGapM ? d : 0.0f);
    }
    if (!(s.back() > 50.0f)) return out;
    auto span = [&](size_t i, size_t& lo, size_t& hi) {
        lo = i, hi = i;
        while (lo > 0 && s[i] - s[lo] < kHeatWindowM) --lo;
        while (hi + 1 < n && s[hi] - s[i] < kHeatWindowM) ++hi;
    };
    const bool have_drive = drive.size() == n * 3;
    std::vector<float> v(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        if (have_drive) {
            v[i] = drive[i * 3];
            continue;
        }
        size_t lo, hi;
        span(i, lo, hi);
        const float dt = ref[hi].t - ref[lo].t;
        v[i]           = dt > 1e-3f ? (s[hi] - s[lo]) / dt : -1.0f;
    }
    std::vector<float> raw(n, 0.0f);
    size_t             known = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t lo, hi;
        span(i, lo, hi);
        const float dt = ref[hi].t - ref[lo].t;
        if (!(dt > 1e-3f) || v[lo] < 0 || v[hi] < 0) continue;
        ++known;
        const float decel = (v[lo] - v[hi]) / dt;
        float       h     = (decel - kDecelLight) / (kDecelHard - kDecelLight) * 0.75f + (decel > kDecelLight ? 0.25f : 0.0f);
        if (have_drive) h = (std::max)(h, drive[i * 3 + 2]);  // the brake itself, when Coach sent it
        raw[i] = (std::max)(0.0f, (std::min)(1.0f, h));
    }
    if (known < n / 2) return out;
    out.assign(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        size_t lo, hi;
        span(i, lo, hi);
        float sum = 0;
        for (size_t k = lo; k <= hi; ++k) sum += raw[k];
        out[i] = sum / float(hi - lo + 1);
    }
    return out;
}

/// The braking zones in a heat profile, as point index ranges [from, to): where the heat holds
/// kZoneHeat for kZoneMinM, zones closer than kZoneMergeM joined into one corner.
inline std::vector<std::pair<size_t, size_t>> HeatZones(const std::vector<coachhud::RefPoint>& ref,
                                                        const std::vector<float>& heat) {
    std::vector<std::pair<size_t, size_t>> out;
    if (heat.size() != ref.size()) return out;
    auto dist = [&](size_t a, size_t b) {
        float d = 0;
        for (size_t i = a; i + 1 <= b && i + 1 < ref.size(); ++i) {
            const float s = std::hypot(ref[i + 1].x - ref[i].x, ref[i + 1].z - ref[i].z);
            d += s < kMaxGapM ? s : 0.0f;
        }
        return d;
    };
    for (size_t i = 0; i < heat.size();) {
        if (heat[i] < kZoneHeat) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < heat.size() && heat[j] >= kZoneHeat) ++j;
        if (!out.empty() && dist(out.back().second, i) < kZoneMergeM) out.back().second = j;
        else out.push_back({i, j});
        i = j;
    }
    std::vector<std::pair<size_t, size_t>> kept;
    for (const auto& z : out)
        if (dist(z.first, z.second) >= kZoneMinM) kept.push_back(z);
    return kept;
}

/// The ribbon's colour: blue on the throttle, through yellow, to red under hard braking.
inline void HeatColour(float h, float& r, float& g, float& b) {
    h = (std::max)(0.0f, (std::min)(1.0f, h));
    if (h < 0.5f) {
        const float k = h / 0.5f;  // blue (0.10, 0.50, 1.00) to yellow (1.00, 0.85, 0.10)
        r = 0.10f + 0.90f * k, g = 0.50f + 0.35f * k, b = 1.00f - 0.90f * k;
    } else {
        const float k = (h - 0.5f) / 0.5f;  // yellow to red (1.00, 0.20, 0.15)
        r = 1.0f, g = 0.85f - 0.65f * k, b = 0.10f + 0.05f * k;
    }
}

/// What the sheet adds to the ribbon beyond its points: the ground across the line ("TRRN") and
/// how hard the lap was braking at each point. Built once per sheet; `version` changes with it.
struct RibbonExtras {
    uint32_t           version = 0;
    uint32_t           terrain_k = 0;
    float              terrain_step = 0;
    std::vector<float> terrain;  // per point x K, NaN off the grid
    std::vector<float> heat;     // per point, empty when unknown
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

/// The ribbon cache. Rebuilt when the rider has moved kRebuildM along the lap, or the height
/// offset, the zones or the sheet changed, not per frame; the per-frame cost is one pass.
class Ribbon {
public:
    /// `ref`: Coach's points (lap position and world x/z). `track`: centreline heights.
    /// `pos`: the rider's lap fraction. `offset`: HeightBias::offset(). `zones`: cue braking
    /// zones, used only when `ex` has no heat. `ex`: the sheet's ground and heat, or null.
    /// Returns true when verts() changed.
    bool update(const std::vector<coachhud::RefPoint>& ref, const coachhud::Track& track, float pos, float offset,
                const std::vector<Zone>& zones = {}, const RibbonExtras* ex = nullptr) {
        if (ref.size() < 2 || !track.ready() || track.length() <= 0 || !std::isfinite(pos) || pos < 0 || pos > 1 ||
            !std::isfinite(offset)) {
            const bool had = !verts_.empty();
            clear();
            return had;
        }
        const float    lapM = track.length();
        const uint32_t ver  = ex ? ex->version : 0;
        if (built_for_ >= 0 && ref_n_ == ref.size() && zones_n_ == zones.size() && ver_ == ver &&
            std::fabs(offset - offset_) < 0.1f) {
            float d = std::fabs(pos - built_for_) * lapM;
            d       = (std::min)(d, lapM - d);
            if (d < kRebuildM) return false;
        }
        built_for_ = pos;
        ref_n_     = ref.size();
        zones_n_   = zones.size();
        ver_       = ver;
        offset_    = offset;
        verts_.clear();
        const bool  terrain = ex && ex->terrain_k && ex->terrain.size() == ref.size() * ex->terrain_k;
        const bool  heat    = ex && ex->heat.size() == ref.size();
        const float step    = terrain ? kStepTerrain : kStep;
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
                    track.height_at_lap(lap, h);
                    const float cx = a.x + fc * dx, cz = a.z + fc * dz;
                    float       yl = h + offset + kLift, yr = yl;
                    // On the track's own ground, each edge where the ground is under it: the
                    // ribbon takes the bumps along and the camber across.
                    if (terrain) {
                        float la, lb, ra, rb;
                        if (TerrainAt(*ex, i, kHalfWidth, la) && TerrainAt(*ex, ib, kHalfWidth, lb) &&
                            TerrainAt(*ex, i, -kHalfWidth, ra) && TerrainAt(*ex, ib, -kHalfWidth, rb)) {
                            yl = la + (lb - la) * fc + kLiftTerrain;
                            yr = ra + (rb - ra) * fc + kLiftTerrain;
                        }
                    }
                    const float nx = -tz, nz = tx;  // the left perpendicular in x/z (y up)
                    float       ht = 0;
                    if (heat) ht = ex->heat[i] + (ex->heat[ib] - ex->heat[i]) * fc;
                    else ht = InZone(zones, lap * lapM, lapM) ? 1.0f : 0.0f;
                    verts_.push_back({cx + nx * kHalfWidth, yl, cz + nz * kHalfWidth, nextRow, ht});
                    verts_.push_back({cx - nx * kHalfWidth, yr, cz - nz * kHalfWidth, nextRow, ht});
                    nextRow += step;
                }
            }
            travelled += len;
            i = ib;
        }
        return true;
    }
    /// Triangle-strip vertices, two per row (left, then right).
    const std::vector<Vert>& verts() const { return verts_; }
    void clear() {
        verts_.clear();
        built_for_ = -1.0f;
        ref_n_ = zones_n_ = 0;
        ver_ = 0;
    }

private:
    std::vector<Vert> verts_;
    float             built_for_ = -1.0f, offset_ = 0;
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

/// One row of the ribbon on screen, for the tests and the offline preview: both edges, how far
/// along it is, how opaque and how hot. Rows behind the camera are left out.
struct ScreenRow {
    Screen l, r;
    float  s, alpha, heat;
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
        row.heat  = vs[i].heat;
        if (row.l.ok && row.r.ok) out.push_back(row);
    }
    return out;
}

}  // namespace coachline
