// tyrelog: the last ~2 s of the bike's tyre state, kept in a ring and written to a CSV when the
// physics NaN shows up. Diagnostic only, rides on nantrap=1, off by default.
//
// Why: the leading theory for the physics NaN (research nan-why-code.md, cause 1) is that the
// tyre sample radius runs away with wheel spin. 0x1A5A10, once per wheel per step, does
//
//     R = r0 + k * |spin| / 2pi                      (0x1A5BE3..0x1A5C47, no clamp)
//     sample = wheel matrix * (0, -R cos a, R sin a) (15 points, 0x1A5CB0..0x1A5E75)
//
// and the first refused NaN height query in a crash we have came from that loop (0x1A5E43),
// 25+ steps before anything else went bad. If the theory is right, spin and R climb step over
// step BEFORE the trap fires or the first query is refused. This ring is the evidence either
// way: per step, per wheel, the spin, R, the first sample point, the deepest ground gap, plus
// the chassis position and velocities.
//
// Read-only: plain copies out of the game's memory after each step returns, nothing written
// back, nothing computed during the step. The live half is in frostmod.cpp (TYRELOG section);
// everything here is pure so the ring and the CSV are tested anywhere: tests/tyrelog_test.cpp.
//
// Layout, read off mxbikes.exe beta21e (RVAs, base 0x140000000). Each is checked against the
// running exe's bytes before the ring turns on (kChecks below); one mismatch and it stays off.
//   bike         = the item in the world slot's bike list, the rcx of the bike sim 0x1ADA40
//   wheel i      = bike + i * 0xE310                          (0x1A5A41 imul rbp, rbp, 0xE310)
//   spin         = double [wheel + 0x12208], rad/s            (0x1A5BE3, |.| taken by the game)
//   R            = double [wheel + 0x121D8], written per step (0x1A5C47)
//   sample 0     = float x,y,z [wheel + 0x12210]; 15 points, 0x60 apart (0x1A5BFB, 0x1A5E6D)
//   ground gap   = double [wheel + 0x140A0]: the smallest (sample - ground) of the samples that
//                  found ground; negative is into the ground (0x1A6117). Kept when none did.
//   hit          = int [wheel + 0x127B8]: any sample found ground this step (0x1A6123)
//   chassis body = [bike + 0x430], an ODE dxBody (0x1AFC70): pos +0xC0, lvel +0x110,
//                  avel +0x120 (dBodyGetPosition/LinearVel/AngularVel 0x2AC8B0/8E0/8F0)
//   bike time    = float [bike + 0x40], seconds since spawn (0x1AEB24)
//   active       = int [bike + 0x38]; the bike sim returns at once when 0 (0x1ADA4B)
//   world slot   = lea at sim step + 0x2B (0x1BE3CB) + (slot-1) * 0x6160 (0x1BE3DC)
//   bike list    = [world + 0x28] (0x1BE657): int count at +0, item i's pointer at
//                  +4 + 16*i + 8 (0x15C350: imul rcx, 0x10 / lea [rax+rcx+4] / mov rax, [rax+8])
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace tyrelog {

// ---- layout --------------------------------------------------------------------------------
inline constexpr int kWheels = 2;              // 0x1ADDBD mov r12d, 2: the wheel loop's bound
inline constexpr size_t kWheelStride  = 0xE310;
inline constexpr size_t kWheelSpin    = 0x12208;
inline constexpr size_t kWheelRadius  = 0x121D8;
inline constexpr size_t kWheelSample0 = 0x12210;
inline constexpr size_t kWheelGap     = 0x140A0;
inline constexpr size_t kWheelHit     = 0x127B8;
inline constexpr size_t kBikeActive   = 0x38;
inline constexpr size_t kBikeTime     = 0x40;
inline constexpr size_t kBikeChassis  = 0x430;
inline constexpr size_t kBodyPos  = 0xC0;
inline constexpr size_t kBodyLvel = 0x110;
inline constexpr size_t kBodyAvel = 0x120;
/// The last byte a read touches past `bike`: wheel 1's ground gap. Tests size the fake to it.
inline constexpr size_t kBikeSpan = kWheelStride * (kWheels - 1) + kWheelGap + 8;
inline constexpr size_t kBodySpan = kBodyAvel + 12;

inline constexpr size_t kWorldStride   = 0x6160;
inline constexpr size_t kWorldBikeList = 0x28;
inline constexpr int    kSimStepLeaAt  = 0x2B;   // sim step + 0x2B: lea rax, [rip + rel32]
inline constexpr int    kMaxBikes      = 64;     // a list longer than this is not a bike list

/// One byte check against the running exe. Every RVA here is beta21e's.
struct Check {
    uint32_t rva;
    const char* bytes;
    int len;
    const char* what;
};
inline constexpr Check kChecks[] = {
    {0x1A5A41, "\x48\x69\xED\x10\xE3\x00\x00", 7, "wheel stride 0xE310"},
    {0x1A5BE3, "\xF2\x0F\x10\x84\x2E\x08\x22\x01\x00", 9, "spin [w+0x12208]"},
    {0x1A5C47, "\xF2\x0F\x11\x94\x2E\xD8\x21\x01\x00", 9, "radius [w+0x121D8]"},
    {0x1A5BFB, "\x48\x8D\xBC\x2E\x14\x22\x01\x00", 8, "sample 0 [w+0x12214]"},
    {0x1A6117, "\xF2\x0F\x11\x84\x2E\xA0\x40\x01\x00", 9, "ground gap [w+0x140A0]"},
    {0x1A6123, "\x89\x9C\x2E\xB8\x27\x01\x00", 7, "hit [w+0x127B8]"},
    {0x1AFC70, "\x48\x8B\x8E\x30\x04\x00\x00", 7, "chassis body [bike+0x430]"},
    {0x2AC8B0, "\x48\x8D\x81\xC0\x00\x00\x00", 7, "body pos +0xC0"},
    {0x2AC8E0, "\x48\x8D\x81\x10\x01\x00\x00", 7, "body lvel +0x110"},
    {0x2AC8F0, "\x48\x8D\x81\x20\x01\x00\x00", 7, "body avel +0x120"},
    {0x1AEB24, "\xF3\x44\x0F\x58\x47\x40", 6, "bike time [bike+0x40]"},
    {0x1ADA4B, "\x83\x79\x38\x00", 4, "bike active [bike+0x38]"},
    {0x1BE3CB, "\x48\x8D\x05", 3, "world array lea"},
    {0x1BE3DC, "\x48\x69\xFF\x60\x61\x00\x00", 7, "world stride 0x6160"},
    {0x1BE657, "\x48\x8B\x4F\x28", 4, "bike list [world+0x28]"},
    {0x15C3B6, "\x48\x6B\xC9\x10", 4, "list item stride 16"},
    {0x15C3BF, "\x48\x8D\x44\x08\x04", 5, "list items at +4"},
    {0x15C3E1, "\x48\x8B\x40\x08", 4, "item pointer at +8"},
};

// ---- frostmod_radar.cfg ----------------------------------------------------------------------
// On with nantrap=1. tyrelog=0 turns the ring off while keeping the trap; tyrelog=1 on its own,
// without nantrap=1, does nothing.
struct Config {
    bool on = true;   // only consulted when nantrap is on
};

/// One cfg line. Returns true if it was ours.
inline bool ParseCfgLine(const char* line, Config& c) {
    if (!line) return false;
    while (*line == ' ' || *line == '\t') ++line;
    if (std::strncmp(line, "tyrelog", 7) != 0) return false;
    const char* p = line + 7;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '=') return false;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p < '0' || *p > '9') return false;
    c.on = *p != '0';
    return true;
}
inline bool Enabled(bool nantrapOn, const Config& c) { return nantrapOn && c.on; }

