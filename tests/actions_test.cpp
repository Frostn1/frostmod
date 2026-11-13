// The rules the command answers run on (src/actions.h): the block MXB App's Game SDK reads to
// learn what each command came to. Pure: no Win32, no game.

#include "../src/actions.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>

static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n");                                                  \
        }                                                                       \
    } while (0)

using namespace frostmod::actions;

static void LayoutIsTheContract() {
    // The offsets the app's Rust reader hard-codes.
    CHECK(offsetof(Block, frostmod) == 8, "frostmod at %zu", offsetof(Block, frostmod));
    CHECK(offsetof(Block, ringHead) == 24, "ringHead at %zu", offsetof(Block, ringHead));
    CHECK(offsetof(Block, refreshActive) == 28, "refreshActive");
    CHECK(offsetof(Block, pendingLists) == 44, "pendingLists");
    CHECK(offsetof(Block, ring) == 72, "ring at %zu", offsetof(Block, ring));
    CHECK(offsetof(Record, atMs) == 16 && offsetof(Record, id) == 24 && offsetof(Record, verb) == 64 &&
              offsetof(Record, detail) == 88,
          "record fields moved");
    CHECK(sizeof(Block) == 72 + 64 * 184, "size %zu", sizeof(Block));
}

static void NotesRingAndRefresh() {
    auto b = std::make_unique<Block>();
    Writer w;
    w.Attach(b.get(), "0.53.0");
    CHECK(std::strcmp(b->frostmod, "0.53.0") == 0 && b->version == kVersion, "header");
    w.Note("a1", "refresh_tracks", ACCEPTED, "", 5, LIST_TRACKS);
    w.Refresh(true, LIST_TRACKS, 1, 2, 0);
    CHECK(b->ringHead == 1, "one record");
    const Record& r = b->ring[1];
    CHECK(r.serial == 1 && r.state == ACCEPTED && r.lists == LIST_TRACKS && r.atMs == 5, "record");
    CHECK(std::strcmp(r.id, "a1") == 0 && std::strcmp(r.verb, "refresh_tracks") == 0, "strings");
    CHECK(b->refreshActive == 1 && b->refreshDone == 1 && b->refreshTotal == 2, "refresh state");
    CHECK((b->seq & 1u) == 0, "seq even");
    // Long strings are cut and terminated.
    char longId[200];
    std::memset(longId, 'x', sizeof(longId) - 1);
    longId[199] = 0;
    w.Note(longId, "v", DONE, longId, 6);
    CHECK(std::strlen(b->ring[2].id) == kIdLen - 1 && std::strlen(b->ring[2].detail) == kDetailLen - 1, "cut");
}

static void PendingAnswersWhatARefreshCovers() {
    Pending p;
    p.Add("paints", "refresh_paints", LIST_PAINTS);
    p.Add("tracks", "refresh_tracks", LIST_TRACKS);
    p.Add("all", "reload_mods", LIST_ALL);
    // A gear refresh rebuilds the paint lists too: it answers the paints request only.
    auto gear = p.Take(LIST_GEAR | LIST_PAINTS);
    CHECK(gear.size() == 1 && gear[0].id == "paints", "gear covers paints, got %zu", gear.size());
    CHECK(p.Peek(LIST_TRACKS).size() == 1, "tracks still waiting");
    // A full reload answers everything left.
    auto all = p.Take(LIST_ALL);
    CHECK(all.size() == 2, "full covers the rest, got %zu", all.size());
    CHECK(p.Take(LIST_ALL).empty(), "nothing left");
}

int main() {
    LayoutIsTheContract();
    NotesRingAndRefresh();
    PendingAnswersWhatARefreshCovers();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("actions: all passed\n");
    return 0;
}
