// replaycam.h - keyframed replay camera: the half with no Win32 in it.
//
// A path is a list of keys on a parameter axis - the replay clock, or the target rider's
// lap fraction. Playback evaluates the path at the game's current value and writes the pose
// into the free-roam camera statics; everything that touches the game lives in frostmod.cpp.
// Splitting it this way is what lets the interpolation, the wrap handling, the aiming and
// the file format be tested off Windows.
//
// Three things shape the maths here:
//   * The engine rebuilds its camera matrix from Euler degrees every frame and never
//     slerps, so the angles are interpolated the same way it does - the only extra care is
//     unwrapping across +-180 before interpolating.
//   * Keys are hit at their own times, always. Curve style changes the SHAPE between them
//     (which tangents), never when the camera arrives, so an edit can never silently slide
//     a shot off the moment it was cut for.
//   * Anything that depends on a convention we have not seen on screen (which way the
//     camera's yaw counts, which way pitch counts) is solved at runtime from the game's own
//     view matrix rather than assumed - see Conv/SolveConvention.
#pragma once
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace rcam {

/// The replay stream holds one sample per 30 ms and the game's own frame-step buttons move
/// the clock by exactly that, so it is the quantum keys snap to.
constexpr int kSampleMs = 30;

/// Guard against a runaway path file; 512 keys is ~4 hours at one every 30 s.
constexpr int kMaxKeys = 512;

constexpr char kFileMagic[] = "frostmod-replaycam";
constexpr int  kFileVersion = 2;      // 1 files still load: the added fields have defaults

/// No rider is being aimed at.
constexpr int kNoTarget = -1;

/// Per-key interpolation. It describes the segment that STARTS at the key, which is the
/// convention every animation editor uses and the one that makes `cut` mean what it says.
enum Ease {
    EaseSmooth = 0,   // splined through the key
    EaseHold,         // zero tangent here: the camera eases in, and eases back out
    EaseCut,          // park on this key until the next one, then cut hard to it
    EaseCount
};

/// How the tangents are measured. Both hit every key at its own time.
enum Curve {
    CurveCentripetal = 0,  // knots from sqrt(chord): no loop when two keys sit close together
    CurveUniform,          // knots from the parameter axis - what paths made before v2 flew
    CurveCount
};

/// Camera body. A small deterministic wobble laid over the finished pose.
enum Rig { RigLocked = 0, RigHandheld, RigDrone, RigCrane, RigCount };

/// What the path is keyed against.
enum Anchor {
    AnchorClock = 0,  // replay milliseconds - this exact replay, this exact moment
    AnchorTrack,      // the target rider's lap fraction - any lap, any rider, any replay
    AnchorCount
};

struct Key {
    int   t     = 0;                       // replay clock, ms
    float x     = 0, y = 0, z = 0;         // world metres, Y up
    float yaw   = 0, pitch = 0, roll = 0;  // degrees
    float fov   = 45.0f;                   // degrees
    int   ease  = EaseSmooth;
    int   target = kNoTarget;              // race number to aim at, or kNoTarget
    float aimDist = 0.0f;                  // metres to that rider when the key was set
    float tp    = -1.0f;                   // lap fraction 0..1 when the key was set; <0 unknown
};

struct Path {
    std::vector<Key> keys;
    int   curve     = CurveCentripetal;
    int   rig       = RigLocked;
    float rigAmount = 1.0f;                // 0..2 multiplier on the rig's own amplitudes
    int   anchor    = AnchorClock;
    bool  autoFov   = false;               // hold the target's framing as it comes and goes
};

struct Pose {
    float x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0, roll = 0;
    float fov = 45.0f;
};

/// Where the riders are this frame. Filled from the game's RaceTrackPosition callback, so
/// it is empty in injected mode - aiming then falls back to the keyed angles.
struct Riders {
    struct R { int raceNum = 0; float x = 0, y = 0, z = 0; float tp = -1.0f; };
    R   r[64];
    int n = 0;

    const R* Find(int raceNum) const {
        for (int i = 0; i < n; ++i) if (r[i].raceNum == raceNum) return &r[i];
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// names - the file is meant to be hand-editable, so every enum has words
// ---------------------------------------------------------------------------

inline const char* EaseName(int e) {
    switch (e) { case EaseHold: return "hold"; case EaseCut: return "cut"; default: return "smooth"; }
}
inline const char* CurveName(int c) { return c == CurveUniform ? "uniform" : "centripetal"; }
inline const char* RigName(int r) {
    switch (r) { case RigHandheld: return "handheld"; case RigDrone: return "drone";
                 case RigCrane: return "crane"; default: return "locked"; }
}
inline const char* AnchorName(int a) { return a == AnchorTrack ? "track" : "clock"; }

/// -1 when the word is not one of ours, so a typo in a hand-edited file is refused rather
/// than quietly meaning "smooth".
inline int NameToEnum(const char* word, const char* const* names, int count) {
    for (int i = 0; i < count; ++i) {
        const char *a = word, *b = names[i];
        for (; *a && *b; ++a, ++b) if (std::tolower((unsigned char)*a) != *b) break;
        if (!*a && !*b) return i;
    }
    return -1;
}
inline int EaseFromName(const char* w)   { const char* n[] = {"smooth","hold","cut"};        return NameToEnum(w, n, 3); }
inline int CurveFromName(const char* w)  { const char* n[] = {"centripetal","uniform"};      return NameToEnum(w, n, 2); }
inline int RigFromName(const char* w)    { const char* n[] = {"locked","handheld","drone","crane"}; return NameToEnum(w, n, 4); }
inline int AnchorFromName(const char* w) { const char* n[] = {"clock","track"};              return NameToEnum(w, n, 2); }

// ---------------------------------------------------------------------------
// angles
// ---------------------------------------------------------------------------

constexpr float kDegPerRad = 57.29577951308232f;
constexpr float kRadPerDeg = 0.017453292519943295f;

/// Fold to (-180, +180].
inline float WrapDeg(float d) {
    d = std::fmod(d, 360.0f);
    if (d > 180.0f)  d -= 360.0f;
    if (d <= -180.0f) d += 360.0f;
    return d;
}

/// `a` shifted by whole turns to sit closest to `ref` - so a pan through south interpolates
/// the short way instead of unwinding 359 degrees.
inline float UnwrapNear(float ref, float a) {
    return ref + WrapDeg(a - ref);
}

inline float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------------------
// key list (kept sorted by t, one key per snapped ms)
// ---------------------------------------------------------------------------

inline int SnapMs(int ms) {
    if (ms >= 0) return ((ms + kSampleMs / 2) / kSampleMs) * kSampleMs;
    return -(((-ms + kSampleMs / 2) / kSampleMs) * kSampleMs);
}

/// Index of the key at exactly `t` (already snapped), or -1.
inline int IndexAt(const std::vector<Key>& keys, int t) {
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i].t == t) return (int)i;
    return -1;
}

/// Add `k` at its snapped time, replacing any key already there. Returns its index, or -1
/// if the list is full (a replace always succeeds).
inline int Upsert(std::vector<Key>& keys, Key k) {
    k.t = SnapMs(k.t);
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].t == k.t) { keys[i] = k; return (int)i; }
        if (keys[i].t > k.t) {
            if ((int)keys.size() >= kMaxKeys) return -1;
            keys.insert(keys.begin() + i, k);
            return (int)i;
        }
    }
    if ((int)keys.size() >= kMaxKeys) return -1;
    keys.push_back(k);
    return (int)keys.size() - 1;
}

