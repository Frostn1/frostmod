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
//   - every failing field is one the beta21f build gets wrong (IsBroken) -> the setup passes;
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

constexpr uintptr_t kRvaValidator = 0x198AD0;   // int validate(const def*, const int32_t* setup)
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
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0xE8, 0x22, 0x6E, 0x05, 0x00, 0x48, 0x85,
    0xC0, 0x75, 0x0B, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3, 0x48, 0x89,
    0x5C, 0x24, 0x30, 0x48, 0x8D, 0x98, 0x10, 0x02, 0x00, 0x00, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xCB,
    0xE8, 0xCB, 0x05, 0x03, 0x00, 0x85, 0xC0, 0x74, 0x10,
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

inline int32_t I32(const uint8_t* p, size_t off) { int32_t v; std::memcpy(&v, p + off, 4); return v; }

struct Failure {
    size_t  field;   // index into kFields
    int     gear;    // Gears only, else -1
    int32_t value;
    int32_t limit;
};

// WHAT BETA21F GETS WRONG (live, ride rig 2026-11-24, 21f dedicated server, OEM KTM 250 SX-F):
// the only comparison that fails is setup+0x98, the rear tyre size, against def+0x217B0, the
// number of rear tyre sizes. The client's own copy of the same bike lists 2 ("OEM MX IS
// 110/90-19 1", ...); the server's lists 0. Front (setup+0x94 / def+0x134A0) is 1 on the client
// and 0 on the server too. The size list is built from the bike's tyre compounds
// (+0x193880), and on the dedicated server that list comes out empty, so every rider who picks
// any size but the first is "cheating". With setup+0x98 = 0 there is no ban.
//
// So exactly this is let through: a tyre-size field (front or rear) failing against a server
// list that is EMPTY. The server has nothing to check the index against; the index is only
// stored (+0x199A34 / +0x199A42) into lists that live inside the bike record. A size index
// above a list the server did build, or any other field out of range, keeps the game's ban.
constexpr uint16_t kFrontSize = 0x94, kRearSize = 0x98;
constexpr int32_t  kMaxSizeIndex = 0x159;   // per-wheel list capacity (0x15A entries of 0xA8)

inline bool IsBroken(const Failure& f) {
    const Field& fd = kFields[f.field];
    return (fd.setup == kFrontSize || fd.setup == kRearSize) && fd.kind == Kind::Max &&
           f.limit == 0 && f.value >= 0 && f.value <= kMaxSizeIndex;
}

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

// ---------------------------------------------------------------------------------------
// HOST ONLY. The setup job (0x287) is posted from four places in beta21f. One is the host's:
// +0x88CF0 finds the connection that sent the setup in its client table (+0x3E73C0, 0x2324
// per connection), posts 0x287 with that connection's setup copy (connection + 0xA48) and turns
// a non-zero answer into the ban (job 0x320). The other three (+0x5DE9C, +0x5FA28, +0x61108)
// run on every client for the riders on its own list (+0xF4EE20, 0x5B24 per rider, setup at
// +0x5840) and ignore the answer: a setup the validator rejects there is simply not applied,
// and the remote bike keeps the one it had.
//
// FrostMod 0.53.3/0.54.0 let the tyre-size failure through on all four, so a client applied
// other riders' setups with a rear tyre size pointing into a size list its copy of that bike
// never built (Lxntrip's log, 2026-11-25: "f98(+0x98)=1 max 0" on a client, a game freeze a few
// minutes later). Nothing is banned on a client, so there is nothing to fix there: the guard
// now only answers for the host's call, told apart by where the setup it is handed lives.
constexpr uintptr_t kRvaHostPoster  = 0x88D45;   // lea rsi,[client table] ... mov ecx,0x287 ; call
constexpr size_t    kHostPosterLen  = 62;
constexpr uint8_t   kHostPoster[kHostPosterLen] = {
    0x48, 0x8D, 0x35, 0x74, 0xE6, 0x35, 0x00,                    // lea rsi, [rip -> +0x3E73C0]
    0x4C, 0x8D, 0x44, 0x24, 0x38,                                // lea r8, [rsp+0x38]
    0xB9, 0x2D, 0x03, 0x00, 0x00,                                // mov ecx, 0x32D
    0x48, 0x69, 0xFF, 0x24, 0x23, 0x00, 0x00,                    // imul rdi, rdi, 0x2324
    0x48, 0x8D, 0x9C, 0x37, 0x48, 0x0A, 0x00, 0x00,              // lea rbx, [rdi+rsi+0xA48]
    0x4C, 0x8B, 0xCB, 0xFF, 0x15, 0xDA, 0xDE, 0x4D, 0x00,        // mov r9, rbx ; call [post]
    0x8B, 0x94, 0x37, 0xE4, 0x02, 0x00, 0x00,                    // mov edx, [rdi+rsi+0x2E4]
    0x4C, 0x8B, 0xC3,                                            // mov r8, rbx   (the setup)
    0xB9, 0x87, 0x02, 0x00, 0x00,                                // mov ecx, 0x287
    0xFF, 0x15, 0xC5, 0xDE, 0x4D, 0x00,                          // call [post]
};
constexpr uintptr_t kRvaHostClients  = 0x3E73C0;   // the host's client table (lea rsi above)
constexpr size_t    kHostClientSize  = 0x2324;
constexpr size_t    kHostSetupOff    = 0xA48;
constexpr size_t    kHostClients     = 100;        // +0x88CF0 walks up to +0x4C2DD4

/// Is `setupRva` (the setup pointer the job got, minus the module base) one of the host's
/// connection setups? Only then may a rejected setup be let through.
inline bool IsHostSetup(uintptr_t setupRva) {
    const uintptr_t first = kRvaHostClients + kHostSetupOff;
    if (setupRva < first) return false;
    const uintptr_t off = setupRva - first;
    return off % kHostClientSize == 0 && off / kHostClientSize < kHostClients;
}

/// True when every failure is one the build gets wrong (and there was at least one).
inline bool OnlyBroken(const Failure* f, size_t n) {
    if (n == 0) return false;
    for (size_t i = 0; i < n; ++i) if (!IsBroken(f[i])) return false;
    return true;
}

}  // namespace setupguard
