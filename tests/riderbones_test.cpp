// The rider's skeleton for MXB Coach's recorder (src/riderbones.h).
//
// A fake MX Bikes process in sparse memory - the own-vehicle global, the vehicle record, the
// object table, the rider and bike objects and a small node tree - laid out the way offsets.h
// says beta21e lays it out. These pin the walk, the build check that keeps it off on any
// other build, what a fault or a garbage matrix turns into, and the two records' bytes, which
// are a wire contract with every .mxbc reader. Pure C++, runs anywhere.

#include "../src/coachrec.h"
#include "../src/riderbones.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n  (%s)\n", #cond);                                   \
        }                                                                       \
    } while (0)

using riderbones::Mat;

// ---------------------------------------------------------------------------------------
// Sparse memory: a read inside one region succeeds, anything else is a fault.

struct FakeMem {
    std::map<uintptr_t, std::vector<uint8_t>> regions;

    uint8_t* at(uintptr_t addr, size_t n) {
        for (auto& kv : regions)
            if (addr >= kv.first && addr + n <= kv.first + kv.second.size()) return kv.second.data() + (addr - kv.first);
        return nullptr;
    }
    void map(uintptr_t addr, size_t n) { regions[addr].assign(n, 0); }
    template <class T> void put(uintptr_t addr, const T& v) {
        uint8_t* p = at(addr, sizeof(T));
        if (!p) {
            std::printf("test bug: write outside the fake memory at 0x%zx\n", size_t(addr));
            ++g_failures;
            return;
        }
        std::memcpy(p, &v, sizeof(T));
    }
    void put_bytes(uintptr_t addr, const void* src, size_t n) {
        uint8_t* p = at(addr, n);
        if (p) std::memcpy(p, src, n);
    }
    bool read(uintptr_t addr, void* out, size_t n) const {
        for (const auto& kv : regions)
            if (addr >= kv.first && addr + n <= kv.first + kv.second.size()) {
                std::memcpy(out, kv.second.data() + (addr - kv.first), n);
                return true;
            }
        return false;
    }
};

static Mat RotY(float deg, float tx, float ty, float tz) {
    const float r = deg * 3.14159265f / 180.0f, c = std::cos(r), s = std::sin(r);
    Mat m = riderbones::Identity();
    m.m[0] = c, m.m[2] = s, m.m[8] = -s, m.m[10] = c;
    m.m[3] = tx, m.m[7] = ty, m.m[11] = tz;
    return m;
}

static Mat RotX(float deg, float tx, float ty, float tz) {
    const float r = deg * 3.14159265f / 180.0f, c = std::cos(r), s = std::sin(r);
    Mat m = riderbones::Identity();
    m.m[5] = c, m.m[6] = -s, m.m[9] = s, m.m[10] = c;
    m.m[3] = tx, m.m[7] = ty, m.m[11] = tz;
    return m;
}

static bool Near(const Mat& a, const Mat& b, float tol = 1e-4f) {
    for (int i = 0; i < 16; ++i)
        if (std::fabs(a.m[i] - b.m[i]) > tol) return false;
    return true;
}

// ---------------------------------------------------------------------------------------
// The fake game.

constexpr uintptr_t kBase  = 0x140000000ull;
constexpr uintptr_t kTable = 0x7000000ull;
constexpr uintptr_t kBike  = 0x7100000ull, kBody = 0x7200000ull;
constexpr uintptr_t kHier  = 0x7300000ull, kNodes = 0x7400000ull, kKids = 0x7500000ull;
constexpr int       kOwn   = 3;                 // own vehicle record + 1
constexpr int32_t   kBikeHandle = 70, kBodyHandle = 80;

