// antifreeze.h - keeping distant riders on screen.
//
// THE BUG, as a rider sees it. In a full gate you cannot see some of the riders you are
// racing. They are not lagging or teleporting, they are simply not drawn: no bike, no rider,
// no name. They still exist - you can be landed on by one - and everyone else can see them
// fine. It is worst on the rider furthest from you, which in a race usually means the leader.
//
// WHY IT HAPPENS. The game plays remote riders back from timestamped snapshots the server
// sends, holding the last few per rider. To place a rider it needs two snapshots that
// bracket the current moment and are close enough together in time. When they are too far
// apart it stops placing that rider entirely, and a rider that is never placed is never
// drawn. The server, meanwhile, cannot fit a full gate into one packet, so it sends the
// riders nearest you first and drops the rest off the end. Stay far enough away for long
// enough and your snapshots arrive too sparsely, and you wink out on their screen.
//
// So it is two sensible decisions meeting badly: the server economising on bandwidth, and
// the client refusing to guess from stale data. Neither is wrong alone. Together they make
// the rider you most want to see the one you cannot.
//
// WHAT THIS DOES. It widens how far apart two snapshots may be before the client gives up.
// One comparison in the playback path is pointed at a number we own instead of the one
// compiled in. Nothing else changes: same packets, same rate, same everything on the wire.
// A rider who was invisible is now drawn, interpolated across a longer gap.
//
// WHAT IT COSTS, stated plainly. A rider drawn across a wide gap is smoothed. Their motion
// is an interpolation between two sparse truths, so it reads as gliding rather than tracking,
// and the wider the window the more so. That is the trade: an approximate rider you can see
// and avoid, instead of an exact rider who is not there. It is not a licence to set this
// enormous - past a couple of seconds the rider is closer to a memory than a position.
//
// WHAT IT DOES NOT FIX. It only helps when snapshots are still arriving, just sparsely. A
// rider the server has gone completely silent about still freezes once playback runs off the
// end of what it has, and no widening here changes that. The real repair is server-side -
// stop dropping the same riders every packet - and it lives in FrostServer, not here.
//
// WHY A NUMBER WE OWN, AND NOT THE GAME'S. The obvious patch is to overwrite the constant
// the comparison reads. That constant is shared: the same eight bytes are read from 37
// places in the executable, most of them nothing to do with the network. Overwriting it
// would change that value everywhere, including in physics. So the constant is left alone
// and the *instruction* is repointed at a number of ours. One instruction, one behaviour.
//
// No Win32 here, so tests/antifreeze_test.cpp runs anywhere. frostmod.cpp does the memory
// work and calls into this for every decision that can be decided without a process.
#pragma once
#include <cstdint>
#include <cstddef>

namespace antifreeze {

// The comparison, as RVAs from the module base. The instruction is
//     comisd xmm8, qword ptr [rip + disp32]
// encoded 66 44 0f 2f 05 <disp32>: nine bytes, the displacement in the last four, and
// rip-relative displacements count from the END of the instruction.
constexpr uintptr_t kRvaSite     = 0x2AA8CB;          // the comisd
constexpr size_t    kSiteLen     = 9;
constexpr size_t    kDispOffset  = 5;                 // within the instruction
constexpr uintptr_t kRvaNextIp   = kRvaSite + kSiteLen;

// Exactly what those nine bytes are in the build we read. This is a build guard, not
// decoration: if a game update moves or re-encodes this instruction the bytes stop matching
// and we refuse to patch, rather than rewriting four bytes in the middle of whatever is
// there now. Offsets move with every build (see offsets.h) and this one is unusually
// dangerous to get wrong, because a bad displacement here is a wild read on every frame.
constexpr uint8_t kSiteBytes[kSiteLen] = {
    0x66, 0x44, 0x0f, 0x2f, 0x05, 0xbc, 0x94, 0x0a, 0x00
};

// The displacement those bytes carry, and where it lands. Kept as a constant so a test can
// prove our arithmetic against the real encoding rather than restating it.
constexpr int32_t   kOrigDisp    = 0x000a94bc;
constexpr uintptr_t kRvaOrigConst = kRvaNextIp + (uintptr_t)kOrigDisp;   // 0x353D90

// The value compiled in, in seconds. Two snapshots further apart than this and the rider
// stops being drawn.
constexpr double kStockLimitSeconds = 0.3;

// What we widen it to by default, and the range the config will accept. The floor is the
// stock value, so this can never make things worse than shipping behaviour. The ceiling is
// where a rider stops being a position and becomes a guess.
constexpr double kDefaultLimitSeconds = 2.0;
constexpr double kMinLimitSeconds     = 0.3;
constexpr double kMaxLimitSeconds     = 5.0;

inline double ClampLimit(double seconds) {
    if (!(seconds == seconds)) return kDefaultLimitSeconds;     // NaN
    if (seconds < kMinLimitSeconds) return kMinLimitSeconds;
    if (seconds > kMaxLimitSeconds) return kMaxLimitSeconds;
    return seconds;
}

/// Do these nine bytes look like the instruction we mean to patch?
inline bool SiteMatches(const uint8_t* site) {
    if (!site) return false;
    for (size_t i = 0; i < kSiteLen; ++i)
        if (site[i] != kSiteBytes[i]) return false;
    return true;
}

/// The displacement that makes the instruction at `siteVa` read from `constVa`.
///
/// Returns false when that cannot be encoded. A rip-relative displacement is a signed 32-bit
/// count from the end of the instruction, so the target has to be within about 2 GB of it;
/// a page allocated at a bad address is the normal way to fail here, and it must fail
/// loudly rather than wrap. The read is eight bytes, so the target is required to be
/// 8-byte aligned too - x86 tolerates an unaligned read, but nothing good comes of one in
/// a path that runs per rider per frame.
inline bool PlanDisp(uintptr_t siteVa, uintptr_t constVa, int32_t* outDisp) {
    if (!outDisp || siteVa == 0 || constVa == 0) return false;
    if ((constVa & 0x7u) != 0) return false;
    const uintptr_t nextIp = siteVa + kSiteLen;
    const int64_t   delta  = (int64_t)constVa - (int64_t)nextIp;
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    *outDisp = (int32_t)delta;
    return true;
}

/// Where the instruction at `siteVa` currently reads from, decoded from its own bytes.
/// Used to prove a patch landed, and to recognise our own patch on the way back out.
inline uintptr_t CurrentTarget(uintptr_t siteVa, const uint8_t* site) {
    if (!site) return 0;
    int32_t disp = 0;
    for (int i = 0; i < 4; ++i)
        disp |= (int32_t)((uint32_t)site[kDispOffset + i] << (8 * i));
    return (uintptr_t)((int64_t)(siteVa + kSiteLen) + (int64_t)disp);
}

} // namespace antifreeze