/// Index of the key nearest `t`, or -1 if there are none.
inline int NearestIndex(const std::vector<Key>& keys, int t) {
    int best = -1, bestD = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
        int d = std::abs(keys[i].t - t);
        if (best < 0 || d < bestD) { best = (int)i; bestD = d; }
    }
    return best;
}

/// Index of the key nearest `t` within `tolMs`, or -1. "The one you can see", which is what
/// every edit that acts on a key without naming it means.
inline int PickIndex(const std::vector<Key>& keys, int t, int tolMs) {
    int i = NearestIndex(keys, t);
    return (i >= 0 && std::abs(keys[i].t - t) <= tolMs) ? i : -1;
}

/// Remove the key nearest `t` if it is within `tolMs`. Returns whether one went.
inline bool EraseNearest(std::vector<Key>& keys, int t, int tolMs) {
    int i = PickIndex(keys, t, tolMs);
    if (i < 0) return false;
    keys.erase(keys.begin() + i);
    return true;
}

/// Move key `i` by `deltaMs`, snapped, refusing to cross its neighbours. Returns its new
/// time, or -1 if it could not move.
inline int NudgeKey(std::vector<Key>& keys, int i, int deltaMs) {
    if (i < 0 || i >= (int)keys.size()) return -1;
    const int want = SnapMs(keys[i].t + deltaMs);
    if (i > 0 && want <= keys[i - 1].t) return -1;
    if (i + 1 < (int)keys.size() && want >= keys[i + 1].t) return -1;
    keys[i].t = want;
    return want;
}

