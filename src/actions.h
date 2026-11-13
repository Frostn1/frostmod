// What happened to MXB App's commands: the answer half of the command channel.
//
// The command channel (cmdchannel.h) was fire-and-forget: the app wrote a verb and never
// learnt whether FrostMod took it, refused it, or is waiting for the player to leave a
// content screen before a reload can run. The Game SDK turns those commands into actions a
// tool can call ("reload mods", "refresh tracks") and needs the answer, so FrostMod now
// reports it here, in a shared block the app reads: `Local\FrostModActions`.
//
// Two things live in the block:
//   - the refresh state right now (running or not, which lists, how far, what is waiting),
//     whoever asked for it: the app, the in-game hotkey, or FrostMod's own watcher;
//   - a numbered ring of what happened to each command, keyed by the `id` the app put in the
//     command document. A command without an id still gets records, with an empty id.
//
// Written by the injected copy (the one that runs commands), on the render thread and on the
// command poll; writers share an in-process mutex around the seqlock, as gamefeed.h does. The
// app relies on the sequence count alone. Nothing here reads input from the block.
//
// No Win32 here, so tests/actions_test.cpp runs in CI.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace frostmod {
namespace actions {

constexpr uint32_t kVersion = 1;
constexpr const char kMappingName[] = "Local\\FrostModActions";
constexpr int kRingSize = 64;       // a power of two
constexpr int kIdLen = 40;
constexpr int kVerbLen = 24;
constexpr int kDetailLen = 96;

// What a record says. Values are wire contract with the app.
enum State : uint32_t {
    ACCEPTED = 1,   // understood; work starts now or is queued
    WAITING = 2,    // a content screen is open; it runs when the player leaves it
    RUNNING = 3,    // the refresh is under way
    DONE = 4,
    REFUSED = 5,    // not done, and won't be: unknown verb, not supported in this game...
    FAILED = 6,     // tried and went wrong
};

// The refresh in progress, by list. A bit set; values are wire contract.
enum Lists : uint32_t {
    LIST_TRACKS = 1, LIST_BIKES = 2, LIST_GEAR = 4, LIST_PAINTS = 8, LIST_ALL = 0xFF,
};

struct Record {
    uint32_t serial;            // 1, 2, 3...; 0 = never written
    uint32_t state;
    uint32_t lists;             // which lists a refresh record is about; 0 otherwise
    uint32_t reserved;
    uint64_t atMs;              // GetTickCount64-style, for ordering only
    char id[kIdLen];
    char verb[kVerbLen];
    char detail[kDetailLen];
};
static_assert(sizeof(Record) == 184, "Record layout changed - update the app's reader");

struct Block {
    uint32_t version;
    uint32_t seq;               // seqlock: odd while a write is in flight
    char frostmod[16];          // FROSTMOD_VERSION of the writer, e.g. "0.53.0"
    uint32_t ringHead;          // serial of the newest record
    uint32_t refreshActive;     // 1 while a refresh runs
    uint32_t refreshLists;      // what it rebuilds
    uint32_t refreshDone;       // steps done
    uint32_t refreshTotal;      // of
    uint32_t pendingLists;      // waiting for a content screen to close; 0 = none
    uint32_t reserved[6];
    Record ring[kRingSize];
};
static_assert(sizeof(Block) == 72 + kRingSize * sizeof(Record), "Block layout changed - update the app's reader");

inline void Set(char* dst, size_t cap, const char* s) {
    std::memset(dst, 0, cap);
    if (!s) return;
    size_t n = std::strlen(s);
    if (n > cap - 1) n = cap - 1;
    std::memcpy(dst, s, n);
}

class Writer {
public:
    void Attach(Block* b, const char* frostmodVersion) {
        std::lock_guard<std::mutex> lock(mu_);
        block_ = b;
        if (!b) return;
        std::memset(b, 0, sizeof(Block));
        b->version = kVersion;
        Set(b->frostmod, sizeof(b->frostmod), frostmodVersion);
    }
    bool attached() const { return block_ != nullptr; }

    /// One command's news. `lists` only for refresh records.
    void Note(const char* id, const char* verb, State s, const char* detail, uint64_t atMs,
              uint32_t lists = 0) {
        Write([&](Block& b) { Push(b, id, verb, s, detail, atMs, lists); });
    }

    /// The refresh state, and a record for every command waiting on it. `ids` are the commands
    /// this refresh answers, as (id, verb); a refresh nobody asked for through the app (the
    /// hotkey, FrostMod's own watcher) has none and still updates the state.
    void Refresh(bool active, uint32_t lists, uint32_t done, uint32_t total, uint32_t pending) {
        Write([&](Block& b) {
            b.refreshActive = active ? 1u : 0u;
            b.refreshLists = lists;
            b.refreshDone = done;
            b.refreshTotal = total;
            b.pendingLists = pending;
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

    static void Push(Block& b, const char* id, const char* verb, State s, const char* detail,
                     uint64_t atMs, uint32_t lists) {
        const uint32_t serial = b.ringHead + 1;
        Record& r = b.ring[serial & (kRingSize - 1)];
        r.serial = serial;
        r.state = s;
        r.lists = lists;
        r.reserved = 0;
        r.atMs = atMs;
        Set(r.id, kIdLen, id);
        Set(r.verb, kVerbLen, verb);
        Set(r.detail, kDetailLen, detail);
        b.ringHead = serial;
    }

    std::mutex mu_;
    Block* block_ = nullptr;
};

// Which commands a refresh is answering. A refresh can be asked for several times before it
// runs (the merge in RequestRefresh), so every id is kept until the refresh that covers it
// finishes. Render thread only, apart from Add on the command poll - hence the lock.
class Pending {
public:
    struct Item { std::string id, verb; uint32_t lists; };

    void Add(const std::string& id, const std::string& verb, uint32_t lists) {
        std::lock_guard<std::mutex> lock(mu_);
        items_.push_back({id, verb, lists});
    }
    /// The commands a refresh of `lists` covers, removed from the set.
    std::vector<Item> Take(uint32_t lists) {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<Item> out, keep;
        for (auto& it : items_) ((it.lists & ~lists) == 0 ? out : keep).push_back(it);
        items_.swap(keep);
        return out;
    }
    /// Look without taking: the commands a refresh of `lists` will answer.
    std::vector<Item> Peek(uint32_t lists) {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<Item> out;
        for (auto& it : items_)
            if ((it.lists & ~lists) == 0) out.push_back(it);
        return out;
    }

private:
    std::mutex mu_;
    std::vector<Item> items_;
};

}  // namespace actions
}  // namespace frostmod
