// The game feed: what the plugin API hands FrostMod, passed on to MXB App for its Game SDK.
//
// MXB App's Game SDK lets overlays, dashboards, extensions and AI agents read the live game:
// the session, the local rider's telemetry, the timing screen and what happens on track. All
// of it arrives in this process through PiBoSo's published plugin callbacks and nowhere else,
// so this block is the channel, exactly like the session block beside it (session.h).
//
// ## The contract
//
// FrostMod writes; the app reads. Nothing here takes input, nothing here touches game memory,
// and nothing is sent anywhere: the block lives in a `Local\` mapping that only this Windows
// user can open. Deciding what a consumer may see (online fairness, rate limits, whether the
// local endpoint is on at all) is the app's job, because the app is the one that hands it on.
//
// ## Raw payloads, not fields
//
// The callbacks' payloads are copied byte for byte, each with the length the game gave. The
// app decodes them with the published layouts, the same way MXB Coach decodes its recordings
// (coachrec.h). So a field nobody reads today is already there for the SDK that wants it, a
// game build that appends fields costs nothing, and the code that runs on the game's threads
// is a bounded memcpy and nothing more.
//
// ## Snapshot plus ring
//
// State that is replaced (telemetry, the timing screen, every bike's place on track, the
// entry list) is a snapshot: the latest copy wins. Things that *happen* (a lap, a split, a
// session change, a rider joining) go into a ring of numbered records as well, so a reader
// polling at 50 Hz still sees every lap even when two arrive between polls. A reader keeps the
// last serial it saw and takes everything after it; if it fell more than the ring behind, it
// knows exactly how much it missed.
//
// ## Two writers, one seqlock
//
// The game calls telemetry and the race callbacks from different threads, and a seqlock has
// room for one writer. So writers take `Writer::mu_` first, an in-process lock held only for
// the copy. The reader in the other process never takes it: it relies on the sequence count
// alone, as session.h's reader does.
//
// No Win32 here, so tests/gamefeed_test.cpp runs in CI.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace frostmod {
namespace gamefeed {

// Bumped if anything below changes shape. The app refuses a version it doesn't know.
constexpr uint32_t kVersion = 1;

constexpr const char kMappingName[] = "Local\\FrostModGameFeed";

// PiBoSo's Startup() convention: 0 = 100 Hz, 1 = 50 Hz, 2 = 20 Hz, 3 = 10 Hz. 50 Hz is what
// MXB Coach records at; it is enough for a dashboard, a shift light or an overlay, and the
// cost per call is one memcpy of ~200 bytes.
constexpr int kTelemetryRate = 1;

constexpr int kMaxRiders = 64;
constexpr int kRingSize = 256;            // records; a power of two
constexpr int kRecordPayload = 52;        // bytes; every event payload we ring fits

// Payload capacities. Each is past the published struct it holds, so a build that appends
// fields still fits; anything longer is cut, and the length says what was kept.
constexpr int kTelemetryCap = 512;        // SPluginsBikeData_t, 188 in data version 8
constexpr int kEventCap = 1024;           // SPluginsBikeEvent_t, 820
constexpr int kRunCap = 512;              // SPluginsBikeSession_t
constexpr int kRaceEventCap = 256;        // SPluginsRaceEvent_t, 208
constexpr int kRaceSessionCap = 1024;     // SPluginsRaceSession_t
constexpr int kClassHeaderCap = 32;       // SPluginsRaceClassification_t, 16
constexpr int kClassEntryCap = 48;        // SPluginsRaceClassificationEntry_t, 36
constexpr int kPositionCap = 32;          // SPluginsRaceTrackPosition_t, 28
constexpr int kEntryCap = 416;            // SPluginsRaceAddEntry_t, 416

// What a ring record is. Values are wire contract with the app.
enum Kind : uint32_t {
    EVENT_INIT = 1,          // payload: none (the event itself is in the snapshot)
    EVENT_DEINIT = 2,
    RUN_INIT = 3,            // payload: none (the session is in the snapshot)
    RUN_DEINIT = 4,
    RUN_START = 5,
    RUN_STOP = 6,
    RUN_LAP = 7,             // payload: raw SPluginsBikeLap_t
    RUN_SPLIT = 8,           // payload: raw SPluginsBikeSplit_t
    RACE_EVENT = 9,          // payload: none (snapshot)
    RACE_DEINIT = 10,
    RACE_ADD_ENTRY = 11,     // payload: i32 race number (the entry is in the snapshot)
    RACE_REMOVE_ENTRY = 12,  // payload: i32 race number
    RACE_SESSION = 13,       // payload: the first 52 bytes of SPluginsRaceSession_t
    RACE_SESSION_STATE = 14, // payload: raw SPluginsRaceSessionState_t
    RACE_LAP = 15,           // payload: raw SPluginsRaceLap_t
    RACE_SPLIT = 16,         // payload: raw SPluginsRaceSplit_t
    RACE_COMMUNICATION = 17, // payload: raw SPluginsRaceCommunication_t
};

struct Record {
    uint32_t serial;         // 1, 2, 3...; 0 = never written
    uint32_t kind;
    uint32_t len;            // bytes of `data` that are payload
    uint8_t data[kRecordPayload];
};
static_assert(sizeof(Record) == 64, "Record layout changed - update the app's reader");

struct Entry {
    int32_t raceNum;
    uint32_t active;         // 1 in the event, 0 removed (the slot is kept so a name survives)
    uint32_t len;
    uint8_t data[kEntryCap]; // raw SPluginsRaceAddEntry_t
};
static_assert(sizeof(Entry) == 428, "Entry layout changed - update the app's reader");

struct Block {
    uint32_t version;
    uint32_t seq;            // seqlock: odd while a write is in flight
    uint32_t game;           // 1 MX Bikes, 2 GP Bikes, 3 Kart Racing Pro
    uint32_t dataVersion;    // GetModDataVersion() of this build

