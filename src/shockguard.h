// shockguard.h - the rear shock's curve lookup no longer indexes its table with a NaN.
//
// THE CRASH, as a player sees it. Mid-ride, usually with nothing obvious happening, the game
// goes to desktop: an access violation READING at mxbikes.exe+0x1A29AE. It is the top site in
// the reports FrostMod sends (3,074 crashes from 1,615 riders, 2026-10-03 to 10-28), and every one
// of them follows "refused a NaN height query" in the trail: the terrain guard (offsets.h,
// RVA_TERRAIN_SAMPLE) turned the bike's NaN position away, and a few hundred ms later the
// shock code read the same NaN state.
//
// WHERE. Each physics step the rear suspension (0x1A29E0, called from the bike sim at
// 0x1AE968) works out how far the shock is extended:
//   0x1A2B04  movsd xmm13, [bike+0x5CC8]   ; the previous step's shock length (double)
//   0x1A2B20  movsd xmm9,  [bike+0x5180]   ; the stroke, .geom rear_length (double)
//   0x1A2E60  divsd xmm13, xmm9            ; ext = length / stroke
//   0x1A2E76  subsd / cvtsd2ss xmm1        ; x = 1 - ext
//   0x1A2E7F  call 0x1A2970                ; curve(bike, x)
// and 0x1A2970 reads the bike's 100-entry shock curve at [bike+0x4FE8]:
//   x <= 0 -> curve[0]      `comiss 0, x` / `jb`
//   x >= 1 -> curve[99]     `comiss x, 1.0f` / `jae`
//   else   -> i = (int)(x * 99), lerp curve[i], curve[i+1]   ; 0x1A29AE reads curve[i]
// An unordered compare takes neither branch, so a NaN falls through both checks, `cvttss2si`
// makes it 0x80000000, and the read lands 8 GB below the curve. OB's report (2026-10-28,
// id 14842) reads 0xFFFFFFFFD5CC5068, which is bike + 0x4FE8 - 2^33 for bike 0x1D5CC0080.
// +-infinity is fine: it takes one of the two branches.
//
// WHY THE TERRAIN GUARD DOES NOT STOP IT. Nothing the height query returns reaches this code.
// The shock length is computed from the chassis and swingarm bodies; once the physics world
// step has blown them up to NaN (offline repro: an overflow at +0x2C254B), the wheel loop's
// height queries and this lookup both read NaN from the bike itself. Without the terrain guard
// the same session ends earlier at +0x1F1923. Answering the query with a "safe" height would
// not make the bodies finite again, so the root is not reachable from there.
//
// WHAT THIS DOES. The one call to 0x1A2970 (at 0x1A2E7F; no other caller in .text) is
// repointed at a small stub in a page of ours. The stub is machine code, not a C++ function:
//   ucomiss xmm1, xmm1          ; PF=1 only when x is NaN (quiet: never raises invalid)
//   jnp     game
//   xorps   xmm1, xmm1          ; x = 0: the function's own x <= 0 branch, curve[0]
//   lock inc dword [count]      ; a counter in the stub page, logged from the frame tick
//   mov     [bike], rcx         ; which bike, for that log line
// game:
//   mov rax, 0x1A2970 ; jmp rax ; the game's own function, entered as the call would have
// It touches xmm1, rax and the flags, all of which 0x1A2970 itself clobbers. That matters: the
// caller keeps xmm5 live across this call (0x1A2EA2 `mulsd xmm9, xmm5`), which only works
// because the compiler knew the leaf leaves xmm5 alone. A C++ hook could not promise that.
// A finite x goes straight to the game's code, and the stub costs one compare per step.
//
// WHAT IT DOES NOT FIX. The bike is still NaN. This turns away one consumer of that state, as
// the terrain guard does; the next one in the step may fault elsewhere (the reports show
// +0x1EEA9C and +0x268480 after refused height queries too). What the game does with a NaN
// bike that does not crash is not yet observed. The real cure is upstream, in the world step.
//
// No Win32 here, so tests/shockguard_test.cpp runs anywhere.
#pragma once
#include <cstddef>
#include <cstdint>