/// Time of the last key strictly before `t`, or -1.
inline int PrevKeyTime(const std::vector<Key>& keys, int t) {
    int best = -1;
    for (const Key& k : keys) if (k.t < t && k.t > best) best = k.t;
    return best;
}

/// Time of the first key strictly after `t`, or -1.
inline int NextKeyTime(const std::vector<Key>& keys, int t) {
    int best = -1;
    for (const Key& k : keys) if (k.t > t && (best < 0 || k.t < best)) best = k.t;
    return best;
}

/// The window the path governs, in replay ms. Outside it the camera is left alone.
inline bool Span(const std::vector<Key>& keys, int& first, int& last) {
    if (keys.size() < 2) return false;
    first = keys.front().t;
    last  = keys.back().t;
    return true;
}

// ---------------------------------------------------------------------------
// shots - a `cut` key ends one and starts the next, so this is just counting
// ---------------------------------------------------------------------------

/// Number of shots in the path: one, plus one per cut that has a key after it.
inline int ShotCount(const std::vector<Key>& keys) {
    if (keys.empty()) return 0;
    int n = 1;
    for (size_t i = 0; i + 1 < keys.size(); ++i) if (keys[i].ease == EaseCut) ++n;
    return n;
}

/// 1-based shot the key at index `i` belongs to, or 0 if there is no such key.
inline int ShotOfKey(const std::vector<Key>& keys, int i) {
    if (i < 0 || i >= (int)keys.size()) return 0;
    int shot = 1;
    for (int j = 0; j < i; ++j) if (keys[j].ease == EaseCut) ++shot;
    return shot;
}

// ---------------------------------------------------------------------------
// the parameter axis
//
// Clock-anchored, it is replay milliseconds and the keys already carry it. Track-anchored,
// it is the target rider's lap fraction, unwrapped so a path that crosses the start/finish
// line keeps climbing instead of falling off 1.0 back to 0.0 - the same trick the angles
// use across +-180, for the same reason.
// ---------------------------------------------------------------------------

inline void Params(const Path& p, std::vector<float>& out) {
    const size_t n = p.keys.size();
    out.resize(n);
    if (!n) return;
    if (p.anchor != AnchorTrack) {
        for (size_t i = 0; i < n; ++i) out[i] = (float)p.keys[i].t;
        return;
    }
    out[0] = p.keys[0].tp;
    for (size_t i = 1; i < n; ++i) {
        float v = p.keys[i].tp;
        for (int lap = 0; lap < 8 && v <= out[i - 1]; ++lap) v += 1.0f;   // one wrap is normal
        out[i] = v;
    }
}

/// Whether the axis came out usable: two keys, strictly increasing, and for a track-anchored
/// path every key actually carrying a lap fraction.
inline bool ParamsValid(const Path& p, const std::vector<float>& u) {
    if (u.size() < 2) return false;
    for (size_t i = 0; i < u.size(); ++i) {
        if (p.anchor == AnchorTrack && p.keys[i].tp < 0.0f) return false;
        if (i && !(u[i] > u[i - 1])) return false;
    }
    return true;
}

/// Put the live moment on the same axis. `tpNow` is the anchor rider's lap fraction, or <0
/// when nobody is reporting one - which is what makes a track-anchored path sit out.
inline bool ParamNow(const Path& p, const std::vector<float>& u, int clockMs, float tpNow,
                     float& out) {
    if (u.size() < 2) return false;
    if (p.anchor != AnchorTrack) { out = (float)clockMs; return true; }
    if (tpNow < 0.0f) return false;
    // The path may sit anywhere on the unwrapped axis; take the lap that lands in it, and
    // failing that the nearest one, so the caller's own span check does the rejecting.
    const float mid = (u.front() + u.back()) * 0.5f;
    const float base = std::floor(mid);
    float best = tpNow + base, bestD = std::fabs(best - mid);
    for (int k = -2; k <= 2; ++k) {
        const float cand = tpNow + base + (float)k;
        if (cand >= u.front() && cand <= u.back()) { out = cand; return true; }
        const float d = std::fabs(cand - mid);
        if (d < bestD) { best = cand; bestD = d; }
    }
    out = best;
    return true;
}

