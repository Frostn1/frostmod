// riderbones.h - the rider's real pose, for MXB Coach's recorder.
//
// lean.h and stance.h record what the rider ASKS for. This records what the game DREW: the
// world matrix of every node in the local rider's animated skeleton, as the engine computes it
// for the frame, put into the bike's own frame. Where the nodes live and how they were found
// is in offsets.h ("the rider's animated skeleton") and docs/RIDER_BONES.md.
//
// Read-only, and only on the build the layout was read from: the exe's TimeDateStamp and every
// code site in mxb::kRiderBoneSigs must match, or the stint's RIDER_SKELETON says "unknown
// build" and no RIDER_BONES is written. A read that faults, a matrix that isn't finite, or a
// bike frame that isn't a rotation is "not found" for that frame, never a guess.
//
// No Win32 here, so tests/riderbones_test.cpp runs anywhere: everything that touches the game
// goes through a `Mem` the caller supplies, `bool read(uintptr_t addr, void* out, size_t n)`.
// mxbcoach.cpp gives it an SEH-guarded memcpy and calls Capture from its Draw callback.
//
// Records in the .mxbc file (tags in coachrec.h; little-endian):
//   RIDER_SKELETON  once per stint, before the first RIDER_BONES; again if the skeleton changes
//      0  u8   layout version (1)
//      1  u8   status: 0 ok, 1 unknown game build (nothing read), 2 not found (no skeleton yet)
//      2  u8   node count N (0 unless ok)
//      3  u8   recorded count M: the nodes each RIDER_BONES carries
//      4  u32  the exe's TimeDateStamp (0x6A21833D = beta21e)
//      8  f32[3] the bike body's offset from the physics chassis, game axes, m (see below)
//      20 N x { u8 parent node (255 = a root), u8 1 if recorded else 0, u8 name length L,
//               char name[L] }  - the node's index is its place in this list
//   RIDER_BONES     24 + 15*M bytes, with every telemetry sample while the skeleton is known
//      0  f32  track time s        (as in the SAMPLE it came with)
//      4  f32  lap position 0..1
//      8  u8   layout version (1)
//      9  u8   M
//      10 u8   flags: bit 0 repeat - the game drew no new frame since the last RIDER_BONES
//      11 u8   zero
//      12 f32[3] origin: the rider body object's position in the bike frame, m
//      24 M x { u8 node index; i16 qx, qy, qz, qw (/32767); i16 x, y, z (mm from origin) }
//
// The bike frame is the bike body object as drawn that frame: game axes (x right, y up,
// z forward), origin at the chassis moved by the RIDER_SKELETON offset. A bone's pose in the
// chassis frame is then  T(offset) * T(origin + p) * R(q).  The quaternion is the node's
// rotation in that frame, not relative to its parent; parents are in RIDER_SKELETON for a
// reader that wants locals.
//
// What is recorded: every "riderRIG_*" node except the finger bones (Thumb, Index, Middle,
// Ring, Pink) - 40 of the stock rider's 71 nodes, Head among them. A rider model with none of those
// names records every node instead. 624 bytes a sample on the stock rider, ~31 KB/s at 50 Hz.
#pragma once

#include "offsets.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace riderbones {

constexpr uint8_t kSkeletonLayout = 1;
constexpr uint8_t kBonesLayout    = 1;
constexpr size_t  kSkeletonHead   = 20;
constexpr size_t  kBonesHead      = 24;
constexpr size_t  kBoneSize       = 15;
constexpr int     kMaxNodes       = 255;
constexpr uint8_t kNoParent       = 255;
constexpr uint8_t kFlagRepeat     = 1;

enum Status : uint8_t { OK = 0, UNKNOWN_BUILD = 1, NOT_FOUND = 2 };

// ---------------------------------------------------------------------------------------
// Matrices: the engine's own layout. 4x4 floats, row-major, column vectors, so a point goes
// p' = M p and the translation is m[3], m[7], m[11]; parent-to-child is world = P * L.

struct Mat {
    float m[16];
};

inline Mat Identity() {
    Mat r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    return r;
}

