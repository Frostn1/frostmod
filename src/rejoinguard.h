// rejoinguard.h - which vehicle the rejoin fix may free, and when.
//
// The rejoin fix (rejoin.h, frostmod.cpp RjTick) tears down a departed rider's vehicle record
// so a rejoin is built fresh. Freeing the wrong record takes a live rider out of the game, and
// on 2026-10-04 at 13:58:57 it freed the LOCAL rider's own bike (vehicle 0, race 41) for a
// connection (id 26) that never had a race entry. The Garage page then read a setup that no
// longer existed and the game crashed (research: crash-2026-10-04-net-update.md).
//
// How the old code got there:
//   - conn -> vehicle came from "any live load request whose +0x30 names this vehicle", and
//     was only ever overwritten, never cleared. A mapping outlived the request that made it.
//   - The only trigger was DISCONNECTION_INFO (the roster memset), which fires for any
//     roster id - spectators and riders still joining included - not a race-entry removal.
//   - Nothing checked the vehicle was not the local rider's.
//
// This header holds the whole decision so it can be tested without a game. frostmod.cpp only
// reads memory into the views below and runs the teardown on a TearDown verdict.
#pragma once
#include <cstdint>

namespace rejoin {

constexpr int      kVehMax         = 50;      // == mxb::VEHICLE_MAX (static_assert in frostmod.cpp)
constexpr uint64_t kMapFreshMs     = 2000;    // a mapping counts only if its request was seen this recently
constexpr uint64_t kRemoveWindowMs = 10000;   // a race-entry removal pairs with a disconnect within this
constexpr uint64_t kGiveUpMs       = 30000;   // a vehicle still in use this long after is left alone
constexpr int      kPendingMax     = 16;
constexpr int      kRemovedMax     = 16;

// What one frame shows of a vehicle record.
struct VehicleView {
    bool live = false;         // record +0 != 0
    int  key = 0;              // race number (+0x0C)
    bool hasRequest = false;   // a live load request names this vehicle
    int  requestConn = 0;      // that request's connection id
};

enum class Verdict { Wait, TearDown, Drop };

enum class Why {
    Ok,                    // TearDown
    BadIndex,              // vehicle index out of range
    OwnUnknown,            // cannot tell which vehicle is ours, so cannot prove this is not it
    LocalVehicle,          // it IS ours
    LocalRaceNumber,       // it carries our race number
    AlreadyFreed,          // record no longer live
    Reused,                // record now holds another race number
    OtherOwner,            // a load request for it belongs to a different connection
    StillInUse,            // its own request is still there after kGiveUpMs
    NoRaceEntryRemoval,    // no RaceRemoveEntry for that race number around the disconnect
    WaitingForRequest,     // Wait: the game still holds its load request
    WaitingForRemoval,     // Wait: removal not seen yet
};

inline const char* WhyText(Why w) {
    switch (w) {
    case Why::Ok:                 return "confirmed race-entry removal, not ours, released by the game";
    case Why::BadIndex:           return "vehicle index out of range";
    case Why::OwnUnknown:         return "own vehicle unknown, so it cannot be proven not ours";
    case Why::LocalVehicle:       return "it is the local rider's own vehicle";
    case Why::LocalRaceNumber:    return "it carries the local rider's race number";
    case Why::AlreadyFreed:       return "record already freed";
    case Why::Reused:             return "record reused by another race number";
    case Why::OtherOwner:         return "a load request for it belongs to another connection";
    case Why::StillInUse:         return "still in use after 30 s";
    case Why::NoRaceEntryRemoval: return "no race-entry removal for that race number";
    case Why::WaitingForRequest:  return "waiting for the game to release it";
    case Why::WaitingForRemoval:  return "waiting for the race-entry removal";
    }
    return "?";
}

struct Decision { Verdict verdict; Why why; };

// A departure being watched: connection `conn` left, and `veh` (race `key`) was theirs.
struct Pending { int conn = 0; int veh = -1; int key = 0; uint64_t since = 0; };

// ownVeh: the local rider's vehicle index ([0xE62710] - 1; -1 = none). ownKey: its race
// number (0 = unknown). removalSeen: a RaceRemoveEntry for p.key near p.since.
inline Decision Decide(const Pending& p, int ownVeh, int ownKey, const VehicleView& v,
                       bool removalSeen, uint64_t now) {
    if (p.veh < 0 || p.veh >= kVehMax)            return {Verdict::Drop, Why::BadIndex};
    if (ownVeh < 0 || ownVeh >= kVehMax)          return {Verdict::Drop, Why::OwnUnknown};
    if (p.veh == ownVeh)                          return {Verdict::Drop, Why::LocalVehicle};
    if (ownKey != 0 && p.key == ownKey)           return {Verdict::Drop, Why::LocalRaceNumber};
    if (!v.live)                                  return {Verdict::Drop, Why::AlreadyFreed};
    if (v.key != p.key)                           return {Verdict::Drop, Why::Reused};
    const uint64_t age = now >= p.since ? now - p.since : 0;
    if (v.hasRequest) {
        if (v.requestConn != p.conn)              return {Verdict::Drop, Why::OtherOwner};
        if (age > kGiveUpMs)                      return {Verdict::Drop, Why::StillInUse};
        return {Verdict::Wait, Why::WaitingForRequest};
    }
    if (!removalSeen) {
        if (age > kRemoveWindowMs)                return {Verdict::Drop, Why::NoRaceEntryRemoval};
        return {Verdict::Wait, Why::WaitingForRemoval};
    }
    return {Verdict::TearDown, Why::Ok};
}

// What a disconnect resolved to.
enum class Lookup { Mapped, NoMapping, Stale, Ambiguous, Full };
inline const char* LookupText(Lookup l) {
    switch (l) {
    case Lookup::Mapped:    return "mapped";
    case Lookup::NoMapping: return "no vehicle mapped to that connection (not a racer)";
    case Lookup::Stale:     return "mapping is stale (its load request is long gone)";
    case Lookup::Ambiguous: return "more than one vehicle mapped to that connection";
    case Lookup::Full:      return "too many departures pending";
    }
    return "?";
}

// conn -> vehicle, the departures being watched, and recent race-entry removals.
class Tracker {
public:
    struct Map { int conn = 0; int key = 0; bool valid = false; uint64_t seen = 0; };
    struct Removal { int key = 0; uint64_t at = 0; };