// The stock rider's node names, in the order the stock .anm lists them (Point01 first).
static const char* kStock[] = {
    "Point01", "riderRIG_Root", "riderRIG_Pelvis", "riderRIG_LeftHip", "riderRIG_LeftKnee", "riderRIG_LeftAnkle",
    "riderRIG_LeftToe", "riderRIG_LeftKneeTwist", "riderRIG_LeftHipTwist1", "riderRIG_LeftHipTwist2",
    "riderRIG_RightHip", "riderRIG_RightKnee", "riderRIG_RightAnkle", "riderRIG_RightToe",
    "riderRIG_RightKneeTwist", "riderRIG_RightHipTwist1", "riderRIG_RightHipTwist2", "riderRIG_Spine1",
    "riderRIG_Spine2", "riderRIG_Spine3", "riderRIG_Spine4", "riderRIG_Neck1", "riderRIG_Neck2", "riderRIG_Head",
    "riderRIG_LeftCollar", "riderRIG_LeftShoulder", "riderRIG_LeftElbow", "riderRIG_LeftWrist",
    "riderRIG_LeftThumb1", "riderRIG_LeftThumb2", "riderRIG_LeftThumb3", "riderRIG_LeftPink1", "riderRIG_LeftPink2",
    "riderRIG_LeftPink3", "riderRIG_LeftIndex1", "riderRIG_LeftIndex2", "riderRIG_LeftIndex3",
    "riderRIG_LeftMiddle1", "riderRIG_LeftMiddle2", "riderRIG_LeftMiddle3", "riderRIG_LeftRing1",
    "riderRIG_LeftRing2", "riderRIG_LeftRing3", "riderRIG_LeftElbowTwist1", "riderRIG_LeftElbowTwist2",
    "riderRIG_LeftShoulderTwist1", "riderRIG_LeftShoulderTwist2", "riderRIG_RightCollar", "riderRIG_RightShoulder",
    "riderRIG_RightElbow", "riderRIG_RightWrist", "riderRIG_RightThumb1", "riderRIG_RightThumb2",
    "riderRIG_RightThumb3", "riderRIG_RightPink1", "riderRIG_RightPink2", "riderRIG_RightPink3",
    "riderRIG_RightIndex1", "riderRIG_RightIndex2", "riderRIG_RightIndex3", "riderRIG_RightMiddle1",
    "riderRIG_RightMiddle2", "riderRIG_RightMiddle3", "riderRIG_RightRing1", "riderRIG_RightRing2",
    "riderRIG_RightRing3", "riderRIG_RightElbowTwist1", "riderRIG_RightElbowTwist2", "riderRIG_RightShoulderTwist1",
    "riderRIG_RightShoulderTwist2", "riderRIG_armour",
};
constexpr int kStockCount = int(sizeof(kStock) / sizeof(kStock[0]));

struct Game {
    FakeMem mem;
    Mat     bike_w = RotY(90, 10, 1, 5);  // the bike, turned to face +x, somewhere on the track
    // Four nodes: Point01 -> riderRIG_Root -> riderRIG_Pelvis -> riderRIG_Head, each placed in
    // the bike frame as below, so the test knows what should come back.
    std::vector<std::string> names = {"Point01", "riderRIG_Root", "riderRIG_Pelvis", "riderRIG_Head"};
    std::vector<int>         parent = {-1, 0, 1, 2};
    std::vector<Mat>         rel    = {riderbones::Identity(), RotX(0, 0, 0.9f, -0.1f), RotX(20, 0, 1.0f, -0.05f),
                                       RotX(-35, 0.02f, 1.6f, 0.25f)};
    Mat                      body_rel = RotX(0, 0, 0.85f, -0.12f);