/// Knots the tangents are measured against. Centripetal uses sqrt(chord), which is what
/// stops two keys a hand's width apart from throwing a loop into the path; uniform is the
/// parameter axis itself, which is how paths flew before v2.
inline void Knots(const Path& p, const std::vector<float>& params, std::vector<float>& out) {
    const size_t n = p.keys.size();
    out.resize(n);
    if (!n) return;
    if (p.curve != CurveCentripetal) { out = params; return; }
    out[0] = 0.0f;
    for (size_t i = 1; i < n; ++i) {
        const float dx = p.keys[i].x - p.keys[i - 1].x;
        const float dy = p.keys[i].y - p.keys[i - 1].y;
        const float dz = p.keys[i].z - p.keys[i - 1].z;
        // Floored: a camera that only pans has no chord at all, and zero-width knots would
        // divide the tangents by nothing.
        const float chord = std::sqrt(dx * dx + dy * dy + dz * dz);
        out[i] = out[i - 1] + std::sqrt(chord > 1e-4f ? chord : 1e-4f);
    }
}

// ---------------------------------------------------------------------------
// aiming, and the convention it needs
//
// A key can name a rider instead of an angle, and playback points the camera at wherever
// that rider is this frame. Hand-keying yaw and pitch through a rhythm section is the part
// nobody enjoys; this is the part that removes it.
//
// Pointing at something needs to know how the camera's angle globals count, and that is a
// convention nobody has watched on screen. So it is not assumed: the game hands us its own
// view matrix every frame, the direction the camera really looks falls straight out of it,
// and comparing the two settles sign and offset in a second of replay. Until a solve lands
// we fly the radar's convention, which is at least the same engine's.
// ---------------------------------------------------------------------------

struct Conv {
    float yawSign   = 1.0f;    // yawGeometric = yawSign * yawGlobal + yawOffset
    float yawOffset = 0.0f;
    float pitchSign = 1.0f;    // pitchGeometric = pitchSign * pitchGlobal
    bool  solved    = false;
};

/// Heading of a world direction: 0 = +Z, growing towards +X. The radar flies on this and is
/// right on screen, which is the only reason it is not a guess.
inline float HeadingDeg(float dx, float dz) { return std::atan2(dx, dz) * kDegPerRad; }

inline float ElevationDeg(float dy, float dist) {
    return dist > 1e-4f ? std::asin(ClampF(dy / dist, -1.0f, 1.0f)) * kDegPerRad : 0.0f;
}

/// The angle globals that point a camera at `t` from `c`. `dist` comes back because the
/// framing needs it.
inline void AimAngles(const Conv& conv, float cx, float cy, float cz,
                      float tx, float ty, float tz,
                      float& yaw, float& pitch, float* dist = nullptr) {
    const float dx = tx - cx, dy = ty - cy, dz = tz - cz;
    const float d  = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist) *dist = d;
    yaw   = WrapDeg(conv.yawSign * (HeadingDeg(dx, dz) - conv.yawOffset));
    pitch = WrapDeg(conv.pitchSign * ElevationDeg(dy, d));
}

/// One frame's evidence: the angle globals as they read, and the direction the view matrix
/// says the camera is actually looking.
struct ConvSample { float yaw = 0, pitch = 0; float fx = 0, fy = 0, fz = 0; };

/// Solve sign and offset from samples. Refuses unless the samples actually disagree with
/// each other (a camera held still proves nothing) and one candidate fits all of them - so
/// a wrong convention stays unsolved rather than becoming a confident mirror image.
inline bool SolveConvention(const ConvSample* s, int n, Conv& out, float tolDeg = 2.0f) {
    if (!s || n < 3) return false;

    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < n; ++i) {
        const float h = HeadingDeg(s[i].fx, s[i].fz);
        lo = h < lo ? h : lo; hi = h > hi ? h : hi;
    }
    if (hi - lo < 20.0f) return false;                  // the camera barely turned

    bool  found = false;
    Conv  best;
    float bestErr = 1e9f;
    const float offsets[] = { 0.0f, 90.0f, 180.0f, -90.0f };
    for (int sg = 0; sg < 2; ++sg) {
        const float sign = sg ? -1.0f : 1.0f;
        for (float off : offsets) {
            float worst = 0.0f;
            for (int i = 0; i < n; ++i) {
                const float want = HeadingDeg(s[i].fx, s[i].fz);
                const float got  = WrapDeg(sign * s[i].yaw + off);
                const float e    = std::fabs(WrapDeg(got - want));
                worst = e > worst ? e : worst;
            }
            if (worst < bestErr) { bestErr = worst; best.yawSign = sign; best.yawOffset = off; }
        }
    }
    if (bestErr > tolDeg) return false;
    found = true;

    // Pitch is one bit, and it needs the camera to have looked off the horizon at all.
    float pitchSpread = 0.0f, pWorst[2] = { 0.0f, 0.0f };
    for (int i = 0; i < n; ++i) {
        const float d = std::sqrt(s[i].fx * s[i].fx + s[i].fy * s[i].fy + s[i].fz * s[i].fz);
        const float want = ElevationDeg(s[i].fy, d);
        pitchSpread = std::fabs(want) > pitchSpread ? std::fabs(want) : pitchSpread;
        for (int sg = 0; sg < 2; ++sg) {
            const float e = std::fabs(WrapDeg((sg ? -1.0f : 1.0f) * s[i].pitch - want));
            if (e > pWorst[sg]) pWorst[sg] = e;
        }
    }
    if (pitchSpread < 3.0f) return false;               // level the whole time: no evidence
    if (pWorst[0] <= tolDeg && pWorst[1] > tolDeg)      best.pitchSign =  1.0f;
    else if (pWorst[1] <= tolDeg && pWorst[0] > tolDeg) best.pitchSign = -1.0f;
    else return false;

    best.solved = true;
    out = best;
    return found;
}

