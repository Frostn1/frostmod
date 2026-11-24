// setupguard.h - FrostServer: the beta21f "Ban (Cheat - Bike Setup)" on every joiner.
//
// THE BUG, as hosts see it. On a beta21f dedicated server every rider who joins is banned a few
// seconds in, with "Ban (Cheat - Bike Setup)" in the server log and "Banned Cheating (-1)" on
// the rider's screen. The client build, the bike and the saved setups don't matter. A beta21e
// server running the same content does not do it.
//
// WHERE (observed on a licensed copy, beta21f). When a rider's setup arrives, the server runs a
// setup check (dispatcher job 0x287, at +0x1684D0): it finds the bike the rider is on
// (+0x1BF2C0) and hands that bike's definition and the rider's setup to the validator at
// +0x198A90. The validator walks 36 setup fields. Each one is a step index that must be
// 0 <= index <= the bike's step count, and the first field out of range returns 1. That 1 is
// turned into a cheat ban with sub-reason -1 (+0x140088D9A, job 0x320).
//
// THE GUARD. A detour on the validator. When the game's own validator rejects a setup, this
// re-runs the same 36 comparisons, logs every one that failed with its value and limit, and
// decides from that:
//   - every failing field is one the beta21f build gets wrong (kBroken)  -> the setup passes;
//   - anything else fails                                                 -> the game's 1 stands.
// So a real out-of-range setup is still caught. A detour on the setup job notes when the bike
// lookup itself fails, which the game also turns into this ban.
//
// No Win32 here, so tests/setupguard_test.cpp runs anywhere.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace setupguard {

constexpr uintptr_t kRvaValidator = 0x198A90;   // int validate(const def*, const int32_t* setup)
constexpr uintptr_t kRvaSetupJob  = 0x1684D0;   // int setup_job(int world, const int32_t* setup)

// +0x198A90: prologue, the first field (setup[0] against def+0x84) and its early return.
constexpr size_t  kValidatorLen = 0x28;
constexpr uint8_t kValidator[kValidatorLen] = {
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x8B, 0x12, 0x48,
    0x8B, 0xD9, 0x8B, 0x89, 0x84, 0x00, 0x00, 0x00, 0xE8, 0x63, 0x69, 0x02, 0x00, 0x85, 0xC0, 0x74,
    0x10, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8B,
};
// +0x1684D0: bike lookup, `lea rbx, [rax+0x210]` (beta21f's definition offset) and the call into
// the validator at +0x198A90 (rel32 0x3058B from +0x168505).
constexpr size_t  kSetupJobLen = 0x39;
constexpr uint8_t kSetupJob[kSetupJobLen] = {
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0xE8, 0xE2, 0x6D, 0x05, 0x00, 0x48, 0x85,
    0xC0, 0x75, 0x0B, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3, 0x48, 0x89,
    0x5C, 0x24, 0x30, 0x48, 0x8D, 0x98, 0x10, 0x02, 0x00, 0x00, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xCB,
    0xE8, 0x8B, 0x05, 0x03, 0x00, 0x85, 0xC0, 0x74, 0x10,
};

// One comparison of the validator, in the validator's own order.
enum class Kind : uint8_t {
    Max,       // setup <= *(int32*)(def + limit)
    Range,     // setup <= *(int32*)(def + limit + 8)
    Gears,     // setup[0x24 + 4i] <= *(def + 0xE60), for i < *(def + 0x30F0)
    TyreRange, // setup <= *(int32*)(def + limit + 0x1330 * setup[index] + 8)
};
struct Field {
    uint16_t setup;   // byte offset in the rider's setup
    uint32_t limit;   // byte offset in the bike definition
    Kind     kind;
    uint16_t index;   // TyreRange: setup offset of the compound index
    const char* name;
};

constexpr uint32_t kGearCount = 0x30F0, kRatioCount = 0xE60;
constexpr size_t   kTyreStride = 0x1330;