    Game() {
        // The image: a PE header with beta21e's stamp, and the code sites the check reads.
        mem.map(kBase, 0x400);
        mem.put<int32_t>(kBase + 0x3C, 0x100);
        mem.put<uint32_t>(kBase + 0x100 + 8, mxb::RB_BUILD_STAMP);
        for (const mxb::CodeSig& s : mxb::kRiderBoneSigs) {
            const size_t n = std::strlen(s.mask);
            mem.map(kBase + s.rva, n);
            std::vector<uint8_t> b(s.bytes, s.bytes + n);
            for (size_t i = 0; i < n; ++i)
                if (s.mask[i] != 'x') b[i] = 0xCC;  // a call target this build happens to have
            mem.put_bytes(kBase + s.rva, b.data(), n);
        }
        // Globals.
        mem.map(kBase + mxb::RVA_OWN_VEHICLE, 4);
        mem.put<int32_t>(kBase + mxb::RVA_OWN_VEHICLE, kOwn);
        mem.map(kBase + mxb::RVA_OBJ_TABLE, 8);
        mem.put<uint64_t>(kBase + mxb::RVA_OBJ_TABLE, kTable);
        // The own vehicle record, live, with its gfx block's two handles and the bike offset.
        const uintptr_t rec = kBase + mxb::RVA_VEHICLES + uintptr_t(kOwn - 1) * mxb::VEHICLE_STRIDE;
        mem.map(rec, mxb::VEHICLE_STRIDE);
        mem.put<int32_t>(rec, 1);
        const uintptr_t gfx = rec + mxb::VEH_GFX;
        mem.put<int32_t>(gfx + mxb::GFX_BIKE_BODY, kBikeHandle);
        const float off[3] = {0.0f, -0.0062f, 0.0273f};
        mem.put_bytes(gfx + mxb::GFX_BIKE_OFFSET, off, sizeof(off));
        mem.put<int32_t>(gfx + mxb::GFX_RIDER_BODY, kBodyHandle);
        // The object table: sorted by handle, ours among others.
        const int32_t handles[] = {12, kBikeHandle, 71, kBodyHandle, 99};
        const uintptr_t objs[]  = {0x9990000, kBike, 0x9991000, kBody, 0x9992000};
        mem.map(kTable, 4 + 16 * 5);
        mem.put<int32_t>(kTable, 5);
        for (int i = 0; i < 5; ++i) {
            mem.put<int32_t>(kTable + 4 + uintptr_t(i) * 16, handles[i]);
            mem.put<uint64_t>(kTable + 4 + uintptr_t(i) * 16 + 8, objs[i]);
        }
        // The objects.
        mem.map(kBike, 0x400);
        mem.map(kBody, 0x400);
        mem.put(kBike + mxb::OBJ_WORLD, bike_w);
        mem.put(kBody + mxb::OBJ_WORLD, riderbones::Mul(bike_w, body_rel));
        mem.put<uint64_t>(kBody + mxb::OBJ_HIERS, kHier);
        // The hierarchy and its nodes.
        const int n = int(names.size());
        mem.map(kHier, 0x100);
        mem.put<int32_t>(kHier + mxb::HIER_ROOT_COUNT, 1);
        mem.put<int32_t>(kHier + mxb::HIER_NODE_COUNT, n);
        mem.put<uint64_t>(kHier + mxb::HIER_NODES, kNodes);
        mem.map(kNodes, size_t(n) * mxb::NODE_STRIDE);
        mem.map(kKids, 0x100);
        for (int i = 0; i < n; ++i) {
            const uintptr_t node = kNodes + uintptr_t(i) * mxb::NODE_STRIDE;
            mem.put_bytes(node + mxb::NODE_NAME, names[size_t(i)].c_str(), names[size_t(i)].size() + 1);
            mem.put(node + mxb::NODE_WORLD, riderbones::Mul(bike_w, rel[size_t(i)]));
            int kids = 0;
            for (int c = 0; c < n; ++c)
                if (parent[size_t(c)] == i) {
                    mem.put<int32_t>(kKids + uintptr_t(i) * 16 + uintptr_t(kids) * 4, c);
                    ++kids;
                }
            mem.put<int32_t>(node + mxb::NODE_CHILD_COUNT, kids);
            if (kids) mem.put<uint64_t>(node + mxb::NODE_CHILDREN, kKids + uintptr_t(i) * 16);
        }
    }
};

// ---------------------------------------------------------------------------------------

static void math() {
    const Mat a = RotY(37, 1, 2, 3);
    CHECK(riderbones::IsRigid(a), "a rotation and a move is rigid");
    CHECK(Near(riderbones::Mul(riderbones::InverseRigid(a), a), riderbones::Identity()), "the inverse undoes it");
    Mat scaled = a;
    scaled.m[0] *= 2;
    CHECK(!riderbones::IsRigid(scaled), "a scaled matrix is not");

    float q[4];
    riderbones::ToQuat(RotY(90, 0, 0, 0), q);
    CHECK(std::fabs(q[1] - std::sqrt(0.5f)) < 1e-5f && std::fabs(q[3] - std::sqrt(0.5f)) < 1e-5f &&
              std::fabs(q[0]) < 1e-5f && std::fabs(q[2]) < 1e-5f,
          "90 degrees about y, got %f %f %f %f", double(q[0]), double(q[1]), double(q[2]), double(q[3]));
    riderbones::ToQuat(RotX(180, 0, 0, 0), q);
    CHECK(std::fabs(std::fabs(q[0]) - 1) < 1e-5f, "a half turn is found too, got x=%f", double(q[0]));
    // A node the model scales still gives its rotation.
    Mat big = RotX(30, 0, 0, 0);
    for (int i = 0; i < 12; ++i)
        if (i % 4 != 3) big.m[i] *= 0.01f;
    riderbones::ToQuat(big, q);
    CHECK(std::fabs(q[0] - std::sin(15 * 3.14159265f / 180)) < 1e-4f, "scale is not rotation, got %f", double(q[0]));
    CHECK(q[3] >= 0, "w is kept positive");

    CHECK(riderbones::Unit16(1.0f) == 32767 && riderbones::Unit16(-1.0f) == -32767, "unit range");
    CHECK(riderbones::Unit16(std::nanf("")) == -32767, "NaN is clamped, not undefined");
    CHECK(riderbones::Mm16(0.5f) == 500 && riderbones::Mm16(-0.001f) == -1, "millimetres");
    CHECK(riderbones::Mm16(100.0f) == 32767, "past 32 m is clamped");
}