    void Reset() { *this = Tracker(); }

    // Once a frame per vehicle. The mapping exists only while a live load request names the
    // vehicle; once it stops, it ages out after kMapFreshMs, and a key change clears it.
    void Observe(int veh, const VehicleView& v, uint64_t now) {
        if (veh < 0 || veh >= kVehMax) return;
        Map& m = map_[veh];
        if (!v.live) { m = Map(); return; }
        if (v.hasRequest) { m = {v.requestConn, v.key, true, now}; return; }
        if (m.valid && (m.key != v.key || now - m.seen > kMapFreshMs)) m = Map();
    }

    // A connection left (DISCONNECTION_INFO). Records a pending departure when exactly one
    // fresh mapping names it. *outVeh is the vehicle, or -1.
    Lookup OnDisconnect(int conn, uint64_t now, int* outVeh = nullptr, int* outKey = nullptr) {
        if (outVeh) *outVeh = -1;
        if (outKey) *outKey = 0;
        int found = -1, stale = 0, count = 0;
        for (int i = 0; i < kVehMax; ++i) {
            const Map& m = map_[i];
            if (!m.valid || m.conn != conn) continue;
            if (now - m.seen > kMapFreshMs) { ++stale; continue; }
            ++count; found = i;
        }
        if (count == 0) return stale ? Lookup::Stale : Lookup::NoMapping;
        if (count > 1)  return Lookup::Ambiguous;
        if (outVeh) *outVeh = found;
        if (outKey) *outKey = map_[found].key;
        if (nPending_ >= kPendingMax) return Lookup::Full;
        pending_[nPending_++] = {conn, found, map_[found].key, now};
        return Lookup::Mapped;
    }

    // RaceRemoveEntry: the race entry for this race number really went away.
    void OnRaceRemove(int key, uint64_t now) {
        if (nRemoved_ >= kRemovedMax) {               // drop the oldest
            for (int i = 1; i < kRemovedMax; ++i) removed_[i - 1] = removed_[i];
            --nRemoved_;
        }
        removed_[nRemoved_++] = {key, now};
    }

    // RaceAddEntry: that race number is (back) in the race. Anything pending on it is
    // cancelled - its record may already be the rejoined rider's - and old removals and
    // mappings for it are forgotten. Returns how many pending departures were cancelled.
    int OnRaceAdd(int key) {
        int dropped = 0;
        for (int k = 0; k < nPending_; ) {
            if (pending_[k].key == key) { pending_[k] = pending_[--nPending_]; ++dropped; }
            else ++k;
        }
        for (int k = 0; k < nRemoved_; ) {
            if (removed_[k].key == key) { for (int j = k + 1; j < nRemoved_; ++j) removed_[j - 1] = removed_[j]; --nRemoved_; }
            else ++k;
        }
        for (Map& m : map_) if (m.valid && m.key == key) m = Map();
        return dropped;
    }

    bool RemovalSeen(int key, uint64_t since) const {
        for (int k = 0; k < nRemoved_; ++k) {
            const uint64_t d = removed_[k].at > since ? removed_[k].at - since : since - removed_[k].at;
            if (removed_[k].key == key && d <= kRemoveWindowMs) return true;
        }
        return false;
    }

    int PendingCount() const { return nPending_; }
    const Pending& PendingAt(int k) const { return pending_[k]; }
    void RemovePendingAt(int k) { pending_[k] = pending_[--nPending_]; }
    void Forget(int veh) { if (veh >= 0 && veh < kVehMax) map_[veh] = Map(); }
    const Map& MapOf(int veh) const { return map_[veh]; }

private:
    Map     map_[kVehMax];
    Pending pending_[kPendingMax];
    int     nPending_ = 0;
    Removal removed_[kRemovedMax];
    int     nRemoved_ = 0;
};

}  // namespace rejoin