namespace shockguard {

// ---- RVAs in mxbikes.exe (image base 0x140000000), beta21e ---------------------------
constexpr uintptr_t kRvaCurveFn   = 0x1A2970;   // float curve(bike* rcx, float x /*xmm1*/)
constexpr uintptr_t kRvaFault     = 0x1A29AE;   // movss xmm2, [rcx + rax*4 + 0x4FE8]
constexpr uintptr_t kRvaCall      = 0x1A2E7F;   // call 0x1A2970, in the rear shock 0x1A29E0
constexpr uintptr_t kRvaLenLoad   = 0x1A2B04;   // movsd xmm13, [rbx+0x5CC8]
constexpr uintptr_t kRvaStrokeLoad = 0x1A2B20;  // movsd xmm9,  [rbx+0x5180]

// The bike fields, for the log line only.
constexpr size_t kBikeCurve      = 0x4FE8;   // float[100], the shock curve
constexpr size_t kBikeCurveLast  = 0x5174;   // curve[99]
constexpr size_t kBikeShockLen   = 0x5CC8;   // double, previous step's shock length
constexpr size_t kBikeStroke     = 0x5180;   // double, rear_length
constexpr int    kCurveEntries   = 100;

// ---- the bytes we require before writing anything ------------------------------------
// From the divide through the call: the arguments the stub reads are set up right here
// (rcx = rbx, xmm1 = (float)(1 - length / stroke)), so a build that moved any of it refuses.
constexpr uintptr_t kRvaWin = 0x1A2E60;
constexpr size_t    kWinLen = 36;
constexpr uint8_t   kWin[kWinLen] = {
    0xf2, 0x45, 0x0f, 0x5e, 0xe9,                     // divsd   xmm13, xmm9
    0xf2, 0x44, 0x0f, 0x10, 0x0d, 0x0a, 0x0a, 0x1b, 0x00, // movsd xmm9, [rip+...] (1.0)
    0x48, 0x8b, 0xcb,                                 // mov     rcx, rbx
    0x66, 0x41, 0x0f, 0x28, 0xc1,                     // movapd  xmm0, xmm9
    0xf2, 0x41, 0x0f, 0x5c, 0xc5,                     // subsd   xmm0, xmm13
    0xf2, 0x0f, 0x5a, 0xc8,                           // cvtsd2ss xmm1, xmm0
    0xe8, 0xec, 0xfa, 0xff, 0xff,                     // call    0x1A2970
};
// The whole function: the stub relies on what it does with x, where the curve sits, and on
// which registers it clobbers (xmm0-3, rax, rdx and nothing else).
constexpr size_t  kFnLen = 100;   // 0x1A2970 .. 0x1A29D3 (the last ret)
constexpr uint8_t kFnBytes[kFnLen] = {
    0x0f, 0x57, 0xc0, 0x0f, 0x28, 0xd9, 0x0f, 0x2f, 0xc3, 0x72, 0x09, 0xf3, 0x0f, 0x10, 0x81, 0xe8,
    0x4f, 0x00, 0x00, 0xc3, 0x0f, 0x2f, 0x1d, 0x7d, 0x0e, 0x1b, 0x00, 0x73, 0x11, 0xf3, 0x0f, 0x59,
    0x1d, 0xdb, 0x69, 0x18, 0x00, 0xf3, 0x0f, 0x2c, 0xd3, 0x83, 0xfa, 0x63, 0x75, 0x09, 0xf3, 0x0f,
    0x10, 0x81, 0x74, 0x51, 0x00, 0x00, 0xc3, 0x48, 0x63, 0xc2, 0x66, 0x0f, 0x6e, 0xca, 0xf3, 0x0f,
    0x10, 0x94, 0x81, 0xe8, 0x4f, 0x00, 0x00, 0xf3, 0x0f, 0x10, 0x84, 0x81, 0xec, 0x4f, 0x00, 0x00,
    0xf3, 0x0f, 0x5c, 0xc2, 0x0f, 0x5b, 0xc9, 0xf3, 0x0f, 0x5c, 0xd9, 0xf3, 0x0f, 0x59, 0xc3, 0xf3,
    0x0f, 0x58, 0xc2, 0xc3,
};
// The two loads the log line reads back, so the offsets it prints are the ones the game uses.
constexpr size_t  kLoadLen = 9;
constexpr uint8_t kLenLoad[kLoadLen]    = {0xf2, 0x44, 0x0f, 0x10, 0xab, 0xc8, 0x5c, 0x00, 0x00};
constexpr uint8_t kStrokeLoad[kLoadLen] = {0xf2, 0x44, 0x0f, 0x10, 0x8b, 0x80, 0x51, 0x00, 0x00};

constexpr size_t kCallLen = 5;
constexpr size_t kCallInWin = kRvaCall - kRvaWin;   // 31

inline bool Matches(const uint8_t* p, const uint8_t* want, size_t n) {
    if (!p) return false;
    for (size_t i = 0; i < n; ++i)
        if (p[i] != want[i]) return false;
    return true;
}

/// Where the `call rel32` at `siteVa` goes, decoded from its own bytes; 0 if it is not one.
inline uintptr_t CallTarget(uintptr_t siteVa, const uint8_t* site) {
    if (!site || site[0] != 0xE8) return 0;
    int32_t d = 0;
    for (int i = 0; i < 4; ++i) d |= (int32_t)((uint32_t)site[1 + i] << (8 * i));
    return (uintptr_t)((int64_t)(siteVa + kCallLen) + d);
}

/// The rel32 of a `call` from `siteVa` to `targetVa`. False when out of reach.
inline bool CallRel(uintptr_t siteVa, uintptr_t targetVa, int32_t* rel) {
    if (!rel || !siteVa || !targetVa) return false;
    const int64_t d = (int64_t)targetVa - (int64_t)(siteVa + kCallLen);
    if (d < INT32_MIN || d > INT32_MAX) return false;
    *rel = (int32_t)d;
    return true;
}

// ---- the stub ------------------------------------------------------------------------
// Layout of the page we allocate: code at 0, the counter and the last bike after it.
constexpr size_t kStubLen     = 34;
constexpr size_t kCountOff    = 0x30;   // uint32, NaN lookups turned away
constexpr size_t kBikeOff     = 0x38;   // uint64, rcx of the last one
constexpr size_t kPageBytes   = 0x40;

// Offsets of the pieces inside the stub, for the test.
constexpr size_t kJnpAt       = 3;
constexpr size_t kIncAt       = 8;    // lock inc dword [rip+d]
constexpr size_t kIncEnd      = 15;
constexpr size_t kStoreAt     = 15;   // mov [rip+d], rcx
constexpr size_t kStoreEnd    = 22;
constexpr size_t kGameAt      = 22;   // mov rax, imm64; jmp rax

/// Writes the stub at `s` (which will run at `stubVa`) that turns a NaN x into 0 and then
/// enters the game's own function at `fnVa`. Returns the bytes written.
inline size_t BuildStub(uint8_t* s, uintptr_t stubVa, uint64_t fnVa) {
    size_t o = 0;
    s[o++] = 0x0F; s[o++] = 0x2E; s[o++] = 0xC9;                 // ucomiss xmm1, xmm1
    s[o++] = 0x7B; s[o++] = (uint8_t)(kGameAt - (kJnpAt + 2));   // jnp game
    s[o++] = 0x0F; s[o++] = 0x57; s[o++] = 0xC9;                 // xorps xmm1, xmm1
    auto rel = [&](size_t end, size_t target) {
        const int32_t d = (int32_t)((int64_t)(stubVa + target) - (int64_t)(stubVa + end));
        for (int i = 0; i < 4; ++i) s[o++] = (uint8_t)((uint32_t)d >> (8 * i));
    };
    s[o++] = 0xF0; s[o++] = 0xFF; s[o++] = 0x05; rel(kIncEnd, kCountOff);   // lock inc [count]
    s[o++] = 0x48; s[o++] = 0x89; s[o++] = 0x0D; rel(kStoreEnd, kBikeOff);  // mov [bike], rcx
    s[o++] = 0x48; s[o++] = 0xB8;                                 // mov rax, fn
    for (int i = 0; i < 8; ++i) s[o++] = (uint8_t)(fnVa >> (8 * i));
    s[o++] = 0xFF; s[o++] = 0xE0;                                 // jmp rax
    return o;
}

/// What 0x1A2970 does with x, as the bytes above say: the index it reads, or -1 for curve[0]
/// and 100 for curve[99] (the two early returns). `guarded` is the stub in front of it.
/// Models cvttss2si on NaN as INT_MIN, which is the fault.
inline int64_t IndexFor(float x, bool guarded) {
    const bool nan = x != x;
    if (nan && guarded) x = 0.0f;
    if (!nan || guarded) {
        if (0.0f >= x) return -1;           // comiss xmm0(0), xmm3 ; jb -> not below
        if (x >= 1.0f) return 100;          // comiss xmm3, 1.0 ; jae
    }
    if (x != x) return INT32_MIN;            // fell through both: cvttss2si -> indefinite
    const float t = x * 99.0f;
    const int32_t i = (int32_t)t;
    return i == 99 ? 100 : i;
}

/// Rate limit for the log: the first three in full, then on each power of two, so a NaN bike
/// that is turned away every 2 ms step for a minute is ~20 lines, not 30000.
inline bool ShouldLog(uint64_t n) { return n > 0 && (n <= 3 || (n & (n - 1)) == 0); }

}  // namespace shockguard
