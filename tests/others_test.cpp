// The other riders for MXB Coach's recorder (src/others.h).
//
// Pins how the Race* payloads become the ENTRY, POSITIONS, RACE_LAP and RACE_SPLIT records,
// whose bytes are a wire contract with MXB Coach's Rust reader, and the 10 Hz throttle.
// Pure C++, runs anywhere.

#include "../src/coachrec.h"
#include "../src/others.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

using namespace others;
using TP = sdk_mxb::SPluginsRaceTrackPosition_t;

template <class T>
static T At(const std::vector<uint8_t>& b, size_t at) {
    T v;
    std::memcpy(&v, &b[at], sizeof(v));
    return v;
}

static sdk::RaceAddEntry Add(int num, const char* name, const char* bike, const char* cat, int unactive = 0) {
    sdk::RaceAddEntry a;
    std::memset(&a, 0, sizeof(a));
    a.m_iRaceNum = num;
    std::strncpy(a.m_szName, name, sizeof(a.m_szName) - 1);
    std::strncpy(a.m_szVehicleName, "Yamaha YZ450F 2023", sizeof(a.m_szVehicleName) - 1);
    std::strncpy(a.m_szVehicleShortName, bike, sizeof(a.m_szVehicleShortName) - 1);
    std::strncpy(a.m_szCategory, cat, sizeof(a.m_szCategory) - 1);
    a.m_iUnactive = unactive;
    return a;
}

static TP Pos(int num, float x, float y, float z, float track, int crashed = 0) {
    TP t = {num, x, y, z, 90.0f, track, crashed};
    return t;
}

static void EntryBytes() {
    sdk::RaceAddEntry a = Add(12, "B1 | Frost", "YZ450F", "MX1 OEM");
    Entry e;
    CHECK(EntryFromAdd(&a, sizeof(a), e), "entry");
    CHECK(EntryRaceNum(e) == 12 && e[4] == 1 && e[5] == 0 && e[7] == 0, "head %d %d", EntryRaceNum(e), e[4]);
    CHECK(std::strcmp(reinterpret_cast<const char*>(&e[8]), "B1 | Frost") == 0, "name");
    CHECK(std::strcmp(reinterpret_cast<const char*>(&e[72]), "YZ450F") == 0, "bike short name");
    CHECK(std::strcmp(reinterpret_cast<const char*>(&e[112]), "MX1 OEM") == 0, "category");
    CHECK(e[8 + 10] == 0 && e[71] == 0 && e[151] == 0, "NUL-padded");

    a = Add(7, "gone", "KX250", "MX2", 1);
    CHECK(EntryFromAdd(&a, sizeof(a), e) && e[4] == 0, "unactive: left");

    // Long strings are cut and stay terminated; a 100-byte name with no NUL doesn't overrun.
    std::memset(a.m_szName, 'n', sizeof(a.m_szName));
    std::memset(a.m_szVehicleShortName, 'b', sizeof(a.m_szVehicleShortName));
    std::memset(a.m_szCategory, 'c', sizeof(a.m_szCategory));
    CHECK(EntryFromAdd(&a, sizeof(a), e), "long");
    CHECK(e[8 + 62] == 'n' && e[8 + 63] == 0 && e[72] == 'b', "name cut to 63");
    CHECK(e[72 + 38] == 'b' && e[72 + 39] == 0 && e[112 + 38] == 'c' && e[112 + 39] == 0, "bike, category cut to 39");

    CHECK(!EntryFromAdd(&a, sizeof(a) - 1, e) && !EntryFromAdd(nullptr, sizeof(a), e), "short or null");
}

