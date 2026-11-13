// The rules the Game SDK feed runs on (src/gamefeed.h).
//
// The block is a wire contract with MXB App's Rust reader, written from two game threads and
// read by another process. Pure, like session_test.cpp: no Win32, no game, no shared memory.

#include "../src/gamefeed.h"

#include <cstdio>
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <thread>

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

using namespace frostmod::gamefeed;

// The offsets the Rust reader hard-codes. If one moves, this fails before the app misreads.
static void LayoutIsTheContract() {
    CHECK(offsetof(Block, ringHead) == 16, "ringHead at %zu", offsetof(Block, ringHead));
    CHECK(offsetof(Block, telemetryTime) == 64, "telemetryTime at %zu", offsetof(Block, telemetryTime));
    CHECK(offsetof(Block, telemetry) == 76, "telemetry at %zu", offsetof(Block, telemetry));
    CHECK(offsetof(Block, eventLen) == 588, "eventLen at %zu", offsetof(Block, eventLen));
    CHECK(offsetof(Block, classHeaderLen) == 3420, "classHeaderLen at %zu", offsetof(Block, classHeaderLen));
    CHECK(offsetof(Block, positionSize) == 6536, "positionSize at %zu", offsetof(Block, positionSize));
    CHECK(offsetof(Block, entryCount) == 8592, "entryCount at %zu", offsetof(Block, entryCount));
    CHECK(offsetof(Block, ring) == 8596 + 64 * 428, "ring at %zu", offsetof(Block, ring));
    CHECK(sizeof(Block) == 8596 + 64 * 428 + 256 * 64, "size %zu", sizeof(Block));
}

static void SnapshotsAndRing() {
    auto block = std::make_unique<Block>();
    Writer w;
    w.Attach(block.get(), 1, 8);
    CHECK(block->version == kVersion && block->game == 1 && block->dataVersion == 8, "header");
    CHECK((block->seq & 1u) == 0, "seq even after attach");

    unsigned char event[820] = {0};
    std::strcpy((char*)event, "Frost");
    w.EventInit(event, sizeof(event));
    CHECK(block->eventLen == 820 && std::strcmp((char*)block->event, "Frost") == 0, "event copied");

    float tele[47] = {0};
    tele[5] = 12.5f;   // speedometer
    w.Telemetry(tele, sizeof(tele), 3.0f, 0.25f);
    CHECK(block->telemetryLen == sizeof(tele) && block->telemetryTick == 2, "telemetry len %u tick %u",
          block->telemetryLen, block->telemetryTick);
    CHECK(block->telemetryTime == 3.0f && block->telemetryPos == 0.25f, "time/pos");

    int lap[8] = {6, 12, 2, 0, 95123, 30000, 61000, 1};
    w.RaceLap(lap, sizeof(lap));
    CHECK(block->ringHead == 2, "two ring records, got %u", block->ringHead);
    const Record& r = block->ring[2 & (kRingSize - 1)];
    CHECK(r.serial == 2 && r.kind == RACE_LAP && r.len == 32, "lap record %u %u %u", r.serial, r.kind, r.len);
    CHECK(std::memcmp(r.data, lap, 32) == 0, "lap payload");

    // An oversized payload is cut to the record, and says so.
    unsigned char big[200];
    std::memset(big, 7, sizeof(big));
    w.RaceSession(big, sizeof(big));
    CHECK(block->raceSessionLen == 200, "session snapshot whole");
    CHECK(block->ring[3].len == (uint32_t)kRecordPayload, "session record cut to %d", kRecordPayload);

    CHECK((block->seq & 1u) == 0, "seq even after writes");
}

static void EntriesKeepTheirSlot() {
    auto block = std::make_unique<Block>();
    Writer w;
    w.Attach(block.get(), 1, 8);
    unsigned char e[416] = {0};
    for (int num : {7, 12, 7}) {
        std::memcpy(e, &num, 4);
        std::snprintf((char*)e + 4, 100, "rider%d", num);
        w.RaceAddEntry(e, sizeof(e));
    }
    CHECK(block->entryCount == 2, "a rider added twice keeps one slot, got %u", block->entryCount);
    int twelve = 12;
    w.RaceRemoveEntry(&twelve, 4);
    CHECK(block->entries[1].raceNum == 12 && block->entries[1].active == 0, "removed is kept inactive");
    CHECK(block->entries[0].active == 1, "others stay");
    // A new event starts with nobody.
    w.EventInit(e, 0);
    CHECK(block->entryCount == 0, "new event clears entries");
}

static void ClassificationStride() {
    auto block = std::make_unique<Block>();
    Writer w;
    w.Attach(block.get(), 1, 8);
    int hdr[4] = {7, 16, 120000, 3};
    int rows[3][9] = {{12, 0, 95000, 2, 3, 0, 0, 0, 0}, {7, 0, 96000, 1, 3, 1500, 0, 0, 0}, {3, 0, 0, 0, 2, 0, 1, 0, 0}};
    w.RaceClassification(hdr, sizeof(hdr), rows, 3, 36);
    CHECK(block->classCount == 3 && block->classEntrySize == 36, "count %u size %u", block->classCount,
          block->classEntrySize);
    int second;
    std::memcpy(&second, block->classEntries + 36, 4);
    CHECK(second == 7, "second row starts at the stride, got %d", second);
    // More riders than the block holds are cut, not overrun.
    static int many[100][9];
    w.RaceClassification(hdr, sizeof(hdr), many, 100, 36);
    CHECK(block->classCount == (uint32_t)kMaxRiders, "capped at %d", kMaxRiders);
}

static void RingWrapsAndReaderCatchesUp() {
    auto block = std::make_unique<Block>();
    Writer w;
    w.Attach(block.get(), 1, 8);
    for (int i = 0; i < kRingSize + 10; ++i) w.RunStart();
    CHECK(block->ringHead == (uint32_t)kRingSize + 10, "head %u", block->ringHead);
    // The oldest still there is head - size + 1.
    uint32_t oldest = block->ringHead - kRingSize + 1;
    CHECK(block->ring[oldest & (kRingSize - 1)].serial == oldest, "oldest kept");
}

static void ConcurrentWritersNeverTearAWholeRead() {
    auto live = std::make_unique<Block>();
    Writer w;
    w.Attach(live.get(), 1, 8);
    std::atomic<bool> stop{false};
    std::thread a([&] {
        float t[47];
        for (int i = 0; !stop; ++i) {
            for (float& f : t) f = (float)i;
            w.Telemetry(t, sizeof(t), (float)i, (float)i);
        }
    });
    std::thread b([&] {
        int pos[7] = {1, 0, 0, 0, 0, 0, 0};
        while (!stop) w.RaceTrackPosition(1, pos, 28);
    });
    auto copy = std::make_unique<Block>();
    int whole = 0;
    for (int i = 0; i < 20000; ++i) {
        if (!TryRead(*live, *copy)) continue;
        ++whole;
        float first, last;
        std::memcpy(&first, copy->telemetry, 4);
        std::memcpy(&last, copy->telemetry + 46 * 4, 4);
        CHECK(first == last, "torn telemetry %f vs %f", first, last);
        if (first != last) break;
    }
    stop = true;
    a.join();
    b.join();
    CHECK(whole > 0, "no whole read in 20000 tries");
}

int main() {
    LayoutIsTheContract();
    SnapshotsAndRing();
    EntriesKeepTheirSlot();
    ClassificationStride();
    RingWrapsAndReaderCatchesUp();
    ConcurrentWritersNeverTearAWholeRead();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("gamefeed: all passed\n");
    return 0;
}