    // The serial of the newest ring record; 0 before the first.
    uint32_t ringHead;
    // Bumped on every write of the matching snapshot, so a reader can tell new from same.
    uint32_t telemetryTick, classTick, positionTick, entriesTick;
    uint32_t reserved[7];

    // RunTelemetry: the track time and lap position the game passes beside the payload.
    float telemetryTime, telemetryPos;
    uint32_t telemetryLen;
    uint8_t telemetry[kTelemetryCap];

    uint32_t eventLen;       // 0 when no event is open
    uint8_t event[kEventCap];
    uint32_t runLen;         // 0 when the bike is not on track
    uint8_t run[kRunCap];
    uint32_t raceEventLen;
    uint8_t raceEvent[kRaceEventCap];
    uint32_t raceSessionLen;
    uint8_t raceSession[kRaceSessionCap];

    uint32_t classHeaderLen;
    uint8_t classHeader[kClassHeaderCap];
    uint32_t classEntrySize; // bytes per entry as stored (the game's stride, cut to the cap)
    uint32_t classCount;
    uint8_t classEntries[kMaxRiders * kClassEntryCap];

    uint32_t positionSize;   // bytes per bike as stored
    uint32_t positionCount;
    uint8_t positions[kMaxRiders * kPositionCap];

    uint32_t entryCount;     // slots in use, active or not
    Entry entries[kMaxRiders];

    Record ring[kRingSize];
};
// A wire contract with a Rust reader compiled separately: 4-byte fields and arrays that are
// multiples of four, so neither side can pad differently.
static_assert(sizeof(Block) % 4 == 0, "Block must stay 4-byte sized");
static_assert(offsetof(Block, telemetryTime) == 64, "Block header moved - update the app's reader");
static_assert(sizeof(Block) == 64 + 8 + 4 + kTelemetryCap + 4 + kEventCap + 4 + kRunCap + 4 +
                                   kRaceEventCap + 4 + kRaceSessionCap + 4 + kClassHeaderCap + 8 +
                                   kMaxRiders * kClassEntryCap + 8 + kMaxRiders * kPositionCap + 4 +
                                   kMaxRiders * sizeof(Entry) + kRingSize * sizeof(Record),
              "Block layout changed - update the app's reader");

namespace detail {
inline uint32_t Copy(uint8_t* dst, size_t cap, const void* src, int len) {
    if (!src || len <= 0) return 0;
    size_t n = (size_t)len < cap ? (size_t)len : cap;
    std::memcpy(dst, src, n);
    if (n < cap) std::memset(dst + n, 0, cap - n);
    return (uint32_t)n;
}
}  // namespace detail

// The writer side. One per process; every call is safe from any thread and returns quickly.
class Writer {
public:
    /// Point at the shared block (or any Block, in a test). Zeroes it: a block left over from
    /// a previous load of this dll describes a game that has ended.
    void Attach(Block* block, uint32_t game, uint32_t dataVersion) {
        std::lock_guard<std::mutex> lock(mu_);
        block_ = block;
        if (!block_) return;
        std::memset(block_, 0, sizeof(Block));
        block_->version = kVersion;
        block_->game = game;
        block_->dataVersion = dataVersion;
    }
    bool attached() const { return block_ != nullptr; }