/// FOV that keeps the framing a key was set with as the subject comes and goes. Constant
/// apparent size wants d * tan(fov/2) held constant, which is all this is.
inline float FramedFov(float keyFov, float keyDist, float distNow) {
    if (keyDist <= 0.1f || distNow <= 0.1f) return keyFov;
    const float t = std::tan(ClampF(keyFov, 1.0f, 170.0f) * 0.5f * kRadPerDeg) * (keyDist / distNow);
    return ClampF(std::atan(t) * 2.0f * kDegPerRad, 5.0f, 140.0f);
}

// ---------------------------------------------------------------------------
// evaluation
// ---------------------------------------------------------------------------

/// Cubic Hermite on a unit segment.
inline float Hermite01(float p1, float p2, float m1, float m2, float u) {
    const float u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * p1
         + (u3 - 2 * u2 + u)     * m1
         + (-2 * u3 + 3 * u2)    * p2
         + (u3 - u2)             * m2;
}

/// Catmull-Rom tangents for one segment, measured on the knots `s` and expressed per unit of
/// the segment. With s = the parameter axis this is the time-scaled form v1 shipped.
inline void Tangents(float p0, float p1, float p2, float p3,
                     float s0, float s1, float s2, float s3,
                     bool flat1, bool flat2, float& m1, float& m2) {
    const float h = s2 - s1;
    m1 = (!flat1 && (s2 - s0) > 0.0f) ? (p2 - p0) / (s2 - s0) * h : 0.0f;
    m2 = (!flat2 && (s3 - s1) > 0.0f) ? (p3 - p1) / (s3 - s1) * h : 0.0f;
}

inline float SmoothStep01(float u) { return u <= 0 ? 0 : (u >= 1 ? 1 : u * u * (3 - 2 * u)); }