// ---- one step ----------------------------------------------------------------------------------
struct Wheel {
    double spin = 0;      // rad/s
    double radius = 0;    // R, metres
    double gap = 0;       // smallest sample-minus-ground, metres
    float sample[3] = {0, 0, 0};
    int32_t hit = 0;
};

struct Step {
    uint64_t step = 0;    // the hook's own step counter
    float dt = 0;         // seconds, as handed to the sim step
    float bikeTime = 0;   // [bike+0x40]
    int16_t slot = 0;     // world slot, 1..3
    int16_t bikes = 0;    // bikes in that slot's list
    int16_t index = -1;   // which of them this is
    int16_t body = 0;     // 1 if the chassis body was read
    float pos[3] = {0, 0, 0};
    float lvel[3] = {0, 0, 0};
    float avel[3] = {0, 0, 0};
    Wheel wheel[kWheels];
};

/// Copies one bike's state into `s`. Plain loads only: no arithmetic on what it reads, so a NaN
/// or an infinity in the game's memory is copied as it is and raises nothing. `body` may be
/// null (a bike with no chassis body yet); the velocities then stay zero and `s.body` is 0.
inline void ReadBike(const uint8_t* bike, const uint8_t* body, Step& s) {
    std::memcpy(&s.bikeTime, bike + kBikeTime, 4);
    for (int i = 0; i < kWheels; ++i) {
        const uint8_t* w = bike + kWheelStride * (size_t)i;
        Wheel& o = s.wheel[i];
        std::memcpy(&o.spin, w + kWheelSpin, 8);
        std::memcpy(&o.radius, w + kWheelRadius, 8);
        std::memcpy(&o.gap, w + kWheelGap, 8);
        std::memcpy(o.sample, w + kWheelSample0, 12);
        std::memcpy(&o.hit, w + kWheelHit, 4);
    }
    s.body = body ? 1 : 0;
    if (body) {
        std::memcpy(s.pos, body + kBodyPos, 12);
        std::memcpy(s.lvel, body + kBodyLvel, 12);
        std::memcpy(s.avel, body + kBodyAvel, 12);
    } else {
        for (int k = 0; k < 3; ++k) s.pos[k] = s.lvel[k] = s.avel[k] = 0;
    }
}