inline Mat Mul(const Mat& a, const Mat& b) {
    Mat r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}

inline bool Finite(const Mat& a) {
    for (float v : a.m)
        if (!std::isfinite(v)) return false;
    return true;
}

/// A rotation and a translation, nothing else: unit columns, at right angles, det +1, and a
/// bottom row of 0 0 0 1. The bike frame has to be one of these to be inverted by transpose.
inline bool IsRigid(const Mat& a, float tol = 0.02f) {
    if (!Finite(a)) return false;
    if (std::fabs(a.m[12]) > tol || std::fabs(a.m[13]) > tol || std::fabs(a.m[14]) > tol ||
        std::fabs(a.m[15] - 1) > tol)
        return false;
    for (int c = 0; c < 3; ++c)
        for (int d = c; d < 3; ++d) {
            float s = 0;
            for (int r = 0; r < 3; ++r) s += a.m[r * 4 + c] * a.m[r * 4 + d];
            if (std::fabs(s - (c == d ? 1.0f : 0.0f)) > tol) return false;
        }
    const float det = a.m[0] * (a.m[5] * a.m[10] - a.m[6] * a.m[9]) - a.m[1] * (a.m[4] * a.m[10] - a.m[6] * a.m[8]) +
                      a.m[2] * (a.m[4] * a.m[9] - a.m[5] * a.m[8]);
    return std::fabs(det - 1) <= tol * 3;
}

/// The inverse of a rigid matrix: R^T and -R^T t.
inline Mat InverseRigid(const Mat& a) {
    Mat r = Identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i * 4 + j] = a.m[j * 4 + i];
    for (int i = 0; i < 3; ++i)
        r.m[i * 4 + 3] = -(r.m[i * 4 + 0] * a.m[3] + r.m[i * 4 + 1] * a.m[7] + r.m[i * 4 + 2] * a.m[11]);
    return r;
}

/// The rotation part as a unit quaternion x, y, z, w with w >= 0. Columns are normalised
/// first, so a node scaled by its model still gives its rotation.
inline void ToQuat(const Mat& a, float q[4]) {
    float r[3][3];
    for (int c = 0; c < 3; ++c) {
        const float x = a.m[0 * 4 + c], y = a.m[1 * 4 + c], z = a.m[2 * 4 + c];
        const float n = std::sqrt(x * x + y * y + z * z);
        const float k = n > 1e-12f ? 1.0f / n : 0.0f;
        r[0][c] = x * k, r[1][c] = y * k, r[2][c] = z * k;
    }
    const float tr = r[0][0] + r[1][1] + r[2][2];
    float x, y, z, w;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2;
        w = 0.25f * s, x = (r[2][1] - r[1][2]) / s, y = (r[0][2] - r[2][0]) / s, z = (r[1][0] - r[0][1]) / s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const float s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2;
        w = (r[2][1] - r[1][2]) / s, x = 0.25f * s, y = (r[0][1] + r[1][0]) / s, z = (r[0][2] + r[2][0]) / s;
    } else if (r[1][1] > r[2][2]) {
        const float s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2;
        w = (r[0][2] - r[2][0]) / s, x = (r[0][1] + r[1][0]) / s, y = 0.25f * s, z = (r[1][2] + r[2][1]) / s;
    } else {
        const float s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2;
        w = (r[1][0] - r[0][1]) / s, x = (r[0][2] + r[2][0]) / s, y = (r[1][2] + r[2][1]) / s, z = 0.25f * s;
    }
    const float n = std::sqrt(x * x + y * y + z * z + w * w);
    const float k = (n > 1e-12f ? 1.0f / n : 0.0f) * (w < 0 ? -1.0f : 1.0f);
    q[0] = x * k, q[1] = y * k, q[2] = z * k, q[3] = w * k;
}

inline int16_t Unit16(float v) {
    if (!(v >= -1.0f)) v = -1.0f;  // NaN lands here too
    if (v > 1.0f) v = 1.0f;
    return int16_t(std::lround(v * 32767.0f));
}