static void which_bones() {
    std::vector<std::string> stock(kStock, kStock + kStockCount);
    CHECK(kStockCount == 71, "the stock rider has 71 nodes, got %d", kStockCount);
    const std::vector<uint8_t> p = riderbones::Pick(stock);
    CHECK(p.size() == 40, "40 body bones without the fingers, got %zu", p.size());
    bool head = false, finger = false, point = false;
    for (uint8_t i : p) {
        head |= stock[i] == "riderRIG_Head";
        finger |= stock[i].find("Thumb") != std::string::npos || stock[i].find("Index") != std::string::npos;
        point |= stock[i] == "Point01";
    }
    CHECK(head, "the head is recorded");
    CHECK(!finger, "fingers are not");
    CHECK(!point, "a helper node is not");

    const std::vector<std::string> other = {"bip01", "bip01 pelvis", "bip01 head"};
    CHECK(riderbones::Pick(other).size() == 3, "a model with other names records every node");
}

static void build_check() {
    Game        g;
    std::string why;
    CHECK(riderbones::BuildMatches(g.mem, kBase, why), "beta21e is recognised: %s", why.c_str());
    CHECK(riderbones::Stamp(g.mem, kBase) == 0x6A21833Du, "the stamp is read out of the PE header");

    // Another build: the stamp alone stops it, before any code is read.
    Game other;
    other.mem.put<uint32_t>(kBase + 0x100 + 8, 0x6B000000u);
    CHECK(!riderbones::BuildMatches(other.mem, kBase, why), "another stamp is another build");
    CHECK(why.find("0x6B000000") != std::string::npos, "and says which, got \"%s\"", why.c_str());

    // The same stamp with one instruction changed is not trusted either.
    for (const mxb::CodeSig& s : mxb::kRiderBoneSigs) {
        Game patched;
        size_t i = 0;
        while (s.mask[i] != 'x') ++i;
        uint8_t b = 0;
        patched.mem.read(kBase + s.rva + i, &b, 1);
        b ^= 0xFF;
        patched.mem.put_bytes(kBase + s.rva + i, &b, 1);
        CHECK(!riderbones::BuildMatches(patched.mem, kBase, why), "a changed site at 0x%zx is caught", size_t(s.rva));
        CHECK(why.find(s.what) != std::string::npos, "and named, got \"%s\"", why.c_str());
    }

    // Code we can't read (a page the game unmapped) is a mismatch, not a crash.
    Game holed;
    holed.mem.regions.erase(kBase + mxb::kRiderBoneSigs[0].rva);
    CHECK(!riderbones::BuildMatches(holed.mem, kBase, why), "unreadable code is not beta21e");
}

static void capture() {
    Game                 g;
    riderbones::Snapshot s;
    std::string          why;
    CHECK(riderbones::Capture(g.mem, kBase, s, why) == riderbones::OK, "the skeleton is found: %s", why.c_str());
    CHECK(s.names == g.names, "every node, in the game's order");
    CHECK(s.parent.size() == 4 && s.parent[0] == riderbones::kNoParent && s.parent[1] == 0 && s.parent[2] == 1 &&
              s.parent[3] == 2,
          "the tree out of the children lists");
    for (size_t i = 0; i < g.rel.size(); ++i)
        CHECK(Near(s.rel[i], g.rel[i]), "node %zu comes back in the bike frame", i);
    CHECK(std::fabs(s.origin[1] - 0.85f) < 1e-4f && std::fabs(s.origin[2] + 0.12f) < 1e-4f,
          "the rider body's origin in the bike frame");
    CHECK(std::fabs(s.offset[2] - 0.0273f) < 1e-6f, "the bike body's offset from the chassis");
}