/// The bike list's item `i`, the way 0x15C350 reads it. Null past the end.
inline const uint8_t* ListItem(const uint8_t* list, int i) {
    if (!list || i < 0) return nullptr;
    int32_t n;
    std::memcpy(&n, list, 4);
    if (i >= n) return nullptr;
    const uint8_t* p;
    std::memcpy(&p, list + 4 + 16 * (size_t)i + 8, sizeof(p));
    return p;
}
inline int ListCount(const uint8_t* list) {
    if (!list) return 0;
    int32_t n;
    std::memcpy(&n, list, 4);
    return n;
}

// ---- the ring ------------------------------------------------------------------------------------
inline constexpr size_t kSteps = 1000;   // 2 s at the game's fixed 2 ms step

/// One writer (the physics thread), readers at dump time. The writer fills the next slot in
/// place and then publishes it; a reader on another thread (a crash elsewhere) can see the
/// oldest row half-overwritten, which a diagnostic can live with.
template <size_t N>
struct Ring {
    Step buf[N];
    std::atomic<uint64_t> written{0};

    Step& Next() { return buf[written.load(std::memory_order_relaxed) % N]; }
    void Commit() { written.store(written.load(std::memory_order_relaxed) + 1, std::memory_order_release); }
    size_t Count() const {
        const uint64_t w = written.load(std::memory_order_acquire);
        return w < N ? (size_t)w : N;
    }
    /// i = 0 is the oldest step still held.
    const Step& At(size_t i) const {
        const uint64_t w = written.load(std::memory_order_acquire);
        const uint64_t first = w < N ? 0 : w - N;
        return buf[(first + i) % N];
    }
    void Clear() { written.store(0, std::memory_order_release); }
};

// ---- the CSV --------------------------------------------------------------------------------------
using Emit = void (*)(void* user, const char* line);

/// %.9g, and nan / inf / -inf spelled out so a spreadsheet does not choke on 1.#QNAN.
inline void Num(double v, char* out, size_t n) {
    if (std::isnan(v)) std::snprintf(out, n, "nan");
    else if (std::isinf(v)) std::snprintf(out, n, v > 0 ? "inf" : "-inf");
    else std::snprintf(out, n, "%.9g", v);
}
inline double Len3(const float* v) {
    return std::sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]);
}

