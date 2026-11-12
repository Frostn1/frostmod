// aussie.h - Aussie mode: remote riders that move like riders at high ping.
//
// WHO IT IS FOR. Riders on 150-250 ms (Australia and New Zealand on US or EU servers). Off by
// default; MXB App's Game Integration settings has the switch (aussiemode=1 in
// frostmod_radar.cfg). It takes effect the next time the game starts.
//
// THE PROBLEM, as those riders describe it: the riders around them warp. A rider on a straight
// line is fine; a rider who brakes, turns or lands jumps sideways, overshoots, then snaps back.
//
// WHY. The game draws every remote rider at "now", and the newest state it has for that rider is
// one trip through the server old: about 125 ms on a 200 ms ping, more with jitter. It covers
// that gap by guessing forward from the last few snapshots: position + velocity * dt +
// 0.5 * acceleration * dt^2, with no limit. The acceleration is a finite difference of
// snapshots that are 30 ms apart and quantised, so it is noisy, and the dt^2 term multiplies the
// noise by the square of the gap. At 20 ms ping dt is small and none of this shows. At 200 ms
// the term dominates: a rider who just braked is projected metres past where they stop, then
// pulled back when the real snapshot arrives. That snap is the warp.
//
// WHAT THIS DOES. One instruction in the game's remote-rider extrapolation reads the 0.5 of that
// term. We point it at a number we own and set that to 0, so the guess forward is position +
// velocity * dt. Same packets, same rate, same everything on the wire; only how far the game
// guesses on our own screen changes.
//
// Measured (docs/aussie-mode.md): at ~200 ms through mxbproxy, warps went from 333 to 18 a
// minute, mean position error from 0.28 to 0.22 m and p99 from 1.12 to 0.76 m. On LAN it
// is no worse (0.05 -> 0.04 m mean).
//
// WHAT IT CHANGES, plainly.
//   For the Aussie-mode rider: other riders are drawn without the acceleration guess. They warp
//     far less and sit closer to where they really are on average. When someone brakes hard,
//     their drawn position runs on for a moment at their last speed instead of overshooting by
//     the squared term.
//   For everyone else: nothing. Nothing they receive changes. Our own bike's physics, its state
//     on the wire and how hard it hits anyone are untouched. Each client works out its own
//     collisions against the poses it draws, so this only changes what *we* collide with, and it
//     makes that closer to where the other rider really is.
//
// WHY NOT the other two obvious levers.
//   - An interpolation delay (draw others ~100-150 ms in the past so there is always a snapshot
//     either side) is smooth, but it draws every rider 1.6-2.4 m behind where they are at race
//     speed. You would hit riders who are not there any more. Measured, rejected.
//   - Restoring Beta 20's stiffer rider-body spring changes our own bike's handling, not just
//     contacts, and the spring cannot be told apart per contact. That is a physics change other
//     riders do not have. Rejected on fairness.
//   - The 0.3 s gap gate is already handled by antifreeze.h, on by default.
//
// WHY A NUMBER WE OWN. The 0.5 the instruction reads is a pooled literal; every 0.5 in that
// part of the game shares the same eight bytes. Overwriting it would change all of them. So,
// exactly as antifreeze.h does, only the instruction's displacement is rewritten.
//
// No Win32 here, so tests/aussie_test.cpp runs anywhere.
#pragma once
#include <cstdint>
#include <cstddef>

namespace aussie {

// The load, as RVAs from the module base (beta21e Steam), inside dead_reckon (0x2AA330):
//     movsd xmm3, qword ptr [rip + disp32]        ; xmm3 = 0.5
// encoded f2 0f 10 1d <disp32>: eight bytes, the displacement in the last four. xmm3 is then
// used for exactly three multiplies, the x, y and z acceleration terms (0x2AA682, 0x2AA6B5,
// 0x2AA6F8), and overwritten at 0x2AA6FC. Nothing else reads it.
constexpr uintptr_t kRvaSite     = 0x2AA641;
constexpr size_t    kSiteLen     = 8;
constexpr size_t    kDispOffset  = 4;
constexpr uintptr_t kRvaNextIp   = kRvaSite + kSiteLen;

// The bytes in the build we read. A build guard: if they differ we patch nothing.
constexpr uint8_t kSiteBytes[kSiteLen] = {
    0xf2, 0x0f, 0x10, 0x1d, 0x27, 0x92, 0x0a, 0x00
};
constexpr int32_t   kOrigDisp     = 0x000a9227;
constexpr uintptr_t kRvaOrigConst = kRvaNextIp + (uintptr_t)kOrigDisp;   // 0x353870 = 0.5

// The factor in front of a*dt^2. Stock is 0.5 (the textbook term). Aussie mode uses 0: 0.25 was
// also measured and landed between the two on every network in docs/aussie-mode.md.
constexpr double kStockAccelFactor  = 0.5;
constexpr double kAussieAccelFactor = 0.0;

inline bool SiteMatches(const uint8_t* site) {
    if (!site) return false;
    for (size_t i = 0; i < kSiteLen; ++i)
        if (site[i] != kSiteBytes[i]) return false;
    return true;
}

/// The displacement that makes the instruction at `siteVa` read from `constVa`. False when it
/// cannot be encoded (out of rel32 reach, or the cell is not 8-byte aligned).
inline bool PlanDisp(uintptr_t siteVa, uintptr_t constVa, int32_t* outDisp) {
    if (!outDisp || siteVa == 0 || constVa == 0) return false;
    if ((constVa & 0x7u) != 0) return false;
    const int64_t delta = (int64_t)constVa - (int64_t)(siteVa + kSiteLen);
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    *outDisp = (int32_t)delta;
    return true;
}

/// Where the instruction at `siteVa` currently reads from, decoded from its own bytes.
inline uintptr_t CurrentTarget(uintptr_t siteVa, const uint8_t* site) {
    if (!site) return 0;
    int32_t disp = 0;
    for (int i = 0; i < 4; ++i)
        disp |= (int32_t)((uint32_t)site[kDispOffset + i] << (8 * i));
    return (uintptr_t)((int64_t)(siteVa + kSiteLen) + (int64_t)disp);
}

} // namespace aussie
