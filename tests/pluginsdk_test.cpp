// Invariants over the per-title plugin ABI in pluginsdk.h.
//
// The structs in that header carry their own static_asserts, so simply including it proves
// the sizes. What this file adds is the layer above: that each title's PluginAbi describes
// THAT title, that the numbers still match PiBoSo's published examples, and that a field a
// title does not have is marked absent rather than left pointing at offset 0 - which is a
// real offset (the rider name lives there).
//
// The expected values below are typed out from mxb_example.c / gpb_example.c /
// krp_example.c by hand, on purpose: a table derived from the same structs it is checking
// would agree with itself no matter what either of them said.
//
// Pure constants, like offsets_test.cpp - no Win32, no game, runs anywhere.

#include "../src/pluginsdk.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n  (%s)\n", #cond);                                   \
        }                                                                       \
    } while (0)

// One row per title, transcribed from that title's published example.
struct Expected {
    const char* title;
    const PluginAbi* abi;
    int tp_size, tp_crashed;
    int cls_hdr_size, cls_num_entries;
    int cls_entry_size, cls_entry_num_laps;
    int ev_size, ev_rider, ev_track, ev_server, ev_guid;
};

static const Expected kExpected[] = {
    // MX Bikes: event is the published 652-byte struct PLUS the server-name tail data
    // version 8 appends (64 + 4 + 100), which FrostMod reads in production.
    {"MX Bikes", &kAbiMxb,
     28, 24,          // track position: 7 fields, m_iCrashed last
     16, 12,          // classification header: session, state, time, count
     36, 16,          // entry: raceNum, state, bestLap, bestLapNum, numLaps, ...
     820, 0, 444, 652, 720},

    // GP Bikes: same track position as MX, but m_fBestSpeed widens the entry.
    {"GP Bikes", &kAbiGpb,
     28, 24,
     16, 12,
     40, 20,
     652, 0, 444, -1, -1},

    // Kart Racing Pro: no crashed flag, an extra m_iSessionSeries in the header, and a
    // different event struct entirely (drive type and a dash name; no server, no GUID).
    {"Kart Racing Pro", &kAbiKrp,
     24, -1,
     20, 16,
     40, 20,
     748, 0, 540, -1, -1},
};
static const int kExpectedCount = (int)(sizeof(kExpected) / sizeof(kExpected[0]));

static void abis_match_the_published_sdks() {
    for (int i = 0; i < kExpectedCount; ++i) {
        const Expected& e = kExpected[i];
        const PluginAbi* a = e.abi;
        CHECK(a->tp_size == e.tp_size, "%s track position is %d bytes, expected %d",
              e.title, a->tp_size, e.tp_size);
        CHECK(a->tp_crashed == e.tp_crashed, "%s m_iCrashed at %d, expected %d",
              e.title, a->tp_crashed, e.tp_crashed);
        CHECK(a->cls_hdr_size == e.cls_hdr_size, "%s classification header is %d, expected %d",
              e.title, a->cls_hdr_size, e.cls_hdr_size);
        CHECK(a->cls_num_entries == e.cls_num_entries, "%s m_iNumEntries at %d, expected %d",
              e.title, a->cls_num_entries, e.cls_num_entries);
        CHECK(a->cls_entry_size == e.cls_entry_size, "%s classification entry is %d, expected %d",
              e.title, a->cls_entry_size, e.cls_entry_size);
        CHECK(a->cls_entry_num_laps == e.cls_entry_num_laps, "%s m_iNumLaps at %d, expected %d",
              e.title, a->cls_entry_num_laps, e.cls_entry_num_laps);
        CHECK(a->ev_size == e.ev_size, "%s event is %d bytes, expected %d",
              e.title, a->ev_size, e.ev_size);
        CHECK(a->ev_rider == e.ev_rider, "%s rider name at %d, expected %d",
              e.title, a->ev_rider, e.ev_rider);
        CHECK(a->ev_track == e.ev_track, "%s track id at %d, expected %d",
              e.title, a->ev_track, e.ev_track);
        CHECK(a->ev_server == e.ev_server, "%s server name at %d, expected %d",
              e.title, a->ev_server, e.ev_server);
        CHECK(a->ev_guid == e.ev_guid, "%s guid at %d, expected %d",
              e.title, a->ev_guid, e.ev_guid);
    }
}

// An absent field must be -1, never 0: offset 0 is where the rider/driver name lives, so a
// field zeroed by accident would read a name as a server and put a voice room under it.
static void absent_fields_are_negative_not_zero() {
    for (int i = 0; i < kExpectedCount; ++i) {
        const PluginAbi* a = kExpected[i].abi;
        CHECK(a->tp_crashed == -1 || (a->tp_crashed > 0 && a->tp_crashed < a->tp_size),
              "%s tp_crashed is neither absent nor inside the element", kExpected[i].title);
        CHECK(a->ev_server == -1 || (a->ev_server > 0 && a->ev_server < a->ev_size),
              "%s ev_server is neither absent nor inside the event", kExpected[i].title);
        CHECK(a->ev_guid == -1 || (a->ev_guid > 0 && a->ev_guid < a->ev_size),
              "%s ev_guid is neither absent nor inside the event", kExpected[i].title);
        CHECK(a->cls_entry_num_laps >= 0 && a->cls_entry_num_laps < a->cls_entry_size,
              "%s m_iNumLaps is outside the classification entry", kExpected[i].title);
        CHECK(a->cls_num_entries >= 0 && a->cls_num_entries < a->cls_hdr_size,
              "%s m_iNumEntries is outside the classification header", kExpected[i].title);
        CHECK(a->sdk && *a->sdk, "%s has no sdk provenance string", kExpected[i].title);
    }
}

// The mapping is the whole point: this is what stops one title's callbacks being read with
// another's layout, which is the quiet cousin of running another title's reload table.
static void every_title_gets_its_own_layout() {
    CHECK(PluginAbiFor(&GAME_MXB) == &kAbiMxb, "MX Bikes maps to the wrong ABI");
    CHECK(PluginAbiFor(&GAME_GPB) == &kAbiGpb, "GP Bikes maps to the wrong ABI");
    CHECK(PluginAbiFor(&GAME_KRP) == &kAbiKrp, "Kart Racing Pro maps to the wrong ABI");
    // An unrecognised host falls back to MX Bikes, matching what the DLL assumes.
    CHECK(PluginAbiFor(nullptr) == &kAbiMxb, "the fallback layout is no longer MX Bikes'");

    for (const GameOffsets* g : ALL_GAMES)
        CHECK(PluginAbiFor(g) != nullptr, "%s has no layout", g->id);
    CHECK(&kAbiMxb != &kAbiGpb && &kAbiGpb != &kAbiKrp && &kAbiMxb != &kAbiKrp,
          "two titles share one layout");
}

int main() {
    abis_match_the_published_sdks();
    absent_fields_are_negative_not_zero();
    every_title_gets_its_own_layout();

    if (g_failures) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("pluginsdk: all checks passed (%d titles)\n", kExpectedCount);
    return 0;
}