static void PositionBytes() {
    const TP arr[3] = {Pos(12, 10.0f, 2.0f, -5.0f, 0.25f), Pos(7, 100.5f, 3.5f, 40.25f, 0.5f, 1),
                       Pos(-1, 0, 0, 0, 0)};
    const float me[3] = {10.4f, 2.0f, -5.3f};
    std::vector<uint8_t> p;
    CHECK(PositionsPayload(3.5f, 3, arr, sizeof(TP), me, p), "payload");
    CHECK(p.size() == kPosHeader + 2 * kPosBike, "size %zu (bad race number dropped)", p.size());
    CHECK(At<float>(p, 0) == 3.5f && At<uint16_t>(p, 4) == 2 && At<uint16_t>(p, 6) == 20, "header");
    CHECK(At<uint16_t>(p, 8) == 12 && p[10] == kLocal && p[11] == 0, "first: local, not crashed");
    CHECK(At<float>(p, 12) == 0.25f && At<float>(p, 16) == 10.0f && At<float>(p, 20) == 2.0f && At<float>(p, 24) == -5.0f,
          "first: pos x y z");
    CHECK(At<uint16_t>(p, 28) == 7 && p[30] == kCrashed, "second: crashed, not local");
    CHECK(At<float>(p, 32) == 0.5f && At<float>(p, 36) == 100.5f && At<float>(p, 40) == 3.5f && At<float>(p, 44) == 40.25f,
          "second: pos x y z");

    // Nobody near enough, or no telemetry yet: no one is flagged.
    const float far[3] = {50.0f, 2.0f, -5.0f};
    CHECK(PositionsPayload(0, 2, arr, sizeof(TP), far, p) && p[10] == 0 && p[30] == kCrashed, "none local");
    CHECK(PositionsPayload(0, 2, arr, sizeof(TP), nullptr, p) && p[10] == 0, "no own position");

    // A later build's wider stride still reads; a narrower one is refused.
    struct Wide {
        TP    t;
        float extra[3];
    } wide[2] = {{arr[0], {9, 9, 9}}, {arr[1], {9, 9, 9}}};
    CHECK(PositionsPayload(1, 2, wide, sizeof(Wide), me, p) && At<uint16_t>(p, 4) == 2 && At<uint16_t>(p, 28) == 7 &&
              At<float>(p, 44) == 40.25f,
          "wide stride");
    CHECK(!PositionsPayload(1, 2, arr, 24, me, p) && p.empty(), "KRP-sized stride refused");
    CHECK(PositionsPayload(1, 0, arr, sizeof(TP), me, p) && p.size() == kPosHeader && At<uint16_t>(p, 4) == 0, "empty");

    std::vector<TP> many(200, Pos(3, 0, 0, 0, 0));
    CHECK(PositionsPayload(1, 200, many.data(), sizeof(TP), nullptr, p) && At<uint16_t>(p, 4) == kMaxBikes, "capped");
}

static void OwnPositionFromTelemetry() {
    uint8_t bike[188] = {0};
    const float pos[3] = {1.5f, -2.0f, 300.0f};
    std::memcpy(bike + 24, pos, 12);
    float x, y, z;
    CHECK(OwnPosition(bike, sizeof(bike), x, y, z) && x == 1.5f && y == -2.0f && z == 300.0f, "x y z at 24");
    CHECK(!OwnPosition(bike, 35, x, y, z), "short");
}

static void TimedBytes() {
    sdk_mxb::SPluginsRaceLap_t lap = {6, 12, 3, 0, 61234, {20000, 41000}, 1};
    std::vector<uint8_t> p;
    CHECK(TimedPayload(99.5f, &lap, sizeof(lap), sizeof(lap), p) && p.size() == 36, "lap size %zu", p.size());
    CHECK(At<float>(p, 0) == 99.5f && At<int32_t>(p, 8) == 12 && At<int32_t>(p, 12) == 3 && At<int32_t>(p, 20) == 61234,
          "lap fields");
    CHECK(!TimedPayload(1, &lap, 31, sizeof(lap), p) && p.empty(), "short lap");
    sdk_mxb::SPluginsRaceSplit_t sp = {6, 12, 3, 1, 20000};
    CHECK(TimedPayload(1, &sp, sizeof(sp), sizeof(sp), p) && p.size() == 24 && At<int32_t>(p, 20) == 20000, "split");
}