/// Pose of the path at axis value `uNow`. False when the path has fewer than two keys, its
/// axis is unusable, or `uNow` lies outside it - the caller then leaves the camera alone.
/// `riders` may be null; only aiming needs it.
inline bool Evaluate(const Path& p, float uNow, const Riders* riders, const Conv& conv,
                     Pose& out) {
    std::vector<float> par, s;
    Params(p, par);
    if (!ParamsValid(p, par)) return false;
    if (uNow < par.front() || uNow > par.back()) return false;
    Knots(p, par, s);

    const std::vector<Key>& keys = p.keys;
    const int n = (int)keys.size();
    int i = 0;                                   // segment keys[i] .. keys[i+1]
    while (i + 2 < n && par[i + 1] <= uNow) ++i;

    const Key& k1 = keys[i];
    const Key& k2 = keys[i + 1];
    // A cut holds its key: the shot is locked off until the next key's own moment, which
    // is the frame it cuts on - and only the last segment can ever be evaluated there.
    const bool  parked = k1.ease == EaseCut && uNow < par[i + 1];
    const float h = par[i + 1] - par[i];
    const float u = (!parked && h > 0.0f) ? (uNow - par[i]) / h : 0.0f;

    float aimDist = k1.aimDist;
    if (parked) {
        out.x = k1.x; out.y = k1.y; out.z = k1.z;
        out.yaw = k1.yaw; out.pitch = k1.pitch; out.roll = k1.roll; out.fov = k1.fov;
    } else {
        // Tangent neighbours stop at a cut: nothing across a hard cut is continuous with
        // this shot, and reaching over it would bend the path towards a camera nobody saw.
        const int i0 = (i > 0 && keys[i - 1].ease != EaseCut) ? i - 1 : i;
        const int i3 = (i + 2 < n) ? i + 2 : i + 1;
        const Key& k0 = keys[i0];
        const Key& k3 = keys[i3];
        const float s0 = s[i0], s1 = s[i], s2 = s[i + 1], s3 = s[i3];
        const bool flat1 = k1.ease == EaseHold;
        const bool flat2 = k2.ease == EaseHold || k2.ease == EaseCut;   // arrive and settle

        auto lin = [&](float a0, float a1, float a2, float a3) {
            float m1, m2;
            Tangents(a0, a1, a2, a3, s0, s1, s2, s3, flat1, flat2, m1, m2);
            return Hermite01(a1, a2, m1, m2, u);
        };
        // Angles are unwrapped into one continuous run around k1 first, then folded back.
        auto ang = [&](float a0, float a1, float a2, float a3) {
            const float b1 = a1;
            const float b0 = UnwrapNear(b1, a0);
            const float b2 = UnwrapNear(b1, a2);
            const float b3 = UnwrapNear(b2, a3);
            return WrapDeg(lin(b0, b1, b2, b3));
        };

        out.x     = lin(k0.x, k1.x, k2.x, k3.x);
        out.y     = lin(k0.y, k1.y, k2.y, k3.y);
        out.z     = lin(k0.z, k1.z, k2.z, k3.z);
        out.fov   = lin(k0.fov, k1.fov, k2.fov, k3.fov);
        out.yaw   = ang(k0.yaw,   k1.yaw,   k2.yaw,   k3.yaw);
        out.pitch = ang(k0.pitch, k1.pitch, k2.pitch, k3.pitch);
        out.roll  = ang(k0.roll,  k1.roll,  k2.roll,  k3.roll);
        if (k2.aimDist > 0.0f && k1.aimDist > 0.0f)
            aimDist = k1.aimDist + (k2.aimDist - k1.aimDist) * SmoothStep01(u);
        else if (k1.aimDist <= 0.0f) aimDist = k2.aimDist;
    }

    // ---- aiming ----
    // A key with no target of its own inherits the segment's, so you mark where the aim
    // starts and where it ends and leave the keys between alone. Two different targets on
    // one segment is a look-off: the aim point eases from one rider to the other.
    if (!riders) return true;
    const Riders::R* a1 = k1.target != kNoTarget ? riders->Find(k1.target) : nullptr;
    const Riders::R* a2 = parked ? a1 : (k2.target != kNoTarget ? riders->Find(k2.target) : nullptr);
    if (!a1 && !a2) return true;

    float ax, ay, az;
    if (a1 && a2 && a1 != a2) {
        const float w = SmoothStep01(u);
        ax = a1->x + (a2->x - a1->x) * w;
        ay = a1->y + (a2->y - a1->y) * w;
        az = a1->z + (a2->z - a1->z) * w;
    } else {
        const Riders::R* a = a1 ? a1 : a2;
        ax = a->x; ay = a->y; az = a->z;
    }

    float dist = 0.0f;
    AimAngles(conv, out.x, out.y, out.z, ax, ay, az, out.yaw, out.pitch, &dist);
    if (p.autoFov && aimDist > 0.0f) out.fov = FramedFov(out.fov, aimDist, dist);
    return true;
}

/// No riders, no aiming - the shape of the path on its own.
inline bool Evaluate(const Path& p, float uNow, Pose& out) {
    static const Conv kIdentity;
    return Evaluate(p, uNow, nullptr, kIdentity, out);
}

// ---------------------------------------------------------------------------
// the rig
//
// A path drawn by hand is perfectly still between its keys, which reads as a camera bolted
// to a rail - fine for a crane shot, wrong for anything meant to look operated. The rig lays
// a small wobble over the finished pose. It is value noise off the replay clock, not a
// random number generator, so the same replay shakes the same way every time you record it.
// ---------------------------------------------------------------------------

inline uint32_t Hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

/// Deterministic -1..1 for a channel and an integer cell.
inline float Rand11(uint32_t channel, int cell) {
    const uint32_t h = Hash32((uint32_t)cell * 2654435761U ^ (channel * 2246822519U));
    return (float)h * (2.0f / 4294967296.0f) - 1.0f;
}

/// Smooth value noise, mean ~0, never outside -1..1.
inline float Noise(uint32_t channel, float x) {
    const float f = std::floor(x);
    const float u = x - f;
    const float w = u * u * (3.0f - 2.0f * u);
    return Rand11(channel, (int)f) * (1.0f - w) + Rand11(channel, (int)f + 1) * w;
}

struct RigProfile {
    float posXZ, posY, freqPos;    // metres, Hz
    float pan, roll, freqAng;      // degrees, Hz
};