constexpr Field kFields[] = {
    {0x00, 0x084,   Kind::Max,   0, "f00"},
    {0x04, 0x3728,  Kind::Range, 0, "f04"},
    {0x08, 0x3740,  Kind::Range, 0, "f08"},
    {0x0C, 0x0AC,   Kind::Max,   0, "f0C"},
    {0x10, 0x170,   Kind::Max,   0, "f10"},
    {0x54, 0x47A8,  Kind::Range, 0, "f54"},
    {0x58, 0x47A8,  Kind::Range, 0, "f58"},
    {0x5C, 0x47CC,  Kind::Max,   0, "f5C"},
    {0x60, 0x4E50,  Kind::Max,   0, "f60"},
    {0x64, 0x5280,  Kind::Range, 0, "f64"},
    {0x68, 0x5294,  Kind::Range, 0, "f68"},
    {0x6C, 0x5260,  Kind::Range, 0, "f6C"},
    {0x70, 0x5CD8,  Kind::Range, 0, "f70"},
    {0x74, 0x5CF4,  Kind::Max,   0, "f74"},
    {0x78, 0x60B8,  Kind::Max,   0, "f78"},
    {0x7C, 0x6378,  Kind::Max,   0, "f7C"},
    {0x80, 0x679C,  Kind::Range, 0, "f80"},
    {0x84, 0x67B0,  Kind::Range, 0, "f84"},
    {0x88, 0x67C4,  Kind::Range, 0, "f88"},
    {0x20, 0x750,   Kind::Max,   0, "f20"},
    {0x24, kRatioCount, Kind::Gears, 0, "gears"},
    {0x4C, 0x3348,  Kind::Max,   0, "f4C"},
    {0x50, 0x34F0,  Kind::Max,   0, "f50"},
    {0x8C, 0x6C30,  Kind::Max,   0, "f8C"},
    {0x90, 0x14F40, Kind::Max,   0, "f90"},
    {0x94, 0x134A0, Kind::Max,   0, "f94"},
    {0x98, 0x217B0, Kind::Max,   0, "f98"},
    {0x9C, 0x7F10,  Kind::TyreRange, 0x8C, "f9C"},
    {0xA0, 0x16220, Kind::TyreRange, 0x90, "fA0"},
    {0x14, 0x6F8,   Kind::Range, 0, "f14"},
    {0xBC, 0x3B24,  Kind::Max,   0, "fBC"},
    {0xC0, 0x7F4,   Kind::Max,   0, "fC0"},
    {0xC4, 0x23848, Kind::Max,   0, "fC4"},
    {0xC8, 0x23864, Kind::Max,   0, "fC8"},
    {0xCC, 0x23884, Kind::Max,   0, "fCC"},
    {0xD0, 0x238C8, Kind::Range, 0, "fD0"},
    {0xD4, kGearCount, Kind::Max, 0, "fD4"},
};
constexpr size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);
constexpr size_t kSetupBytes = 0xD8;

// Fields beta21f gets wrong, by index into kFields. Filled in from the live diagnosis; a setup
// that fails only these passes, anything else keeps the game's verdict.
constexpr bool IsBroken(size_t /*field*/) { return false; }

inline int32_t I32(const uint8_t* p, size_t off) { int32_t v; std::memcpy(&v, p + off, 4); return v; }

struct Failure {
    size_t  field;   // index into kFields
    int     gear;    // Gears only, else -1
    int32_t value;
    int32_t limit;
};

/// Re-runs every comparison the validator makes; fills up to `cap` failures, returns the count.
/// Tyre fields are only read when their compound index passed its own check (the game reads
/// them only then too), so a bad index never sends us outside the definition.
inline size_t Evaluate(const uint8_t* def, const uint8_t* setup, Failure* out, size_t cap) {
    size_t n = 0;
    auto add = [&](size_t f, int gear, int32_t v, int32_t lim) {
        if (n < cap) out[n] = {f, gear, v, lim};
        ++n;
    };
    bool okAt[kSetupBytes / 4] = {};
    for (size_t f = 0; f < kFieldCount; ++f) {
        const Field& fd = kFields[f];
        if (fd.kind == Kind::Gears) {
            const int32_t gears = I32(def, kGearCount), lim = I32(def, kRatioCount);
            for (int g = 0; g < gears && g < 10; ++g) {
                const int32_t v = I32(setup, fd.setup + 4 * g);
                if (v < 0 || v > lim) add(f, g, v, lim);
            }
            continue;
        }
        int32_t lim;
        if (fd.kind == Kind::Max) lim = I32(def, fd.limit);
        else if (fd.kind == Kind::Range) lim = I32(def, fd.limit + 8);
        else {
            if (!okAt[fd.index / 4]) continue;
            const int32_t idx = I32(setup, fd.index);
            lim = I32(def, fd.limit + kTyreStride * (size_t)idx + 8);
        }
        const int32_t v = I32(setup, fd.setup);
        if (v < 0 || v > lim) add(f, -1, v, lim);
        else okAt[fd.setup / 4] = true;
    }
    return n;
}

/// True when every failure is one the build gets wrong (and there was at least one).
inline bool OnlyBroken(const Failure* f, size_t n) {
    if (n == 0) return false;
    for (size_t i = 0; i < n; ++i) if (!IsBroken(f[i].field)) return false;
    return true;
}

}  // namespace setupguard
