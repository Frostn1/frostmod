// replaycam.h - keyframed replay camera: the half with no Win32 in it.
//
// A path is a list of keys on the replay clock (ms). Playback evaluates the path at the
// game's current clock and writes the pose into the free-roam camera statics; everything
// that touches the game lives in frostmod.cpp. Splitting it this way is what lets the
// interpolation, the wrap handling and the file format be tested off Windows.
//
// Interpolation is Catmull-Rom in Hermite form with time-scaled tangents, so keys may be
// unevenly spaced. The engine rebuilds its camera matrix from Euler degrees every frame and
// never slerps, so interpolating the angles the same way matches what it does - the only
// extra care is unwrapping across +-180 before interpolating.
#pragma once
#include <cmath>
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
constexpr int  kFileVersion = 1;

struct Key {
    int   t     = 0;                       // replay clock, ms
    float x     = 0, y = 0, z = 0;         // world metres, Y up
    float yaw   = 0, pitch = 0, roll = 0;  // degrees
    float fov   = 45.0f;                   // degrees
};

struct Pose {
    float x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0, roll = 0;
    float fov = 45.0f;
};

// ---------------------------------------------------------------------------
// angles
// ---------------------------------------------------------------------------

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

/// Remove the key nearest `t` if it is within `tolMs`. Returns whether one went.
inline bool EraseNearest(std::vector<Key>& keys, int t, int tolMs) {
    int i = NearestIndex(keys, t);
    if (i < 0 || std::abs(keys[i].t - t) > tolMs) return false;
    keys.erase(keys.begin() + i);
    return true;
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

/// The window the path governs. Outside it the camera is left alone.
inline bool Span(const std::vector<Key>& keys, int& first, int& last) {
    if (keys.size() < 2) return false;
    first = keys.front().t;
    last  = keys.back().t;
    return true;
}

// ---------------------------------------------------------------------------
// evaluation
// ---------------------------------------------------------------------------

/// Cubic Hermite on [t1,t2] with Catmull-Rom tangents scaled for uneven key spacing.
inline float Hermite(float p0, float p1, float p2, float p3,
                     float t0, float t1, float t2, float t3, float t) {
    const float h = t2 - t1;
    if (h <= 0.0f) return p1;
    const float u = (t - t1) / h;
    const float m1 = (t2 - t0) > 0.0f ? (p2 - p0) / (t2 - t0) * h : 0.0f;
    const float m2 = (t3 - t1) > 0.0f ? (p3 - p1) / (t3 - t1) * h : 0.0f;
    const float u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * p1
         + (u3 - 2 * u2 + u)     * m1
         + (-2 * u3 + 3 * u2)    * p2
         + (u3 - u2)             * m2;
}

/// Pose of the path at replay time `t`. False when the path has fewer than two keys or `t`
/// lies outside it - the caller then leaves the game's camera untouched.
inline bool Evaluate(const std::vector<Key>& keys, int t, Pose& out) {
    int first = 0, last = 0;
    if (!Span(keys, first, last)) return false;
    if (t < first || t > last) return false;

    const int n = (int)keys.size();
    int i = 0;                                  // segment keys[i] .. keys[i+1]
    while (i + 2 < n && keys[i + 1].t <= t) ++i;

    const Key& k1 = keys[i];
    const Key& k2 = keys[i + 1];
    const Key& k0 = keys[i > 0 ? i - 1 : i];            // ends duplicate, giving a linear tangent
    const Key& k3 = keys[i + 2 < n ? i + 2 : i + 1];

    const float t0 = (float)k0.t, t1 = (float)k1.t, t2 = (float)k2.t, t3 = (float)k3.t;
    const float tf = (float)t;

    auto lin = [&](float a0, float a1, float a2, float a3) {
        return Hermite(a0, a1, a2, a3, t0, t1, t2, t3, tf);
    };
    // Angles are unwrapped into one continuous run around k1 first, then folded back.
    auto ang = [&](float a0, float a1, float a2, float a3) {
        const float u1 = a1;
        const float u0 = UnwrapNear(u1, a0);
        const float u2 = UnwrapNear(u1, a2);
        const float u3 = UnwrapNear(u2, a3);
        return WrapDeg(Hermite(u0, u1, u2, u3, t0, t1, t2, t3, tf));
    };

    out.x     = lin(k0.x, k1.x, k2.x, k3.x);
    out.y     = lin(k0.y, k1.y, k2.y, k3.y);
    out.z     = lin(k0.z, k1.z, k2.z, k3.z);
    out.fov   = lin(k0.fov, k1.fov, k2.fov, k3.fov);
    out.yaw   = ang(k0.yaw,   k1.yaw,   k2.yaw,   k3.yaw);
    out.pitch = ang(k0.pitch, k1.pitch, k2.pitch, k3.pitch);
    out.roll  = ang(k0.roll,  k1.roll,  k2.roll,  k3.roll);
    return true;
}

// ---------------------------------------------------------------------------
// file format (plain text, one key per line - hand-editable on purpose)
// ---------------------------------------------------------------------------

inline std::string Serialize(const std::vector<Key>& keys) {
    std::string s;
    char line[256];
    std::snprintf(line, sizeof(line), "%s %d\n", kFileMagic, kFileVersion);
    s += line;
    s += "# t_ms x y z yaw pitch roll fov\n";
    for (const Key& k : keys) {
        std::snprintf(line, sizeof(line), "%d %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n",
                      k.t, k.x, k.y, k.z, k.yaw, k.pitch, k.roll, k.fov);
        s += line;
    }
    return s;
}

/// Parse a path file. Unknown-but-parseable lines are an error rather than a silent drop:
/// a path that quietly loses half its keys is worse than one that refuses to load.
inline bool Parse(const std::string& text, std::vector<Key>& out, std::string* err = nullptr) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    out.clear();

    size_t pos = 0;
    bool haveHeader = false;
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

        if (!haveHeader) {
            char magic[64] = {0};
            int  ver = 0;
            if (std::sscanf(line.c_str(), "%63s %d", magic, &ver) != 2 ||
                std::strcmp(magic, kFileMagic) != 0)
                return fail("not a FrostMod replay camera path");
            if (ver != kFileVersion) return fail("unsupported path file version");
            haveHeader = true;
            continue;
        }

        Key k;
        if (std::sscanf(line.c_str(), "%d %f %f %f %f %f %f %f",
                        &k.t, &k.x, &k.y, &k.z, &k.yaw, &k.pitch, &k.roll, &k.fov) != 8)
            return fail("malformed key line");
        if ((int)out.size() >= kMaxKeys) return fail("too many keys");
        k.t = SnapMs(k.t);
        k.yaw = WrapDeg(k.yaw); k.pitch = WrapDeg(k.pitch); k.roll = WrapDeg(k.roll);
        out.push_back(k);
    }

    if (!haveHeader) return fail("empty path file");
    std::stable_sort(out.begin(), out.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const Key& a, const Key& b) { return a.t == b.t; }),
              out.end());
    return true;
}

} // namespace rcam