inline RigProfile RigProfileOf(int rig) {
    switch (rig) {
        case RigHandheld: return { 0.020f, 0.030f, 2.2f,  0.20f, 0.12f, 2.8f };
        case RigDrone:    return { 0.100f, 0.140f, 0.32f, 0.22f, 0.35f, 0.28f };
        case RigCrane:    return { 0.030f, 0.060f, 0.14f, 0.05f, 0.03f, 0.12f };
        default:          return { 0.0f,   0.0f,   1.0f,  0.0f,  0.0f,  1.0f };
    }
}

/// Lay the rig's wobble over `p`. `seconds` is replay time, so a re-record is identical.
inline void ApplyRig(int rig, float amount, float seconds, Pose& p) {
    if (rig == RigLocked || amount <= 0.0f) return;
    const RigProfile r = RigProfileOf(rig);
    const float a  = ClampF(amount, 0.0f, 4.0f);
    const float tp = seconds * r.freqPos, ta = seconds * r.freqAng;
    p.x     += Noise(1, tp)          * r.posXZ * a;
    p.y     += Noise(2, tp * 1.13f)  * r.posY  * a;
    p.z     += Noise(3, tp * 0.91f)  * r.posXZ * a;
    p.yaw    = WrapDeg(p.yaw   + Noise(4, ta)         * r.pan  * a);
    p.pitch  = WrapDeg(p.pitch + Noise(5, ta * 1.17f) * r.pan  * a);
    p.roll   = WrapDeg(p.roll  + Noise(6, ta * 0.83f) * r.roll * a);
}

// ---------------------------------------------------------------------------
// retiming
// ---------------------------------------------------------------------------

/// Respace the keys so the camera covers ground at a steady rate: the time between two keys
/// becomes proportional to the distance between them, inside the same first and last time.
/// A cut's shot is a duration someone chose, not a distance, so it keeps the length it had.
/// Returns false if there is nothing to spread (no keys, or no movement at all).
inline bool RetimeByDistance(std::vector<Key>& keys) {
    const int n = (int)keys.size();
    if (n < 3) return false;

    const int first = keys.front().t, last = keys.back().t;
    if (last - first < kSampleMs * (n - 1)) return false;     // no room to move anything

    std::vector<float> chord((size_t)n - 1, 0.0f);
    float moving = 0.0f;
    int   held   = 0;
    for (int i = 0; i + 1 < n; ++i) {
        if (keys[i].ease == EaseCut) { held += keys[i + 1].t - keys[i].t; continue; }
        const float dx = keys[i+1].x - keys[i].x, dy = keys[i+1].y - keys[i].y,
                    dz = keys[i+1].z - keys[i].z;
        chord[i] = std::sqrt(dx*dx + dy*dy + dz*dz);
        moving += chord[i];
    }
    if (moving <= 1e-3f) return false;

    int budget = (last - first) - held;
    if (budget < kSampleMs) return false;

    // Every moving segment keeps at least one sample, so a near-still one cannot collapse
    // onto its neighbour and lose the key between them.
    int movingSegs = 0;
    for (int i = 0; i + 1 < n; ++i) if (keys[i].ease != EaseCut) ++movingSegs;
    if (budget < movingSegs * kSampleMs) return false;

    std::vector<int> dur((size_t)n - 1, 0);
    int total = 0, widest = -1;
    for (int i = 0; i + 1 < n; ++i) {
        if (keys[i].ease == EaseCut) {
            dur[i] = keys[i + 1].t - keys[i].t;                    // the shot keeps its length
        } else {
            dur[i] = SnapMs((int)((float)budget * (chord[i] / moving)));
            if (dur[i] < kSampleMs) dur[i] = kSampleMs;
            if (widest < 0 || dur[i] > dur[widest]) widest = i;
        }
        total += dur[i];
    }
    // Snapping leaves the tail a sample or two off the end it started on. The longest
    // moving segment absorbs it, so the path still finishes exactly when it used to.
    const int slack = (last - first) - total;
    if (widest >= 0 && dur[widest] + slack >= kSampleMs) dur[widest] += slack;

    int t = first;
    for (int i = 0; i + 1 < n; ++i) { keys[i].t = t; t += dur[i]; }
    keys[n - 1].t = t;
    return true;
}

// ---------------------------------------------------------------------------
// file format (plain text, one key per line - hand-editable on purpose)
//
// Version 2 adds a `path` line for the whole-path settings and four columns to each key.
// Version 1 files still load: the columns it never had take their defaults, which is the
// v1 behaviour exactly. Written files are always v2.
// ---------------------------------------------------------------------------

