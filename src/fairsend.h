// fairsend.h - giving a starved rider a turn in the packet.
//
// THE BUG, from the server's side. A dedicated server builds its rider-state broadcast once
// per recipient, and a full gate does not fit in one datagram. It orders the riders by how
// far they are from whoever it is building for, nearest first, then fills the packet from the
// top of that list and stops at the first rider who will not fit.
//
// Nothing gives a rider a turn for having been skipped. The order is distance, so the riders
// furthest away are cut, and they are cut again on the next packet, and the one after that.
// A network tick is 30 ms and the receiving client stops drawing a rider whose updates fall
// more than 300 ms apart, so ten consecutive cuts and that rider is invisible on that screen.
// He is still solid, still racing, and can still land on somebody who cannot see him.
//
// The worst case is the rider furthest away for the longest, which in a race is the leader.
//
// WHAT THIS DOES. Before the packet is filled, any rider who is close to being starved is
// moved to the front of the candidate list. He goes ahead of the distance order, gets into
// this packet, and his gap resets. Everybody else keeps their order behind him.
//
// The packet does not get bigger and no extra packets are sent. The same bytes go out, to the
// same riders, at the same rate. Only the choice of who is in them changes, and it changes
// only for a rider who would otherwise have vanished.
//
// WHY THE THRESHOLD IS NINE. The client gives up at ten ticks. Promoting at nine leaves one
// tick of margin, which is the smallest number that cannot be late. Promoting earlier would
// push nearby riders out of packets to no purpose, since a rider at three ticks is being
// drawn perfectly well.
//
// THE COST, stated plainly. A promoted rider takes a slot a nearer rider would have had, so
// that nearer rider waits a tick. Riders close to you are sent every tick and have ticks to
// spare, so a single tick of delay on one of them costs nothing that can be seen. The trade
// is deliberate: it spends slack that near riders have to buy visibility for far riders who
// have none.
//
// WHAT IT CANNOT DO. If the gate is large enough that one datagram cannot hold even the
// starving riders, promotion rotates who is starved instead of ending it. The rule still
// helps, because rotating is what the stock code fails to do, but it stops being a guarantee.
// A fuller repair would need more than one datagram per tick, which is a protocol change.
//
// No Win32 and no game here, so tests/fairsend_test.cpp runs anywhere. frostserver.cpp does
// the memory work and calls Promote() on the real array.
#pragma once
#include <cstdint>
#include <cstddef>