static void capture_fails_safe() {
    riderbones::Snapshot s;
    std::string          why;
    {
        Game g;
        g.mem.put<int32_t>(kBase + mxb::RVA_OWN_VEHICLE, 0);
        CHECK(riderbones::Capture(g.mem, kBase, s, why) == riderbones::NOT_FOUND, "no own vehicle, no skeleton");
        CHECK(s.names.empty() && s.rel.empty(), "and nothing half-filled");
    }
    {
        Game g;
        g.mem.put<int32_t>(kBase + mxb::RVA_OWN_VEHICLE, 51);
        CHECK(riderbones::Capture(g.mem, kBase, s, why) == riderbones::NOT_FOUND, "an index past the 50 records");
    }
    {
        Game g;
        g.mem.put<int32_t>(kTable + 4 + 3 * 16, 81);  // the body's handle gone from the table
        CHECK(riderbones::Capture(g.mem, kBase, s, why) == riderbones::NOT_FOUND, "an object not in the table");
    }
    {
        Game g;
        g.mem.regions.erase(kNodes);  // freed under us
        CHECK(riderbones::Capture(g.mem, kBase, s, why) == riderbones::NOT_FOUND, "a fault is a miss");
    }
    {
        Game g;
        Mat  junk = g.bike_w;
        junk.m[0] = 3;
        g.mem.put(kBike + mxb::OBJ_WORLD, junk);
        CHECK(riderbones::Capture(g.mem, kBase, s, why) == riderbones::NOT_FOUND, "a bike frame that isn't one");
    }
    {
        Game g;
        Mat  w = riderbones::Mul(g.bike_w, g.rel[3]);
        w.m[7] = std::nanf("");
        g.mem.put(kNodes + 3 * mxb::NODE_STRIDE + mxb::NODE_WORLD, w);
        CHECK(riderbones::Capture(g.mem, kBase, s, why) == riderbones::NOT_FOUND, "a NaN in a node");
    }
    {
        Game g;
        g.mem.put<int32_t>(kKids + 2 * 16, 9);  // a child index past the nodes
        CHECK(riderbones::Capture(g.mem, kBase, s, why) == riderbones::NOT_FOUND, "a child that isn't a node");
    }
    {
        Game g;
        g.mem.put<int32_t>(kHier + mxb::HIER_NODE_COUNT, 100000);
        CHECK(riderbones::Capture(g.mem, kBase, s, why) == riderbones::NOT_FOUND, "a node count that is garbage");
    }
}