inline std::string Serialize(const Path& p) {
    std::string s;
    char line[256];
    std::snprintf(line, sizeof(line), "%s %d\n", kFileMagic, kFileVersion);
    s += line;
    std::snprintf(line, sizeof(line), "path %s %s %.3f %s %d\n",
                  CurveName(p.curve), RigName(p.rig), p.rigAmount, AnchorName(p.anchor),
                  p.autoFov ? 1 : 0);
    s += line;
    s += "# t_ms x y z yaw pitch roll fov ease target aimdist trackpos\n";
    for (const Key& k : p.keys) {
        std::snprintf(line, sizeof(line),
                      "%d %.4f %.4f %.4f %.4f %.4f %.4f %.4f %s %d %.3f %.5f\n",
                      k.t, k.x, k.y, k.z, k.yaw, k.pitch, k.roll, k.fov,
                      EaseName(k.ease), k.target, k.aimDist, k.tp);
        s += line;
    }
    return s;
}

/// Split a line on whitespace. Returns how many of `out` were filled.
inline int Tokens(const std::string& line, char out[16][64]) {
    int n = 0;
    size_t i = 0;
    while (i < line.size() && n < 16) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;
        size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
        const size_t len = j - i < 63 ? j - i : 63;
        std::memcpy(out[n], line.data() + i, len);
        out[n][len] = '\0';
        ++n;
        i = j;
    }
    return n;
}

/// Parse a path file. A line we can read but do not understand is an error rather than a
/// silent drop: a path that quietly loses half its keys is worse than one that refuses.
inline bool Parse(const std::string& text, Path& out, std::string* err = nullptr) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    // Built aside and handed over only once the whole file has been read, so a file that
    // fails halfway leaves the caller with the path it had rather than half of a new one.
    Path p;
    out = Path{};

    size_t pos = 0;
    bool haveHeader = false;
    int  version = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        line = line.substr(b);
        if (line[0] == '#') continue;

        char tok[16][64];
        const int nt = Tokens(line, tok);
        if (!nt) continue;

        if (!haveHeader) {
            if (nt < 2 || std::strcmp(tok[0], kFileMagic) != 0)
                return fail("not a FrostMod replay camera path");
            version = std::atoi(tok[1]);
            if (version < 1 || version > kFileVersion) return fail("unsupported path file version");
            haveHeader = true;
            continue;
        }

        if (std::strcmp(tok[0], "path") == 0) {          // whole-path settings (v2)
            if (nt < 6) return fail("malformed path line");
            const int c = CurveFromName(tok[1]), r = RigFromName(tok[2]), a = AnchorFromName(tok[4]);
            if (c < 0) return fail("unknown curve");
            if (r < 0) return fail("unknown rig");
            if (a < 0) return fail("unknown anchor");
            p.curve = c; p.rig = r; p.anchor = a;
            p.rigAmount = ClampF((float)std::atof(tok[3]), 0.0f, 4.0f);
            p.autoFov = std::atoi(tok[5]) != 0;
            continue;
        }

        if (nt < 8) return fail("malformed key line");
        Key k;
        k.t     = std::atoi(tok[0]);
        k.x     = (float)std::atof(tok[1]);
        k.y     = (float)std::atof(tok[2]);
        k.z     = (float)std::atof(tok[3]);
        k.yaw   = (float)std::atof(tok[4]);
        k.pitch = (float)std::atof(tok[5]);
        k.roll  = (float)std::atof(tok[6]);
        k.fov   = (float)std::atof(tok[7]);
        if (nt >= 9) {
            const int e = EaseFromName(tok[8]);
            if (e < 0) return fail("unknown ease");
            k.ease = e;
        }
        if (nt >= 10) k.target  = std::atoi(tok[9]);
        if (nt >= 11) k.aimDist = (float)std::atof(tok[10]);
        if (nt >= 12) k.tp      = (float)std::atof(tok[11]);
        if ((int)p.keys.size() >= kMaxKeys) return fail("too many keys");
        k.t = SnapMs(k.t);
        k.yaw = WrapDeg(k.yaw); k.pitch = WrapDeg(k.pitch); k.roll = WrapDeg(k.roll);
        p.keys.push_back(k);
    }

    if (!haveHeader) return fail("empty path file");
    std::stable_sort(p.keys.begin(), p.keys.end(),
                     [](const Key& a, const Key& b) { return a.t < b.t; });
    p.keys.erase(std::unique(p.keys.begin(), p.keys.end(),
                             [](const Key& a, const Key& b) { return a.t == b.t; }),
                 p.keys.end());
    if (p.anchor == AnchorTrack) {
        for (const Key& k : p.keys)
            if (k.tp < 0.0f) return fail("track-anchored path has a key with no lap fraction");
    }
    out = p;
    return true;
}

} // namespace rcam
