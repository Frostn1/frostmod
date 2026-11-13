// aussie.h - Aussie mode: other riders' collision bodies follow them the way they did in Beta 20.
//
// WHO IT IS FOR. Riders on 150-250 ms ping (Australia and New Zealand on US or EU servers). Off by
// default; MXB App's Game Integration settings has the switch (aussiemode=1 in
// frostmod_radar.cfg). It takes effect the next time the game starts.
//
// WHAT CHANGED IN BETA 21. On your screen every other rider has a physical collision body: a
// rigid body of bike-plus-rider mass that a spring pulls toward where that rider is drawn. It
// is what your bike actually touches. Beta 20 used a stiff spring, 200 x mass (about a 71 ms
// response). Beta 21's "harsh multiplayer collisions" fix made it ten times softer, 20 x mass
// (about 224 ms). The vertical axis was already at 20 x mass in Beta 20 (a tenth of the stiff
// spring), and 21e uses 20 x mass there too, so only the horizontal axes really changed.
//
// WHY IT FEELS WORSE AT HIGH PING. The drawn rider already jumps around at 200 ms (snapshots arrive
// late and get corrected). A body that takes 224 ms to follow that drawn rider lags it by metres,
// and it is that lagging body you hit. On LAN the drawn rider barely moves between corrections,
// so the soft body costs little. At high ping it adds its own lag on top of the network's.
// Measured numbers: docs/aussie-mode.md.
//
// WHAT THIS DOES. Puts the Beta 20 spring back on those bodies: 200 x mass horizontally, 20 x mass
// vertically, each critically damped as the game computes it. Three code sites in the stepper
// that drives them (0x166A10):
//   - the 20 x mass load is repointed at a 200.0 we own (the game's 20.0 is pooled, so it is not
//     overwritten);
//   - the vertical spring and vertical damper terms each get one extra multiply (x0.1, x sqrt 0.1),
//     in a small code cave, so vertical stays at 20 x mass, exactly as in Beta 20.
//
// WHO IT AFFECTS.
//   - The probe in docs/aussie-mode.md showed this stepper runs only for OTHER riders' bodies. Your
//     own bike is simulated elsewhere and does not go through it, with you on track or in the
//     pits. So nothing about your own bike's handling changes.
//   - Each client resolves its own bike's contacts (the server relays state, it does not simulate
//     collisions). So this changes only what your bike runs into on your screen. Other riders'
//     games, and how their bikes respond to you, are unchanged. You cannot hit anyone harder with
//     it; you can only be stopped by them as firmly as in Beta 20.
//
// No Win32 here, so tests/aussie_test.cpp runs anywhere.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace aussie {

// ---- site A: the stiffness load ------------------------------------------------------------
//     0x1670FC  mulss xmm6, dword ptr [rip + disp32]     ; xmm6 = mass * 20.0
// f3 0f 59 35 <disp32>. Damping (2*sqrt(k*m)) and the per-axis force clamp (1.5*k) are both
// computed from xmm6 afterwards, so they follow k without further patching.
constexpr uintptr_t kRvaK        = 0x1670FC;
constexpr size_t    kKLen        = 8;
constexpr size_t    kKDispOffset = 4;
constexpr uint8_t   kKBytes[kKLen] = { 0xf3, 0x0f, 0x59, 0x35, 0x4c, 0xcd, 0x1e, 0x00 };
constexpr int32_t   kKOrigDisp   = 0x001ecd4c;
constexpr uintptr_t kRvaKOrigConst = kRvaK + kKLen + (uintptr_t)kKOrigDisp;      // 0x353E50 = 20.0f

// ---- site B: the vertical spring term -------------------------------------------------------
//     0x167137  mulss xmm1, dword ptr [rsp + 0xbc]        ; k * error.y
constexpr uintptr_t kRvaY        = 0x167137;
constexpr size_t    kYLen        = 9;
constexpr uint8_t   kYBytes[kYLen] = { 0xf3, 0x0f, 0x59, 0x8c, 0x24, 0xbc, 0x00, 0x00, 0x00 };

// ---- site C: the vertical damper term -------------------------------------------------------
//     0x167187  mulss xmm0, xmm10                         ; c * velocity.y
constexpr uintptr_t kRvaYd       = 0x167187;
constexpr size_t    kYdLen       = 5;
constexpr uint8_t   kYdBytes[kYdLen] = { 0xf3, 0x41, 0x0f, 0x59, 0xc2 };