    void Telemetry(const void* data, int len, float time, float pos) {
        Write([&](Block& b) {
            b.telemetryTime = time;
            b.telemetryPos = pos;
            b.telemetryLen = detail::Copy(b.telemetry, kTelemetryCap, data, len);
            ++b.telemetryTick;
        });
    }

    void EventInit(const void* data, int len) {
        Write([&](Block& b) {
            b.eventLen = detail::Copy(b.event, kEventCap, data, len);
            // A new event: last event's riders, timing and positions are not this one's.
            b.classCount = 0; b.classHeaderLen = 0; ++b.classTick;
            b.positionCount = 0; ++b.positionTick;
            b.entryCount = 0; ++b.entriesTick;
            b.runLen = 0; b.telemetryLen = 0; ++b.telemetryTick;
            b.raceEventLen = 0; b.raceSessionLen = 0;
            Ring(b, EVENT_INIT, nullptr, 0);
        });
    }
    void EventDeinit() {
        Write([&](Block& b) {
            b.eventLen = 0; b.runLen = 0; b.telemetryLen = 0; ++b.telemetryTick;
            Ring(b, EVENT_DEINIT, nullptr, 0);
        });
    }
    void RunInit(const void* data, int len) {
        Write([&](Block& b) {
            b.runLen = detail::Copy(b.run, kRunCap, data, len);
            Ring(b, RUN_INIT, nullptr, 0);
        });
    }
    void RunDeinit() {
        Write([&](Block& b) {
            b.runLen = 0; b.telemetryLen = 0; ++b.telemetryTick;
            Ring(b, RUN_DEINIT, nullptr, 0);
        });
    }
    void RunStart() { Write([&](Block& b) { Ring(b, RUN_START, nullptr, 0); }); }
    void RunStop()  { Write([&](Block& b) { Ring(b, RUN_STOP, nullptr, 0); }); }
    void RunLap(const void* data, int len)   { Event(RUN_LAP, data, len); }
    void RunSplit(const void* data, int len) { Event(RUN_SPLIT, data, len); }

    void RaceEvent(const void* data, int len) {
        Write([&](Block& b) {
            b.raceEventLen = detail::Copy(b.raceEvent, kRaceEventCap, data, len);
            Ring(b, RACE_EVENT, nullptr, 0);
        });
    }
    void RaceDeinit() {
        Write([&](Block& b) {
            b.raceEventLen = 0; b.raceSessionLen = 0;
            b.classCount = 0; b.classHeaderLen = 0; ++b.classTick;
            b.positionCount = 0; ++b.positionTick;
            Ring(b, RACE_DEINIT, nullptr, 0);
        });
    }
    void RaceSession(const void* data, int len) {
        Write([&](Block& b) {
            b.raceSessionLen = detail::Copy(b.raceSession, kRaceSessionCap, data, len);
            Ring(b, RACE_SESSION, data, len);
        });
    }
    void RaceSessionState(const void* data, int len) { Event(RACE_SESSION_STATE, data, len); }
    void RaceLap(const void* data, int len)          { Event(RACE_LAP, data, len); }
    void RaceSplit(const void* data, int len)        { Event(RACE_SPLIT, data, len); }
    void RaceCommunication(const void* data, int len){ Event(RACE_COMMUNICATION, data, len); }