inline int16_t Mm16(float metres) {
    float mm = metres * 1000.0f;
    if (!(mm >= -32767.0f)) mm = -32767.0f;
    if (mm > 32767.0f) mm = 32767.0f;
    return int16_t(std::lround(mm));
}

// ---------------------------------------------------------------------------------------
// Which nodes go in each sample.

inline bool IsBodyBone(const std::string& name) {
    static const char kPrefix[] = "riderRIG_";
    if (name.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) return false;
    for (const char* finger : {"Thumb", "Index", "Middle", "Ring", "Pink"})
        if (name.find(finger) != std::string::npos) return false;
    return true;
}

inline std::vector<uint8_t> Pick(const std::vector<std::string>& names) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < names.size() && i < size_t(kMaxNodes); ++i)
        if (IsBodyBone(names[i])) out.push_back(uint8_t(i));
    if (out.empty())
        for (size_t i = 0; i < names.size() && i < size_t(kMaxNodes); ++i) out.push_back(uint8_t(i));
    return out;
}

// ---------------------------------------------------------------------------------------
// One frame of the skeleton.

struct Snapshot {
    Status                   status = NOT_FOUND;
    uint32_t                 stamp  = 0;      // the exe's TimeDateStamp
    uint32_t                 frame  = 0;      // the caller's count of captures, for "repeat"
    float                    offset[3] = {};  // bike body offset from the chassis (gfx +0x004)
    float                    origin[3] = {};  // rider body object, bike frame
    std::vector<std::string> names;
    std::vector<uint8_t>     parent;
    std::vector<Mat>         rel;             // each node in the bike frame
};

/// Same nodes, same names, same tree: the RIDER_SKELETON already written still describes it.
inline bool SameSkeleton(const Snapshot& a, const Snapshot& b) {
    return a.status == b.status && a.names == b.names && a.parent == b.parent &&
           std::memcmp(a.offset, b.offset, sizeof(a.offset)) == 0;
}

inline std::vector<uint8_t> SkeletonPayload(const Snapshot& s, const std::vector<uint8_t>& picked) {
    const bool ok = s.status == OK;
    const size_t n = ok ? (s.names.size() < size_t(kMaxNodes) ? s.names.size() : size_t(kMaxNodes)) : 0;
    std::vector<uint8_t> p(kSkeletonHead, 0);
    p[0] = kSkeletonLayout;
    p[1] = s.status;
    p[2] = uint8_t(n);
    p[3] = ok ? uint8_t(picked.size()) : 0;
    std::memcpy(&p[4], &s.stamp, 4);
    std::memcpy(&p[8], s.offset, 12);
    std::vector<bool> rec(n, false);
    for (uint8_t i : picked)
        if (i < n) rec[i] = true;
    for (size_t i = 0; i < n; ++i) {
        const std::string& nm = s.names[i];
        const size_t       l  = nm.size() < 255 ? nm.size() : 255;
        p.push_back(i < s.parent.size() ? s.parent[i] : kNoParent);
        p.push_back(rec[i] ? 1 : 0);
        p.push_back(uint8_t(l));
        p.insert(p.end(), nm.begin(), nm.begin() + std::ptrdiff_t(l));
    }
    return p;
}

inline void Put16(std::vector<uint8_t>& p, int16_t v) {
    p.push_back(uint8_t(uint16_t(v) & 0xFF));
    p.push_back(uint8_t(uint16_t(v) >> 8));
}

inline std::vector<uint8_t> BonesPayload(float time, float pos, const Snapshot& s, const std::vector<uint8_t>& picked,
                                         bool repeat) {
    std::vector<uint8_t> p(kBonesHead, 0);
    std::memcpy(&p[0], &time, 4);
    std::memcpy(&p[4], &pos, 4);
    p[8]  = kBonesLayout;
    p[10] = repeat ? kFlagRepeat : 0;
    std::memcpy(&p[12], s.origin, 12);
    uint8_t m = 0;
    p.reserve(kBonesHead + picked.size() * kBoneSize);
    for (uint8_t i : picked) {
        if (i >= s.rel.size()) continue;
        const Mat& r = s.rel[i];
        float      q[4];
        ToQuat(r, q);
        p.push_back(i);
        for (float v : q) Put16(p, Unit16(v));
        Put16(p, Mm16(r.m[3] - s.origin[0]));
        Put16(p, Mm16(r.m[7] - s.origin[1]));
        Put16(p, Mm16(r.m[11] - s.origin[2]));
        ++m;
    }
    p[9] = m;
    return p;
}