static void RosterAndThrottle() {
    Tracker t;
    Entry e;
    sdk::RaceAddEntry a = Add(12, "Frost", "YZ450F", "MX1");
    CHECK(t.added(&a, sizeof(a), e) && t.roster().size() == 1, "added");
    a = Add(7, "Other", "KX450", "MX1");
    t.added(&a, sizeof(a), e);
    a = Add(12, "Frost2", "YZ450F", "MX1");
    CHECK(t.added(&a, sizeof(a), e) && t.roster().size() == 2, "re-added updates in place");
    CHECK(std::strcmp(reinterpret_cast<const char*>(&t.roster().at(12)[8]), "Frost2") == 0, "new name kept");

    int32_t num = 7;
    CHECK(t.removed(&num, 4, e) && e[4] == 0 && EntryRaceNum(e) == 7 && t.roster().size() == 1, "removed");
    CHECK(std::strcmp(reinterpret_cast<const char*>(&e[8]), "Other") == 0, "removed keeps the name");
    num = 99;
    CHECK(t.removed(&num, 4, e) && EntryRaceNum(e) == 99 && e[4] == 0 && e[8] == 0, "unknown rider removed");

    const TP arr[1] = {Pos(12, 0, 0, 0, 0.1f)};
    std::vector<uint8_t> p;
    t.on_stint();
    CHECK(!t.positions(1, arr, sizeof(TP), p), "no sample yet: no time to stamp");
    sdk_mxb::SPluginsRaceLap_t lap = {};
    CHECK(!t.lap(&lap, sizeof(lap), p), "no lap before a sample either");

    uint8_t bike[188] = {0};
    int written = 0;
    std::vector<uint8_t> last;
    for (int i = 0; i < 50; ++i) {  // one second at 50 Hz, a position call after every sample
        t.on_sample(10.0f + 0.02f * i, bike, sizeof(bike));
        if (t.positions(1, arr, sizeof(TP), p)) {
            ++written;
            last = p;
        } else {
            CHECK(p.empty(), "no record: nothing built");
        }
    }
    CHECK(written == 10, "10 Hz: %d in a second", written);
    CHECK(last.size() == kPosHeader + kPosBike && last[10] == kLocal, "local rider at the origin");

    // A new stint writes at once even though its clock restarted below the last write.
    t.on_stint();
    t.on_sample(0.0f, bike, sizeof(bike));
    CHECK(t.positions(1, arr, sizeof(TP), p) && At<float>(p, 0) == 0.0f, "first of a new stint");
    // A clock that goes back mid-stint (a reset) writes too.
    t.on_sample(5.0f, bike, sizeof(bike));
    t.positions(1, arr, sizeof(TP), p);
    t.on_sample(1.0f, bike, sizeof(bike));
    CHECK(t.positions(1, arr, sizeof(TP), p), "time went back");
    CHECK(t.lap(&lap, sizeof(lap), p) && At<float>(p, 0) == 1.0f, "lap stamped with the sample time");

    t.clear_roster();
    CHECK(t.roster().empty(), "cleared");
}

static std::string TempDir() {
    const char* tmp = std::getenv("TMPDIR");
    if (!tmp) tmp = std::getenv("TEMP");  // Windows
    std::string dir = tmp ? tmp : "/tmp";
    if (dir.back() != '/' && dir.back() != '\\') dir += '/';
    return dir;
}