// The bytes are a contract with every reader: sizes and field places are pinned here.
static void record_bytes() {
    CHECK(coachrec::RIDER_SKELETON == 18 && coachrec::RIDER_BONES == 19, "the tags readers look for");
    CHECK(riderbones::kSkeletonHead == 20 && riderbones::kBonesHead == 24 && riderbones::kBoneSize == 15,
          "head and per-bone sizes");

    Game                 g;
    riderbones::Snapshot s;
    std::string          why;
    riderbones::Capture(g.mem, kBase, s, why);
    s.stamp = mxb::RB_BUILD_STAMP;
    const std::vector<uint8_t> pick = riderbones::Pick(s.names);
    CHECK(pick.size() == 3 && pick[0] == 1 && pick[2] == 3, "Root, Pelvis, Head; not Point01");

    const std::vector<uint8_t> k = riderbones::SkeletonPayload(s, pick);
    CHECK(k[0] == 1 && k[1] == riderbones::OK && k[2] == 4 && k[3] == 3, "layout, status, nodes, recorded");
    uint32_t stamp;
    std::memcpy(&stamp, &k[4], 4);
    CHECK(stamp == 0x6A21833Du, "the build it came from");
    size_t at = riderbones::kSkeletonHead;
    for (size_t i = 0; i < 4; ++i) {
        const uint8_t parent = k[at], rec = k[at + 1], len = k[at + 2];
        const std::string name(reinterpret_cast<const char*>(&k[at + 3]), len);
        CHECK(name == g.names[i], "node %zu's name, got \"%s\"", i, name.c_str());
        CHECK(parent == (i == 0 ? 255 : i - 1), "node %zu's parent", i);
        CHECK(rec == (i == 0 ? 0 : 1), "node %zu recorded or not", i);
        at += 3 + len;
    }
    CHECK(at == k.size(), "nothing after the last node");

    const std::vector<uint8_t> b = riderbones::BonesPayload(12.5f, 0.25f, s, pick, true);
    CHECK(b.size() == 24 + 15 * 3, "24 + 15 per bone, got %zu", b.size());
    float head[5];
    std::memcpy(head, &b[0], 8);
    std::memcpy(head + 2, &b[12], 12);
    CHECK(head[0] == 12.5f && head[1] == 0.25f, "time and lap position lead");
    CHECK(b[8] == 1 && b[9] == 3 && b[10] == riderbones::kFlagRepeat && b[11] == 0, "layout, count, repeat flag");
    CHECK(std::fabs(head[3] - 0.85f) < 1e-5f, "the origin");
    // The last bone is the head: decode it as a reader would and compare with what was put in.
    const uint8_t* h = &b[24 + 15 * 2];
    int16_t        v[7];
    std::memcpy(v, h + 1, 14);
    CHECK(h[0] == 3, "the head's node index");
    const float qx = v[0] / 32767.0f, qw = v[3] / 32767.0f;
    const float want = std::sin(-17.5f * 3.14159265f / 180);
    CHECK(std::fabs(qx - want) < 1e-4f && std::fabs(qw - std::cos(17.5f * 3.14159265f / 180)) < 1e-4f,
          "the head's pitch, got qx=%f qw=%f", double(qx), double(qw));
    const float y = head[3] + v[5] / 1000.0f, z = head[4] + v[6] / 1000.0f;
    CHECK(std::fabs(y - 1.6f) < 1e-3f && std::fabs(z - 0.25f) < 1e-3f, "the head's place to the millimetre");

    // Unknown is said, with nothing after it.
    riderbones::Snapshot off;
    off.status = riderbones::UNKNOWN_BUILD;
    off.stamp  = 0x6B000000u;
    const std::vector<uint8_t> u = riderbones::SkeletonPayload(off, {});
    CHECK(u.size() == riderbones::kSkeletonHead && u[1] == riderbones::UNKNOWN_BUILD && u[2] == 0 && u[3] == 0,
          "an unknown build is a bare head");
}

// Old readers walk a file by each record's length and skip a tag they don't know. A file with
// the new records in it must still read the old ones exactly as before.
static void old_readers_skip() {
    const std::string path = "riderbones_test.mxbc";
    {
        coachrec::Writer w;
        CHECK(w.open(path.c_str()), "can write a test file");
        const uint8_t sample[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        Game                 g;
        riderbones::Snapshot s;
        std::string          why;
        riderbones::Capture(g.mem, kBase, s, why);
        const auto pick = riderbones::Pick(s.names);
        const auto k    = riderbones::SkeletonPayload(s, pick);
        const auto b    = riderbones::BonesPayload(1, 0, s, pick, false);
        w.record(coachrec::RIDER_SKELETON, k.data(), uint32_t(k.size()));
        w.record(coachrec::SAMPLE, sample, sizeof(sample));
        w.record(coachrec::RIDER_BONES, b.data(), uint32_t(b.size()));
        w.record(coachrec::END);
    }
    std::FILE* f = std::fopen(path.c_str(), "rb");
    std::vector<uint8_t> d;
    if (f) {
        uint8_t buf[4096];
        size_t  n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
        std::fclose(f);
    }
    std::remove(path.c_str());
    // A reader that knows only SAMPLE and END.
    size_t at = 8;
    int    samples = 0, ends = 0, skipped = 0;
    while (at + 8 <= d.size()) {
        uint32_t len;
        std::memcpy(&len, &d[at + 4], 4);
        const uint8_t tag = d[at];
        if (tag == coachrec::SAMPLE) samples += len == 16 && d[at + 8] == 1 && d[at + 8 + 15] == 16;
        else if (tag == coachrec::END) ++ends;
        else ++skipped;
        at += 8 + len;
    }
    CHECK(at == d.size(), "the lengths walk the file exactly");
    CHECK(samples == 1 && ends == 1 && skipped == 2, "old records intact, new ones skipped: %d/%d/%d", samples,
          ends, skipped);
}

int main() {
    math();
    which_bones();
    build_check();
    capture();
    capture_fails_safe();
    record_bytes();
    old_readers_skip();
    if (g_failures) {
        std::printf("riderbones: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("riderbones: all checks passed\n");
    return 0;
}