struct Meta {
    const char* reason = "";    // "trap" | "refusal" | "crash"
    const char* version = "";   // FrostMod's
    const char* whenUtc = "";
    const char* track = "";
};

inline const char* kHeader =
    "t_ms,step,dt_ms,slot,bikes,bike,bike_t,body,pos_x,pos_y,pos_z,lvel_x,lvel_y,lvel_z,speed,"
    "avel_x,avel_y,avel_z,aspeed,"
    "w0_spin,w0_radius,w0_s0_x,w0_s0_y,w0_s0_z,w0_gap,w0_hit,"
    "w1_spin,w1_radius,w1_s0_x,w1_s0_y,w1_s0_z,w1_gap,w1_hit";

/// The ring, oldest first. t_ms is the time before the newest row (0 on the last line), summed
/// from each step's dt so it holds even when steps bunch up on a slow frame. Lines starting
/// with '#' are metadata. No Steam ID, GUID or name goes in here: only the track folder.
template <size_t N>
inline void WriteCsv(const Ring<N>& r, const Meta& m, Emit emit, void* user) {
    char line[1024];
    std::snprintf(line, sizeof(line), "# frostmod nan ring: reason=%s frostmod=%s when=%s track=%s",
                  m.reason, m.version, m.whenUtc, m.track);
    emit(user, line);
    const size_t n = r.Count();
    std::snprintf(line, sizeof(line),
                  "# %zu step(s), oldest first. spin rad/s, radius/gap/pos m, lvel m/s, avel rad/s. "
                  "gap < 0 is into the ground.", n);
    emit(user, line);
    emit(user, kHeader);
    // Time back from the newest row: walk newest to oldest once to sum, then emit oldest first.
    double total = 0;
    for (size_t i = 1; i < n; ++i) total += r.At(i).dt;   // the gaps between rows
    double prefix = 0;   // summed in the same order, so the newest row lands on exactly 0
    for (size_t i = 0; i < n; ++i) {
        const Step& s = r.At(i);
        if (i > 0) prefix += s.dt;
        const double t = prefix - total;
        char f[32][32];
        int k = 0;
        Num(t * 1000.0, f[k++], 32);
        Num(s.dt * 1000.0, f[k++], 32);
        Num(s.bikeTime, f[k++], 32);
        for (int c = 0; c < 3; ++c) Num(s.pos[c], f[k++], 32);
        for (int c = 0; c < 3; ++c) Num(s.lvel[c], f[k++], 32);
        Num(Len3(s.lvel), f[k++], 32);
        for (int c = 0; c < 3; ++c) Num(s.avel[c], f[k++], 32);
        Num(Len3(s.avel), f[k++], 32);
        for (int w = 0; w < kWheels; ++w) {
            const Wheel& o = s.wheel[w];
            Num(o.spin, f[k++], 32);
            Num(o.radius, f[k++], 32);
            for (int c = 0; c < 3; ++c) Num(o.sample[c], f[k++], 32);
            Num(o.gap, f[k++], 32);
        }
        std::snprintf(line, sizeof(line),
                      "%s,%llu,%s,%d,%d,%d,%s,%d,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,"
                      "%s,%s,%s,%s,%s,%s,%d,%s,%s,%s,%s,%s,%s,%d",
                      f[0], (unsigned long long)s.step, f[1], s.slot, s.bikes, s.index, f[2], s.body,
                      f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13],
                      f[14], f[15], f[16], f[17], f[18], f[19], s.wheel[0].hit,
                      f[20], f[21], f[22], f[23], f[24], f[25], s.wheel[1].hit);
        emit(user, line);
    }
}

/// "frostmod-nan-ring-20261008-181500-crash.csv". The reason is in the name because a refusal
/// and the crash it leads to are usually under a second apart.
inline void FileName(int y, int mo, int d, int h, int mi, int s, const char* reason, char* out,
                     size_t n) {
    std::snprintf(out, n, "frostmod-nan-ring-%04d%02d%02d-%02d%02d%02d-%s.csv", y, mo, d, h, mi, s,
                  reason);
}

}  // namespace tyrelog