// The records land in the stint's file and tile it, so a reader that knows none of them (MXB
// Coach before this) reads the rest unchanged.
static void RecordsAndOldReadersSkipThem() {
    coachrec::Recorder r;
    r.set_dir(TempDir());
    uint8_t event[820] = {0};
    r.on_event(event, sizeof(event));

    Tracker t;
    Entry e;
    sdk::RaceAddEntry a = Add(12, "Frost", "YZ450F", "MX1");
    t.added(&a, sizeof(a), e);
    a = Add(7, "Other", "KX450", "MX1");
    t.added(&a, sizeof(a), e);

    uint8_t session[112] = {0};
    CHECK(r.on_run_init(session, sizeof(session), "others-test"), "open");
    t.on_stint();
    for (const auto& kv : t.roster()) r.record(coachrec::ENTRY, kv.second.data(), uint32_t(kv.second.size()));

    uint8_t bike[188] = {0};
    const TP arr[2] = {Pos(12, 0, 0, 0, 0.1f), Pos(7, 5, 0, 0, 0.12f)};
    std::vector<uint8_t> p;
    for (int i = 0; i < 20; ++i) {
        r.on_sample(bike, sizeof(bike), 0.02f * i, 0.1f);
        t.on_sample(0.02f * i, bike, sizeof(bike));
        if (t.positions(2, arr, sizeof(TP), p)) r.record(coachrec::POSITIONS, p.data(), uint32_t(p.size()));
    }
    sdk_mxb::SPluginsRaceLap_t lap = {6, 7, 1, 0, 60000, {20000, 40000}, 0};
    if (t.lap(&lap, sizeof(lap), p)) r.record(coachrec::RACE_LAP, p.data(), uint32_t(p.size()));
    sdk_mxb::SPluginsRaceSplit_t sp = {6, 7, 2, 0, 20100};
    if (t.split(&sp, sizeof(sp), p)) r.record(coachrec::RACE_SPLIT, p.data(), uint32_t(p.size()));
    int32_t gone = 7;
    if (t.removed(&gone, 4, e)) r.record(coachrec::ENTRY, e.data(), uint32_t(e.size()));
    const std::string path = r.path();
    r.on_run_end();

    std::FILE* f = std::fopen(path.c_str(), "rb");
    std::vector<uint8_t> b;
    if (f) {
        uint8_t buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + n);
        std::fclose(f);
    }
    CHECK(b.size() > 8, "file read from %s", path.c_str());
    // telemetry.rs's loop: skip every record by its length, act on the tags it knows.
    int samples = 0, entries = 0, positions = 0, laps = 0, splits = 0;
    size_t at = 8;
    uint8_t last_tag = 0;
    while (at + 8 <= b.size()) {
        uint32_t len;
        std::memcpy(&len, &b[at + 4], 4);
        if (at + 8 + len > b.size()) break;
        const uint8_t tag = b[at];
        last_tag = tag;
        if (tag == coachrec::SAMPLE) ++samples;
        if (tag == coachrec::ENTRY) {
            ++entries;
            CHECK(len == kEntrySize, "entry length %u", len);
        }
        if (tag == coachrec::POSITIONS) {
            ++positions;
            CHECK(len == kPosHeader + 2 * kPosBike, "positions length %u", len);
        }
        if (tag == coachrec::RACE_LAP) {
            ++laps;
            CHECK(len == 36, "race lap length %u", len);
        }
        if (tag == coachrec::RACE_SPLIT) {
            ++splits;
            CHECK(len == 24, "race split length %u", len);
        }
        at += 8 + len;
    }
    CHECK(at == b.size(), "records don't tile the file");
    // 20 samples 0.02 s apart span 0.38 s: writes at 0, 0.1, 0.2, 0.3.
    CHECK(samples == 20 && entries == 3 && positions == 4 && laps == 1 && splits == 1,
          "samples %d entries %d positions %d laps %d splits %d", samples, entries, positions, laps, splits);
    CHECK(last_tag == coachrec::END, "file ends with END");
    std::remove(path.c_str());
}

int main() {
    EntryBytes();
    PositionBytes();
    OwnPositionFromTelemetry();
    TimedBytes();
    RosterAndThrottle();
    RecordsAndOldReadersSkipThem();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("others: all checks passed\n");
    return 0;
}