    void RaceAddEntry(const void* data, int len) {
        if (!data || len < 4) return;
        int32_t num;
        std::memcpy(&num, data, 4);
        Write([&](Block& b) {
            Entry* e = Slot(b, num, true);
            if (e) {
                e->active = 1;
                e->len = detail::Copy(e->data, kEntryCap, data, len);
                ++b.entriesTick;
            }
            Ring(b, RACE_ADD_ENTRY, &num, 4);
        });
    }
    void RaceRemoveEntry(const void* data, int len) {
        if (!data || len < 4) return;
        int32_t num;
        std::memcpy(&num, data, 4);
        Write([&](Block& b) {
            if (Entry* e = Slot(b, num, false)) { e->active = 0; ++b.entriesTick; }
            Ring(b, RACE_REMOVE_ENTRY, &num, 4);
        });
    }

    /// `header` is the classification header, `entries` the array the game passes beside it,
    /// `count` the number of entries the header names and `stride` the size of each.
    void RaceClassification(const void* header, int headerLen, const void* entries, int count,
                            int stride) {
        Write([&](Block& b) {
            b.classHeaderLen = detail::Copy(b.classHeader, kClassHeaderCap, header, headerLen);
            if (!entries || stride <= 0 || count < 0) count = 0;
            if (count > kMaxRiders) count = kMaxRiders;
            const int keep = stride < kClassEntryCap ? stride : kClassEntryCap;
            for (int i = 0; i < count; ++i)
                detail::Copy(b.classEntries + i * keep, (size_t)keep,
                             (const uint8_t*)entries + (size_t)i * stride, keep);
            b.classEntrySize = (uint32_t)(count ? keep : 0);
            b.classCount = (uint32_t)count;
            ++b.classTick;
        });
    }

    void RaceTrackPosition(int count, const void* array, int stride) {
        Write([&](Block& b) {
            if (!array || stride <= 0 || count < 0) count = 0;
            if (count > kMaxRiders) count = kMaxRiders;
            const int keep = stride < kPositionCap ? stride : kPositionCap;
            for (int i = 0; i < count; ++i)
                detail::Copy(b.positions + i * keep, (size_t)keep,
                             (const uint8_t*)array + (size_t)i * stride, keep);
            b.positionSize = (uint32_t)(count ? keep : 0);
            b.positionCount = (uint32_t)count;
            ++b.positionTick;
        });
    }

private:
    template <class F>
    void Write(F&& change) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!block_) return;
        Block& b = *block_;
        b.seq |= 1u;
        std::atomic_thread_fence(std::memory_order_release);
        change(b);
        std::atomic_thread_fence(std::memory_order_release);
        b.seq = (b.seq + 1u) & ~1u;
    }

    void Event(Kind kind, const void* data, int len) {
        Write([&](Block& b) { Ring(b, kind, data, len); });
    }

    static void Ring(Block& b, Kind kind, const void* data, int len) {
        const uint32_t serial = b.ringHead + 1;
        Record& r = b.ring[serial & (kRingSize - 1)];
        r.serial = serial;
        r.kind = kind;
        r.len = detail::Copy(r.data, kRecordPayload, data, len);
        if (r.len == 0) std::memset(r.data, 0, kRecordPayload);
        b.ringHead = serial;
    }

    static Entry* Slot(Block& b, int32_t num, bool create) {
        for (uint32_t i = 0; i < b.entryCount; ++i)
            if (b.entries[i].raceNum == num) return &b.entries[i];
        if (!create || b.entryCount >= (uint32_t)kMaxRiders) return nullptr;
        Entry* e = &b.entries[b.entryCount++];
        std::memset(e, 0, sizeof(Entry));
        e->raceNum = num;
        return e;
    }

    std::mutex mu_;
    Block* block_ = nullptr;
};

// Reader: take a whole copy, or fail and try again later (the writer was mid-update).
inline bool TryRead(const volatile Block& live, Block& out) {
    uint32_t before = live.seq;
    if (before & 1u) return false;
    std::atomic_thread_fence(std::memory_order_acquire);
    std::memcpy(&out, const_cast<const Block*>(&live), sizeof(Block));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (live.seq != before) return false;
    return out.version == kVersion;
}

}  // namespace gamefeed
}  // namespace frostmod