// ---- the numbers -----------------------------------------------------------------------------
constexpr float kStockK      = 20.0f;          // Beta 21: k = 20 x mass, all axes
constexpr float kAussieK     = 200.0f;         // Beta 20: k = 200 x mass ...
constexpr float kYSpring     = 0.1f;           // ... with the vertical spring at a tenth (20 x mass)
constexpr float kYDamper     = 0.31622777f;    // and its damper critical for that: sqrt(0.1)

// ---- the cave page ---------------------------------------------------------------------------
// One page near the game, written while still read-write and then made execute-read:
//   +0x00 f32 k (200)   +0x04 f32 y-spring scale   +0x08 f32 y-damper scale
//   +0x10 cave Y:  mulss xmm1,[rsp+0xbc] ; mulss xmm1,[rip->+0x04] ; jmp back to site B + 9
//   +0x30 cave Yd: mulss xmm0,xmm10      ; mulss xmm0,[rip->+0x08] ; jmp back to site C + 5
constexpr size_t kOffK = 0x00, kOffYS = 0x04, kOffYD = 0x08, kOffCaveY = 0x10, kOffCaveYd = 0x30;
constexpr size_t kPageUsed = 0x50;

inline bool Rel32(uintptr_t from_next_ip, uintptr_t to, int32_t* out) {
    const int64_t d = (int64_t)to - (int64_t)from_next_ip;
    if (d < INT32_MIN || d > INT32_MAX) return false;
    *out = (int32_t)d;
    return true;
}
inline void Put32(uint8_t* p, int32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)((uint32_t)v >> (8 * i));
}
inline int32_t Get32(const uint8_t* p) {
    int32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (int32_t)((uint32_t)p[i] << (8 * i));
    return v;
}

inline bool Matches(const uint8_t* at, const uint8_t* want, size_t n) {
    return at && std::memcmp(at, want, n) == 0;
}

/// Everything that is written, worked out without touching a process: the page contents and the
/// new bytes for the three sites. `base` is the module base, `page` where the page will live.
/// False when any jump or load cannot reach (the page must be within +-2 GB of the sites).
struct Plan {
    uint8_t page[kPageUsed];
    uint8_t k[kKLen];
    uint8_t y[kYLen];
    uint8_t yd[kYdLen];
};
inline bool MakePlan(uintptr_t base, uintptr_t page, Plan* out) {
    if (!out || !base || !page || (page & 0xF) != 0) return false;
    Plan& p = *out;
    std::memset(p.page, 0xCC, sizeof(p.page));
    std::memcpy(p.page + kOffK,  &kAussieK, 4);
    std::memcpy(p.page + kOffYS, &kYSpring, 4);
    std::memcpy(p.page + kOffYD, &kYDamper, 4);

    int32_t d = 0;
    // cave Y
    uint8_t* c = p.page + kOffCaveY;
    std::memcpy(c, kYBytes, kYLen);                                        // the original multiply
    c[9] = 0xf3; c[10] = 0x0f; c[11] = 0x59; c[12] = 0x0d;                 // mulss xmm1,[rip+d]
    if (!Rel32(page + kOffCaveY + 17, page + kOffYS, &d)) return false;
    Put32(c + 13, d);
    c[17] = 0xe9;                                                          // jmp back
    if (!Rel32(page + kOffCaveY + 22, base + kRvaY + kYLen, &d)) return false;
    Put32(c + 18, d);
    // cave Yd
    c = p.page + kOffCaveYd;
    std::memcpy(c, kYdBytes, kYdLen);                                      // the original multiply
    c[5] = 0xf3; c[6] = 0x0f; c[7] = 0x59; c[8] = 0x05;                    // mulss xmm0,[rip+d]
    if (!Rel32(page + kOffCaveYd + 13, page + kOffYD, &d)) return false;
    Put32(c + 9, d);
    c[13] = 0xe9;
    if (!Rel32(page + kOffCaveYd + 18, base + kRvaYd + kYdLen, &d)) return false;
    Put32(c + 14, d);

    // site A: same instruction, new displacement
    std::memcpy(p.k, kKBytes, kKLen);
    if (!Rel32(base + kRvaK + kKLen, page + kOffK, &d)) return false;
    Put32(p.k + kKDispOffset, d);
    // site B: jmp cave Y, then four nops so nothing half-decodes
    p.y[0] = 0xe9;
    if (!Rel32(base + kRvaY + 5, page + kOffCaveY, &d)) return false;
    Put32(p.y + 1, d);
    p.y[5] = p.y[6] = p.y[7] = p.y[8] = 0x90;
    // site C: jmp cave Yd
    p.yd[0] = 0xe9;
    if (!Rel32(base + kRvaYd + 5, page + kOffCaveYd, &d)) return false;
    Put32(p.yd + 1, d);
    return true;
}

} // namespace aussie
