// The MXB Coach recorder's file rules (src/coachrec.h).
//
// The .mxbc file is a wire contract with a Rust reader in the MXB Coach app, compiled
// separately, so its shape is pinned here: what opens a file, what goes in it and in which
// order, and that a lap is on disk the moment it is reported. Pure C++, runs anywhere.

#include "../src/coachrec.h"

#include <cstdio>
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

struct Rec {
    uint8_t              tag;
    std::vector<uint8_t> payload;
};

static std::vector<uint8_t> Slurp(const std::string& path) {
    std::vector<uint8_t> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}

// Parses what the app's reader parses. Returns false on a bad header.
static bool Parse(const std::vector<uint8_t>& b, std::vector<Rec>& out) {
    if (b.size() < 8 || std::memcmp(b.data(), coachrec::kMagic, 4) != 0) return false;
    uint32_t ver;
    std::memcpy(&ver, b.data() + 4, 4);
    if (ver != coachrec::kFormatVersion) return false;
    size_t at = 8;
    while (at + 8 <= b.size()) {
        uint32_t len;
        std::memcpy(&len, b.data() + at + 4, 4);
        if (at + 8 + len > b.size()) break;
        out.push_back({b[at], std::vector<uint8_t>(b.begin() + at + 8, b.begin() + at + 8 + len)});
        at += 8 + len;
    }
    return true;
}

static std::string TempDir() {
    const char* t = std::getenv("TMPDIR");
    if (!t) t = std::getenv("TEMP");
    std::string d = t ? t : "/tmp";
    if (d.back() != '/' && d.back() != '\\') d += '/';
    return d;
}

static std::vector<Rec> Read(const std::string& path) {
    std::vector<Rec> recs;
    CHECK(Parse(Slurp(path), recs), "bad header in %s", path.c_str());
    return recs;
}

static void OneStint() {
    coachrec::Recorder r;
    r.set_dir(TempDir());

    // Telemetry before a stint has nowhere to go and must not create a file.
    uint8_t bike[188] = {0};
    r.on_sample(bike, sizeof(bike), 0.f, 0.f);
    CHECK(!r.recording(), "a sample outside a stint opened a file");

    uint8_t event[820] = {0};
    std::memcpy(event, "Rider", 5);
    r.on_event(event, sizeof(event));
    uint8_t segs[2 * coachrec::kTrackSegmentSize] = {0};
    segs[4] = 7;
    r.on_centreline(2, segs, coachrec::kTrackSegmentSize);

    uint8_t session[112] = {0};
    CHECK(r.on_run_init(session, sizeof(session), "coachrec-test-a"), "could not open a file");
    const std::string path = r.path();

    r.on_start();
    for (int i = 0; i < 3; ++i) {
        bike[0] = uint8_t(i);
        r.on_sample(bike, sizeof(bike), 0.02f * i, 0.1f * i);
    }
    int lap[4] = {0, 0, 61234, 1};
    r.on_lap(lap, sizeof(lap));

    // A lap is flushed as it is reported, so a crash after it keeps it.
    {
        std::vector<Rec> mid = Read(path);
        CHECK(!mid.empty() && mid.back().tag == coachrec::LAP, "lap not on disk before close");
    }

    int split[3] = {0, 20000, -150};
    r.on_split(split, sizeof(split));
    r.on_stop();
    r.on_run_end();
    CHECK(!r.recording(), "run end left the file open");

    std::vector<Rec> recs = Read(path);
    const uint8_t want[] = {coachrec::EVENT,  coachrec::CENTRELINE, coachrec::SESSION,
                            coachrec::START,  coachrec::SAMPLE,     coachrec::SAMPLE,
                            coachrec::SAMPLE, coachrec::LAP,        coachrec::SPLIT,
                            coachrec::STOP,   coachrec::END};
    CHECK(recs.size() == sizeof(want), "record count %zu", recs.size());
    for (size_t i = 0; i < recs.size() && i < sizeof(want); ++i)
        CHECK(recs[i].tag == want[i], "record %zu is tag %d, want %d", i, recs[i].tag, want[i]);
    if (recs.size() != sizeof(want)) return;

    CHECK(recs[0].payload.size() == 820 && std::memcmp(recs[0].payload.data(), "Rider", 5) == 0,
          "event not stored verbatim");

    int count, stride;
    std::memcpy(&count, recs[1].payload.data(), 4);
    std::memcpy(&stride, recs[1].payload.data() + 4, 4);
    CHECK(count == 2 && stride == coachrec::kTrackSegmentSize, "centreline header %d/%d", count,
          stride);
    CHECK(recs[1].payload.size() == 8u + 2u * coachrec::kTrackSegmentSize && recs[1].payload[8 + 4] == 7,
          "centreline segments not stored verbatim");

    // Sample = time, position, then the game's struct untouched.
    const Rec& s = recs[6];
    CHECK(s.payload.size() == 8 + sizeof(bike), "sample size %zu", s.payload.size());
    float t, pos;
    std::memcpy(&t, s.payload.data(), 4);
    std::memcpy(&pos, s.payload.data() + 4, 4);
    CHECK(t > 0.039f && t < 0.041f && pos > 0.19f && pos < 0.21f, "sample head %f %f", t, pos);
    CHECK(s.payload[8] == 2, "sample body not the game's bytes");

    int lap_ms;
    std::memcpy(&lap_ms, recs[7].payload.data() + 8, 4);
    CHECK(lap_ms == 61234, "lap time %d", lap_ms);
    CHECK(recs[10].payload.empty(), "END carries a payload");

    std::remove(path.c_str());
}