// ---------------------------------------------------------------------------------------
// Reading the game. `Mem` reads the game's memory: bool read(uintptr_t, void*, size_t) const.

template <class Mem, class T>
bool Read(const Mem& mem, uintptr_t addr, T& out) {
    return addr && mem.read(addr, &out, sizeof(T));
}

/// The exe's TimeDateStamp out of its PE header, or 0.
template <class Mem>
uint32_t Stamp(const Mem& mem, uintptr_t base) {
    int32_t  lfanew = 0;
    uint32_t stamp  = 0;
    if (!Read(mem, base + 0x3C, lfanew) || lfanew <= 0 || lfanew > 0x1000) return 0;
    if (!Read(mem, base + uintptr_t(lfanew) + 8, stamp)) return 0;
    return stamp;
}

/// The build the layout was read from, checked once: the stamp, then every site byte for byte.
template <class Mem>
bool BuildMatches(const Mem& mem, uintptr_t base, std::string& why) {
    const uint32_t stamp = Stamp(mem, base);
    if (stamp != mxb::RB_BUILD_STAMP) {
        char b[96];
        std::snprintf(b, sizeof(b), "game build 0x%08X, the skeleton layout is beta21e's (0x%08X)", unsigned(stamp),
                      unsigned(mxb::RB_BUILD_STAMP));
        why = b;
        return false;
    }
    for (const mxb::CodeSig& s : mxb::kRiderBoneSigs) {
        const size_t         n = std::strlen(s.mask);
        std::vector<uint8_t> got(n);
        bool                 ok = mem.read(base + s.rva, got.data(), n);
        for (size_t i = 0; ok && i < n; ++i)
            if (s.mask[i] == 'x' && got[i] != uint8_t(s.bytes[i])) ok = false;
        if (!ok) {
            char b[96];
            std::snprintf(b, sizeof(b), "code at 0x%zX (%s) differs from beta21e", size_t(s.rva), s.what);
            why = b;
            return false;
        }
    }
    why.clear();
    return true;
}

/// handle -> object, the game's own binary search (0x15C270) done with reads.
template <class Mem>
uintptr_t Object(const Mem& mem, uintptr_t table, int32_t handle) {
    int32_t count = 0;
    if (!table || handle == 0 || !Read(mem, table, count) || count <= 0 || count > 1000000) return 0;
    int32_t lo = 0, hi = count;
    while (hi > lo) {
        const int32_t   mid = lo + (hi - lo) / 2;
        const uintptr_t e   = table + mxb::OBJ_TABLE_ENTRIES + uintptr_t(mid) * mxb::OBJ_TABLE_STRIDE;
        int32_t         h   = 0;
        if (!Read(mem, e, h)) return 0;
        if (h == handle) {
            uintptr_t obj = 0;
            return Read(mem, e + mxb::OBJ_TABLE_PTR, obj) ? obj : 0;
        }
        if (h > handle) hi = mid;
        else lo = mid + 1;
    }
    return 0;
}

