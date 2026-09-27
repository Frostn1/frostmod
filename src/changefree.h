// After an accepted bike change: which vehicle record to free, if any.
//
// On CHANGEANSWER rej 0 the game builds the new bike in the FIRST FREE vehicle record
// (0x6ABB0, mode 34) and moves the own-vehicle index [0xE62710] (record + 1) to it, but
// never tears the old record down: the old bike stays on the pit stand, the new one spawns
// inside it, and a change made on track leaves a frozen rider with physics behind. FrostMod
// snapshots the index before sending and frees the old record once the index has moved.
// Map: net/change.c.
//
// Pure - no Win32, no game - so tests/changefree_test.cpp runs it in CI.
#pragma once

namespace frostmod {

// What the own-vehicle index said before the change was sent, and what the old record
// looked like then.
struct ChangeSnapshot {
    int index = -1;       // record index (the global minus one); -1 = nothing to free
    int key = 0;          // its race number (vehicle +0x0C)
};

enum class ChangeFree { Wait, Free, Drop };

// Decide, once a frame after the accept.
//   nowIndex   the own-vehicle index now (global minus one)
//   oldLive    old record's +0x00 is non-zero
//   oldKey     old record's race number now
//   elapsedMs  since the accept
// Free only on a real move to another valid record, and only while the old record still
// holds what it held (not zeroed, not reused by someone else). A rejoin never arms this.
inline ChangeFree DecideChangeFree(const ChangeSnapshot& s, int nowIndex, bool oldLive, int oldKey,
                                   unsigned long long elapsedMs, int vehicleMax,
                                   unsigned long long timeoutMs = 10000) {
    if (s.index < 0 || s.index >= vehicleMax) return ChangeFree::Drop;
    if (!oldLive || oldKey != s.key) return ChangeFree::Drop;        // already gone / reused
    if (nowIndex >= 0 && nowIndex < vehicleMax && nowIndex != s.index) return ChangeFree::Free;
    return elapsedMs >= timeoutMs ? ChangeFree::Drop : ChangeFree::Wait;
}

}  // namespace frostmod