namespace fairsend {

// ---- the game's candidate list ----------------------------------------------------------
// RVAs from the module base, MX Bikes. The builder memsets the whole array at entry, so a
// record whose slot pointer is null has not been filled this pass.
constexpr uintptr_t kRvaBuilder    = 0x29C760;  // builds SERVER_DATA for one recipient
constexpr uintptr_t kRvaCandidates = 0x9D7460;  // the candidate array
constexpr uintptr_t kRvaServerTick = 0x9E3AC0;  // the current network tick
constexpr size_t    kRecStride     = 0x20;      // 32 bytes per candidate
constexpr size_t    kMaxRecords    = 50;        // and 50 rider slots, so at most 50 of them

/// One candidate, exactly as the game lays it out. Only the three fields we read are named;
/// the rest is the game's and is carried through a move untouched, which is why a promotion
/// moves whole records rather than rewriting fields.
struct Record {
    int32_t  index;        // +0x00  rider slot index, later overwritten with the tier
    uint32_t pad04;        // +0x04
    uint64_t slot;         // +0x08  pointer to the entity slot; null means unused
    int32_t  tickDelta;    // +0x10  the rider's stored tick MINUS the current tick
    float    sortKey;      // +0x14  distance from the recipient, ascending
    uint64_t tail;         // +0x18
};
static_assert(sizeof(Record) == kRecStride, "candidate record must match the game's stride");

// ---- the rule ---------------------------------------------------------------------------

/// Ticks of silence after which the receiving client stops drawing a rider. 300 ms at 30 ms
/// per tick. This number belongs to the client, not the server; see antifreeze.h.
constexpr int kCliffTicks = 10;

/// Where we step in. One tick under the cliff, so a promoted rider cannot arrive late.
constexpr int kPromoteAtTicks = kCliffTicks - 1;

/// How far behind this rider is, in ticks.
///
/// The record stores `rider_tick - current_tick`, so a rider who has not been sent for a
/// while carries a negative number that grows more negative. Staleness is its negation, and
/// a rider sent this tick reads zero.
///
/// *Inferred:* that the stored tick is the last tick this rider was sent to this recipient.
/// It is the only reading that fits the arithmetic, and a positive value would mean a tick
/// in the future. Negative results are clamped to zero rather than trusted, so a surprise
/// here costs a missed promotion instead of a corrupted order.
inline int StalenessTicks(const Record& r) {
    const int64_t s = -(int64_t)r.tickDelta;
    if (s <= 0) return 0;
    if (s > INT32_MAX) return INT32_MAX;
    return (int)s;
}

/// How many records are live this pass.
///
/// Two things end the prefix, and both have to be checked.
///
/// The array is memset at entry, so on an untouched pass the first **null** slot pointer ends
/// it. But the builder also drops riders whose send interval has not elapsed, and it does that
/// by memmoving the rest of the array down one record and decrementing its own count. That
/// leaves the last record **duplicated** in the tail, with a live-looking non-null pointer.
/// Counting on null alone would walk into those and promote a stale copy of a rider who is
/// already in the list, which would push a real rider out of the packet.
///
/// A dead tail record is always a copy of one still in the prefix, and every live rider
/// appears once, so the first **repeated** slot pointer ends the prefix just as surely as a
/// null one. Checking both means the hook needs nothing from the game's registers.
inline int CountFilled(const Record* recs, int cap) {
    if (!recs || cap <= 0) return 0;
    if (cap > (int)kMaxRecords) cap = (int)kMaxRecords;
    uint64_t seen[kMaxRecords];
    int n = 0;
    while (n < cap && recs[n].slot != 0) {
        for (int i = 0; i < n; ++i)
            if (seen[i] == recs[n].slot) return n;   // a memmove left its copy behind
        seen[n] = recs[n].slot;
        ++n;
    }
    return n;
}

/// Is this rider close enough to the cliff to need a slot now?
inline bool NeedsPromotion(const Record& r, int promoteAt) {
    return StalenessTicks(r) >= promoteAt;
}

/// Move every starving rider to the front, keeping order within both groups.
///
/// A stable partition, so the starving riders stay in distance order among themselves and so
/// does everybody else. Stability matters: an unstable shuffle would churn the order every
/// tick and move the starvation around rather than ending it.
///
/// Returns how many records were promoted, which is 0 when there is nothing to do. The array
/// is left untouched in that case, so the common path writes no memory at all.
inline int Promote(Record* recs, int n, int promoteAt) {
    if (!recs || n <= 1) return 0;
    if (n > (int)kMaxRecords) n = (int)kMaxRecords;

    // Count first. Most ticks promote nobody, and this keeps that case free.
    int needed = 0;
    for (int i = 0; i < n; ++i)
        if (NeedsPromotion(recs[i], promoteAt)) ++needed;
    if (needed == 0 || needed == n) return 0;   // nothing to do, or nothing to reorder against

    // A stable partition in place, front to back. `write` is where the next starving rider
    // goes; everything between it and the rider being moved shifts back one slot.
    int write = 0;
    for (int i = 0; i < n; ++i) {
        if (!NeedsPromotion(recs[i], promoteAt)) continue;
        if (i != write) {
            const Record moving = recs[i];
            for (int k = i; k > write; --k) recs[k] = recs[k - 1];
            recs[write] = moving;
        }
        ++write;
    }
    return needed;
}

} // namespace fairsend