/// One frame of the local rider's skeleton into `out` (its `frame` and `stamp` are left to the
/// caller). Call only once BuildMatches has said yes. `why` says what was missing.
template <class Mem>
Status Capture(const Mem& mem, uintptr_t base, Snapshot& out, std::string& why) {
    // Names are assigned only when they differ, so a Draw that captures every frame stops
    // allocating after the first one.
    out.status      = NOT_FOUND;
    const auto miss = [&why, &out](const char* w) {
        out.names.clear();
        out.parent.clear();
        out.rel.clear();
        why = w;
        return NOT_FOUND;
    };
    int32_t own = 0;
    if (!Read(mem, base + mxb::RVA_OWN_VEHICLE, own) || own < 1 || own > mxb::VEHICLE_MAX) return miss("no own vehicle");
    const uintptr_t rec  = base + mxb::RVA_VEHICLES + uintptr_t(own - 1) * mxb::VEHICLE_STRIDE;
    int32_t         live = 0;
    if (!Read(mem, rec, live) || !live) return miss("own vehicle record not live");
    const uintptr_t gfx    = rec + mxb::VEH_GFX;
    int32_t         bike_h = 0, body_h = 0;
    if (!Read(mem, gfx + mxb::GFX_BIKE_BODY, bike_h) || !Read(mem, gfx + mxb::GFX_RIDER_BODY, body_h) || !bike_h ||
        !body_h)
        return miss("rider not built");
    if (!mem.read(gfx + mxb::GFX_BIKE_OFFSET, out.offset, sizeof(out.offset))) return miss("bike offset unreadable");
    uintptr_t table = 0;
    if (!Read(mem, base + mxb::RVA_OBJ_TABLE, table)) return miss("no object table");
    const uintptr_t bike = Object(mem, table, bike_h), body = Object(mem, table, body_h);
    if (!bike || !body) return miss("rider objects not in the table");
    Mat bike_w, body_w;
    if (!Read(mem, bike + mxb::OBJ_WORLD, bike_w) || !Read(mem, body + mxb::OBJ_WORLD, body_w))
        return miss("object matrices unreadable");
    if (!IsRigid(bike_w)) return miss("bike matrix is not a rotation");
    if (!Finite(body_w)) return miss("rider matrix not finite");
    uintptr_t hier = 0, nodes = 0;
    int32_t   n    = 0;
    if (!Read(mem, body + mxb::OBJ_HIERS, hier) || !hier) return miss("rider has no hierarchy");
    if (!Read(mem, hier + mxb::HIER_NODE_COUNT, n) || !Read(mem, hier + mxb::HIER_NODES, nodes) || !nodes || n < 1 ||
        n > kMaxNodes)
        return miss("rider hierarchy empty or too big");
    const Mat to_bike = InverseRigid(bike_w);
    const Mat o       = Mul(to_bike, body_w);
    out.origin[0] = o.m[3], out.origin[1] = o.m[7], out.origin[2] = o.m[11];
    out.names.resize(size_t(n));
    out.parent.assign(size_t(n), kNoParent);
    out.rel.resize(size_t(n));
    for (int32_t i = 0; i < n; ++i) {
        const uintptr_t node = nodes + uintptr_t(i) * mxb::NODE_STRIDE;
        char            name[mxb::NODE_NAME_LEN + 1] = {};
        Mat             w;
        if (!mem.read(node + mxb::NODE_NAME, name, mxb::NODE_NAME_LEN) || !Read(mem, node + mxb::NODE_WORLD, w))
            return miss("node unreadable");
        if (!Finite(w)) return miss("node matrix not finite");
        if (out.names[size_t(i)] != name) out.names[size_t(i)] = name;
        out.rel[size_t(i)]   = Mul(to_bike, w);
        // A body thrown clear of its bike is still metres, not kilometres.
        const Mat& r = out.rel[size_t(i)];
        if (std::fabs(r.m[3]) > 1000 || std::fabs(r.m[7]) > 1000 || std::fabs(r.m[11]) > 1000)
            return miss("node far from the bike");
    }
    for (int32_t i = 0; i < n; ++i) {
        const uintptr_t node = nodes + uintptr_t(i) * mxb::NODE_STRIDE;
        int32_t         nc   = 0;
        uintptr_t       kids = 0;
        if (!Read(mem, node + mxb::NODE_CHILD_COUNT, nc) || nc < 0 || nc > n) return miss("node children unreadable");
        if (nc && (!Read(mem, node + mxb::NODE_CHILDREN, kids) || !kids)) return miss("node children unreadable");
        for (int32_t k = 0; k < nc; ++k) {
            int32_t c = -1;
            if (!Read(mem, kids + uintptr_t(k) * 4, c) || c < 0 || c >= n || c == i) return miss("bad child index");
            out.parent[size_t(c)] = uint8_t(i);
        }
    }
    why.clear();
    out.status = OK;
    return OK;
}

}  // namespace riderbones