// A second stint in the same event opens its own file and still knows the event and track.
static void SecondStintKeepsTheEvent() {
    coachrec::Recorder r;
    r.set_dir(TempDir());
    uint8_t event[820] = {1};
    r.on_event(event, sizeof(event));
    uint8_t segs[coachrec::kTrackSegmentSize] = {0};
    r.on_centreline(1, segs, coachrec::kTrackSegmentSize);

    r.on_run_init(nullptr, 0, "coachrec-test-b");
    const std::string first = r.path();
    r.on_run_init(nullptr, 0, "coachrec-test-c");   // no RunDeinit in between
    const std::string second = r.path();
    r.on_event_end();

    std::vector<Rec> a = Read(first), b = Read(second);
    CHECK(!a.empty() && a.back().tag == coachrec::END, "first stint not closed by the second");
    CHECK(b.size() == 4 && b[0].tag == coachrec::EVENT && b[1].tag == coachrec::CENTRELINE &&
              b[2].tag == coachrec::SESSION && b[3].tag == coachrec::END,
          "second stint lost the event (%zu records)", b.size());
    CHECK(b.size() > 2 && b[2].payload.empty(), "a null session wrote bytes");

    // A new event forgets the old track's centreline.
    r.on_event(event, sizeof(event));
    r.on_run_init(nullptr, 0, "coachrec-test-d");
    const std::string third = r.path();
    r.on_run_end();
    std::vector<Rec> c = Read(third);
    CHECK(c.size() == 3 && c[1].tag == coachrec::SESSION, "stale centreline carried over");

    std::remove(first.c_str());
    std::remove(second.c_str());
    std::remove(third.c_str());
}

// The game hands these callbacks garbage sizes in no documented case, but a negative one
// must not become a four-gigabyte write.
static void NonsenseSizes() {
    coachrec::Recorder r;
    r.set_dir(TempDir());
    r.on_event(nullptr, -1);
    r.on_centreline(-3, nullptr, coachrec::kTrackSegmentSize);
    r.on_run_init(nullptr, -5, "coachrec-test-e");
    int lap[4] = {0};
    r.on_lap(lap, -16);
    r.on_sample(nullptr, 188, 0.f, 0.f);
    const std::string path = r.path();
    r.on_run_end();
    std::vector<Rec> recs = Read(path);
    CHECK(recs.size() == 4, "record count %zu", recs.size());
    for (const Rec& x : recs) CHECK(x.payload.empty(), "tag %d wrote bytes", x.tag);
    std::remove(path.c_str());
}

int main() {
    OneStint();
    SecondStintKeepsTheEvent();
    NonsenseSizes();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("coachrec: all checks passed\n");
    return 0;
}
