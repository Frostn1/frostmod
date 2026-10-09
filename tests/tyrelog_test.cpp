// tyrelog (src/tyrelog.h): the cfg key, the copy out of a bike laid out like the game's, the
// bike list walk, the ring, the CSV, the byte checks against the layout constants, and the
// "nanRing" key in both report shapes. Also prints the cost of one sample. Runs anywhere.

#include "../src/tyrelog.h"
#include "../src/nantrap.h"
#include "../src/offsets.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
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

using namespace tyrelog;

static void collect(void* user, const char* line) {
    auto* s = (std::string*)user;
    *s += line;
    *s += "\n";
}

static std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a < s.size()) {
        size_t b = s.find('\n', a);
        if (b == std::string::npos) b = s.size();
        out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}
static int commas(const std::string& s) {
    int n = 0;
    for (char c : s) n += c == ',';
    return n;
}
static std::vector<std::string> fields(const std::string& s) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(',', a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

template <class T>
static void put(std::vector<uint8_t>& m, size_t off, T v) {
    std::memcpy(m.data() + off, &v, sizeof(v));
}

static void cfg() {
    Config c;
    CHECK(c.on, "on by default (with nantrap)");
    CHECK(ParseCfgLine("tyrelog=0", c) && !c.on, "tyrelog=0 turns it off");
    CHECK(ParseCfgLine("  tyrelog = 1\n", c) && c.on, "tyrelog=1, spaced");
    CHECK(!ParseCfgLine("nantrap=1", c), "not ours");
    CHECK(!ParseCfgLine("tyrelog", c) && !ParseCfgLine("tyrelog=x", c), "no value: not ours");
    CHECK(!ParseCfgLine(nullptr, c), "null");
    // nantrap gates it: tyrelog=1 alone is nothing.
    CHECK(!Enabled(false, Config{}), "off without nantrap");
    Config off; off.on = false;
    CHECK(!Enabled(true, off), "off with tyrelog=0");
    CHECK(Enabled(true, Config{}), "on with nantrap=1");
    // And nantrap's own parser does not swallow our key.
    nantrap::Config n;
    CHECK(!nantrap::ParseCfgLine("tyrelog=0", n) && !n.on, "nantrap ignores tyrelog");
}

// The byte checks carry the displacements the reads use; prove they agree with the constants.
static uint32_t disp32(const Check& c, int at) {
    uint32_t v;
    std::memcpy(&v, c.bytes + at, 4);
    return v;
}
static const Check* find(uint32_t rva) {
    for (const Check& c : kChecks) if (c.rva == rva) return &c;
    return nullptr;
}
static void checks() {
    for (const Check& c : kChecks) {
        CHECK(c.len > 0 && c.len <= 16 && c.what && c.what[0], "check %X shape", c.rva);
        int same = 0;
        for (const Check& d : kChecks) same += d.rva == c.rva;
        CHECK(same == 1, "check %X listed once", c.rva);
    }
    const Check* k;
    k = find(0x1A5A41); CHECK(k && disp32(*k, 3) == kWheelStride, "wheel stride");
    k = find(0x1A5BE3); CHECK(k && disp32(*k, 5) == kWheelSpin, "spin offset");
    k = find(0x1A5C47); CHECK(k && disp32(*k, 5) == kWheelRadius, "radius offset");
    // lea rdi, [w+0x12214] is the sample's y; x is 4 before it.
    k = find(0x1A5BFB); CHECK(k && disp32(*k, 4) == kWheelSample0 + 4, "sample 0 offset");
    k = find(0x1A6117); CHECK(k && disp32(*k, 5) == kWheelGap, "gap offset");
    k = find(0x1A6123); CHECK(k && disp32(*k, 3) == kWheelHit, "hit offset");
    k = find(0x1AFC70); CHECK(k && disp32(*k, 3) == kBikeChassis, "chassis body offset");
    k = find(0x2AC8B0); CHECK(k && disp32(*k, 3) == kBodyPos, "body pos");
    k = find(0x2AC8E0); CHECK(k && disp32(*k, 3) == kBodyLvel, "body lvel");
    k = find(0x2AC8F0); CHECK(k && disp32(*k, 3) == kBodyAvel, "body avel");
    k = find(0x1AEB24); CHECK(k && (uint8_t)k->bytes[5] == kBikeTime, "bike time");
    k = find(0x1ADA4B); CHECK(k && (uint8_t)k->bytes[2] == kBikeActive, "bike active");
    k = find(0x1BE3DC); CHECK(k && disp32(*k, 3) == kWorldStride, "world stride");
    k = find(0x1BE657); CHECK(k && (uint8_t)k->bytes[3] == kWorldBikeList, "bike list");
    k = find(0x1BF0DD); CHECK(k && (uint8_t)k->bytes[3] == kWorldBikeList, "bikes created in that list");
    k = find(0x1BF1BC); CHECK(k && (uint8_t)k->bytes[9] == kWorldBikeList, "handles resolve into it");
    k = find(0x1BF13C); CHECK(k && (uint8_t)k->bytes[1] == 0x07 && kBikeHandle == 0, "handle at +0");
    k = find(0x21F6E); CHECK(k && (uint8_t)k->bytes[2] == kVehicleBikeHandle, "vehicle +4");
    // The vehicle records are the ones FrostMod already indexes by RVA_OWN_VEHICLE.
    k = find(0x21E66); CHECK(k && disp32(*k, 3) == (uint32_t)mxb::VEHICLE_STRIDE, "vehicle stride");
    k = find(0x21E75);
    CHECK(k && 0x21E75 + 7 + (int32_t)disp32(*k, 3) == mxb::RVA_VEHICLES, "vehicle records RVA");
    // The lea the world array comes from sits where the live code reads it: sim step + 0x2B.
    k = find(0x1BE3CB);
    CHECK(k && k->rva == mxb::RVA_SIM_STEP + kSimStepLeaAt, "world lea at sim step + 0x2B");
    // ...and after the sim step's own signature, which nantrap already verifies.
    CHECK((size_t)kSimStepLeaAt >= sizeof(mxb::SIG_SIM_STEP) - 1, "lea past the signature");
}

static void read_bike() {
    std::vector<uint8_t> bike(kBikeSpan + 64, 0xCD), body(kBodySpan + 16, 0xCD);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    put<float>(bike, kBikeTime, 163.47f);
    put<double>(bike, kWheelSpin, -812.5);
    put<double>(bike, kWheelRadius, 0.412);
    put<double>(bike, kWheelGap, -0.031);
    put<float>(bike, kWheelSample0, 1.f);
    put<float>(bike, kWheelSample0 + 4, 2.f);
    put<float>(bike, kWheelSample0 + 8, 3.f);
    put<int32_t>(bike, kWheelHit, 1);
    put<double>(bike, kWheelStride + kWheelSpin, inf);
    put<double>(bike, kWheelStride + kWheelRadius, nan);
    put<double>(bike, kWheelStride + kWheelGap, 5.0);
    put<float>(bike, kWheelStride + kWheelSample0, 1e30f);
    put<float>(bike, kWheelStride + kWheelSample0 + 4, (float)nan);
    put<float>(bike, kWheelStride + kWheelSample0 + 8, -1e30f);
    put<int32_t>(bike, kWheelStride + kWheelHit, 0);
    for (int c = 0; c < 3; ++c) {
        put<float>(body, kBodyPos + 4 * c, 10.f + c);
        put<float>(body, kBodyLvel + 4 * c, 20.f + c);
        put<float>(body, kBodyAvel + 4 * c, 30.f + c);
    }
    Step s;
    ReadBike(bike.data(), body.data(), s);
    CHECK(s.bikeTime == 163.47f, "bike time");
    CHECK(s.wheel[0].spin == -812.5 && s.wheel[0].radius == 0.412 && s.wheel[0].gap == -0.031,
          "wheel 0 doubles");
    CHECK(s.wheel[0].sample[0] == 1.f && s.wheel[0].sample[1] == 2.f && s.wheel[0].sample[2] == 3.f,
          "wheel 0 sample");
    CHECK(s.wheel[0].hit == 1 && s.wheel[1].hit == 0, "hit flags");
    CHECK(std::isinf(s.wheel[1].spin) && std::isnan(s.wheel[1].radius), "inf/nan copied as is");
    CHECK(s.wheel[1].sample[0] == 1e30f && std::isnan(s.wheel[1].sample[1]), "huge/nan sample");
    CHECK(s.body == 1 && s.pos[2] == 12.f && s.lvel[0] == 20.f && s.avel[1] == 31.f, "body");
    Step t;
    t.pos[0] = 99;
    ReadBike(bike.data(), nullptr, t);
    CHECK(t.body == 0 && t.pos[0] == 0 && t.lvel[2] == 0, "no body: zeros");
    // The span constants cover every read.
    CHECK(kBikeSpan == kWheelStride + kWheelGap + 8, "span to wheel 1's gap");
}

static void bike_list() {
    // count, then 16-byte items: int id at +4, pointer at +4+8.
    std::vector<uint8_t> list(4 + 16 * 3, 0);
    const uint8_t* ptrs[3] = {(const uint8_t*)0x1000, (const uint8_t*)0x2000, (const uint8_t*)0x3000};
    put<int32_t>(list, 0, 3);
    for (int i = 0; i < 3; ++i) {
        put<int32_t>(list, 4 + 16 * i, 7 + i);
        std::memcpy(list.data() + 4 + 16 * i + 8, &ptrs[i], sizeof(ptrs[i]));
    }
    CHECK(ListCount(list.data()) == 3, "count");
    CHECK(ListItem(list.data(), 0) == ptrs[0] && ListItem(list.data(), 2) == ptrs[2], "items");
    CHECK(!ListItem(list.data(), 3) && !ListItem(list.data(), -1), "out of range");
    CHECK(!ListItem(nullptr, 0) && ListCount(nullptr) == 0, "no list");
}

static void pick() {
    const int32_t h[6] = {5, 9, 7, 3, 8, 6};
    bool on[6] = {true, true, true, true, true, true};
    int out[kMaxTagged];
    bool local = false;
    CHECK(PickBikes(h, on, 6, 7, out, &local) == 1 && out[0] == 2 && local, "own handle found");
    CHECK(PickBikes(h, on, 6, 0, out, &local) == kMaxTagged && !local && out[0] == 0 && out[3] == 3,
          "no own record: first 4 active, not local");
    CHECK(PickBikes(h, on, 6, 99, out, &local) == kMaxTagged && !local, "own not in list: fallback");
    on[2] = false;
    CHECK(PickBikes(h, on, 6, 7, out, &local) == kMaxTagged && !local && out[2] == 3,
          "own inactive: fallback skips inactive");
    bool none[6] = {};
    CHECK(PickBikes(h, none, 6, 0, out, &local) == 0, "nothing active");
    // Offline there is one bike and it is recorded either way.
    CHECK(PickBikes(h, on, 1, 0, out, &local) == 1 && out[0] == 0, "single bike");
    CHECK(kRows == kSteps * kMaxTagged, "2 s even at 4 bikes a step");
}

static void ring() {
    auto r = std::make_unique<Ring<8>>();
    CHECK(r->Count() == 0, "empty");
    for (int i = 1; i <= 5; ++i) { r->Next().step = (uint64_t)i; r->Commit(); }
    CHECK(r->Count() == 5 && r->At(0).step == 1 && r->At(4).step == 5, "partial, oldest first");
    for (int i = 6; i <= 21; ++i) { r->Next().step = (uint64_t)i; r->Commit(); }
    CHECK(r->Count() == 8, "full");
    CHECK(r->At(0).step == 14 && r->At(7).step == 21, "wrapped: last 8, oldest first (got %llu..%llu)",
          (unsigned long long)r->At(0).step, (unsigned long long)r->At(7).step);
    r->Clear();
    CHECK(r->Count() == 0, "cleared");
    // The real one: 1,000 steps, 2 s at 2 ms.
    CHECK(kSteps == 1000, "1000 steps");
}

static void csv() {
    auto r = std::make_unique<Ring<4>>();
    for (int i = 1; i <= 6; ++i) {
        Step& s = r->Next();
        s = Step{};
        s.step = (uint64_t)i;
        s.dt = 0.002f;
        s.slot = 1;
        s.bikes = 1;
        s.index = 0;
        s.body = 1;
        s.lvel[0] = 3.f; s.lvel[1] = 4.f;          // speed 5
        s.wheel[0].spin = 100.0 * i;
        s.wheel[0].radius = 0.4 + 0.01 * i;
        s.wheel[1].spin = i == 6 ? std::numeric_limits<double>::infinity() : 1.0;
        s.wheel[1].radius = i == 6 ? std::numeric_limits<double>::quiet_NaN() : 0.3;
        s.wheel[1].hit = 1;
        s.handle = 4242;
        s.local = 1;
        r->Commit();
    }
    Meta m;
    m.reason = "crash";
    m.version = "0.50.0";
    m.whenUtc = "2026-10-08T18:00:00Z";
    m.track = "trial";
    std::string out;
    WriteCsv(*r, m, collect, &out);
    const auto ls = lines(out);
    CHECK(ls.size() == 3 + 4, "2 meta + header + 4 rows (got %zu)", ls.size());
    if (ls.size() != 7) { std::printf("%s", out.c_str()); return; }
    CHECK(ls[0].rfind("# frostmod nan ring: reason=crash frostmod=0.50.0", 0) == 0, "meta line");
    CHECK(ls[0].find("track=trial") != std::string::npos, "track in meta");
    CHECK(ls[1][0] == '#', "second meta line");
    CHECK(ls[2] == kHeader, "header");
    const int cols = commas(kHeader);
    CHECK(cols == 34, "35 columns (got %d)", cols + 1);
    for (size_t i = 3; i < ls.size(); ++i)
        CHECK(commas(ls[i]) == cols, "row %zu has %d commas", i, commas(ls[i]));
    const auto first = fields(ls[3]), last = fields(ls[6]);
    CHECK(first[1] == "3" && last[1] == "6", "oldest first: steps 3..6 (got %s..%s)",
          first[1].c_str(), last[1].c_str());
    CHECK(last[0] == "0", "newest row at t=0 (got %s)", last[0].c_str());
    CHECK(std::fabs(std::atof(first[0].c_str()) + 6.0) < 1e-3, "oldest at -6 ms (got %s)",
          first[0].c_str());
    CHECK(std::fabs(std::atof(first[2].c_str()) - 2.0) < 1e-4, "dt in ms (got %s)", first[2].c_str());
    CHECK(first[16] == "5", "speed = |lvel| (got %s)", first[16].c_str());
    CHECK(last[21] == "600" && first[21] == "300", "w0 spin (got %s / %s)", first[21].c_str(),
          last[21].c_str());
    CHECK(last[28] == "inf" && last[29] == "nan", "w1 inf/nan spelled out (got %s %s)",
          last[28].c_str(), last[29].c_str());
    CHECK(last[34] == "1", "w1 hit");
    CHECK(last[6] == "4242" && last[7] == "1", "handle and local tag (got %s %s)", last[6].c_str(),
          last[7].c_str());
    // Nothing identifying: only the folder of the track. No rider, server, Steam ID or GUID
    // field exists to be filled.
    CHECK(out.find("7656") == std::string::npos && out.find("rider") == std::string::npos,
          "no ids");

    // Several bikes in one step (the local one unknown): t_ms advances once per step.
    {
        auto m4 = std::make_unique<Ring<16>>();
        for (int st = 1; st <= 3; ++st)
            for (int b = 0; b < 2; ++b) {
                Step& s = m4->Next();
                s = Step{};
                s.step = (uint64_t)st;
                s.dt = 0.002f;
                s.index = (int16_t)b;
                s.handle = 10 + b;
                m4->Commit();
            }
        std::string o4;
        WriteCsv(*m4, m, collect, &o4);
        const auto l4 = lines(o4);
        CHECK(l4.size() == 3 + 6, "6 rows");
        if (l4.size() == 9) {
            const auto r0 = fields(l4[3]), r1 = fields(l4[4]), r5 = fields(l4[8]);
            CHECK(r0[0] == r1[0], "same step, same t (%s vs %s)", r0[0].c_str(), r1[0].c_str());
            CHECK(std::fabs(std::atof(r0[0].c_str()) + 4.0) < 1e-3, "two steps back: -4 ms (got %s)",
                  r0[0].c_str());
            CHECK(r5[0] == "0" && r5[6] == "11" && r5[7] == "0", "last row: t 0, handle 11, local 0");
        }
    }

    std::string empty;
    WriteCsv(Ring<4>{}, m, collect, &empty);
    CHECK(lines(empty).size() == 3, "empty ring: meta + header only");

    char name[96];
    FileName(2026, 10, 8, 18, 5, 9, "refusal", name, sizeof(name));
    CHECK(std::strcmp(name, "frostmod-nan-ring-20261008-180509-refusal.csv") == 0, "file name %s",
          name);
}

// "nanRing" goes in both report shapes when there is a ring, and not at all when there is not.
static void reports() {
    namespace crash = frostmod::crash;
    crash::Context ctx;
    crash::Trail trail;
    crash::Fault f;
    f.kind = "access violation";
    std::string a, b;
    crash::WriteJson(f, ctx, trail, nullptr, 0, 0, collect, &a);
    CHECK(a.find("nanRing") == std::string::npos, "crash JSON: no key without a ring");
    f.ringFile = "frostmod-nan-ring-20261008-180509-crash.csv";
    crash::WriteJson(f, ctx, trail, nullptr, 0, 0, collect, &b);
    CHECK(b.find("\"nanRing\": \"frostmod-nan-ring-20261008-180509-crash.csv\",") != std::string::npos,
          "crash JSON names the ring");
    CHECK(b.size() > a.size() && b.find("\"dump\"") < b.find("\"nanRing\""), "after dump");

    nantrap::Record r;
    nantrap::Meta m;
    std::string c, d;
    nantrap::WriteJson(r, m, ctx, trail, 0, collect, &c);
    CHECK(c.find("nanRing") == std::string::npos, "trap JSON: no key without a ring");
    m.ringFile = "frostmod-nan-ring-20261008-180509-trap.csv";
    nantrap::WriteJson(r, m, ctx, trail, 0, collect, &d);
    CHECK(d.find("\"nanRing\": \"frostmod-nan-ring-20261008-180509-trap.csv\",") != std::string::npos,
          "trap JSON names the ring");
}

// What a step pays: one ReadBike into the ring and a commit. Printed, not asserted tightly; CI
// runners vary. The bound only catches something gone badly wrong.
static void cost() {
    std::vector<uint8_t> bike(kBikeSpan + 64, 0), body(kBodySpan + 16, 0);
    auto r = std::make_unique<Ring<kRows>>();
    const int n = 2000000;
    volatile const uint8_t* vb = bike.data();   // keep the reads from being hoisted
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        Step& s = r->Next();
        s.step = (uint64_t)i;
        s.dt = 0.002f;
        ReadBike((const uint8_t*)vb, body.data(), s);
        r->Commit();
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / n;
    std::printf("tyrelog: %.1f ns per sampled step (%zu-byte row, %zu KB ring), %.4f%% of a 2 ms step\n",
                ns, sizeof(Step), sizeof(Ring<kRows>) / 1024, ns / 2e6 * 100.0);
    CHECK(ns < 2000.0, "a sample under 2 us (got %.1f ns)", ns);

    std::string out;
    out.reserve(400000);
    Meta m;
    const auto t2 = std::chrono::steady_clock::now();
    WriteCsv(*r, m, collect, &out);
    const auto t3 = std::chrono::steady_clock::now();
    std::printf("tyrelog: CSV of %zu rows: %zu KB in %.2f ms (once, at dump time)\n", r->Count(),
                out.size() / 1024, std::chrono::duration<double, std::milli>(t3 - t2).count());
}

int main() {
    cfg();
    checks();
    read_bike();
    bike_list();
    pick();
    ring();
    csv();
    reports();
    cost();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("tyrelog: all checks passed\n");
    return 0;
}
