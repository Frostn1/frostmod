// nanwatch (src/nanwatch.h) and the workshop additions to nantrap/tyrelog: the cfg keys, the
// overflow MXCSR, the spawn 0*inf match, the skip list, the extra fields read off a bike and an
// ODE world laid out like the game's, the check (non-finite first, then limits), the tyre-flat
// episodes, the stress kick, the longer ring and the extra CSV columns. Runs anywhere.

#include "../src/nanwatch.h"
#include "../src/nantrap.h"

#include <chrono>
#include <cmath>
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

using namespace nanwatch;
static const double kNaN = std::numeric_limits<double>::quiet_NaN();
static const double kInf = std::numeric_limits<double>::infinity();

template <class T>
static void put(std::vector<uint8_t>& m, size_t off, T v) {
    std::memcpy(m.data() + off, &v, sizeof(v));
}
static int commas(const std::string& s) {
    int n = 0;
    for (char c : s) n += c == ',';
    return n;
}
static void collect(void* user, const char* line) {
    auto* s = (std::string*)user;
    *s += line;
    *s += "\n";
}

static void cfg() {
    Config c;
    CHECK(!c.Any() && !c.watch && !c.flat && !c.stress, "all off by default");
    CHECK(c.ring == 1000, "ring default 1000 steps");
    CHECK(ParseCfgLine("nanwatch=1", c) && c.watch && c.Any(), "nanwatch=1");
    CHECK(ParseCfgLine("nanwatch=2", c) && !c.watch, "only exactly 1 turns it on");
    CHECK(ParseCfgLine("nanring=5000", c) && c.ring == 5000, "nanring");
    CHECK(ParseCfgLine("nanring=99999999", c) && c.ring == kRingMax, "nanring clamped high");
    CHECK(ParseCfgLine("nanring=10", c) && c.ring == kRingMin, "nanring clamped low");
    CHECK(ParseCfgLine("  nanflat = 1\n", c) && c.flat, "nanflat, spaced");
    CHECK(ParseCfgLine("nanflatpct=80", c) && c.flatPct == 80 && c.flat, "nanflatpct does not touch nanflat");
    CHECK(ParseCfgLine("nanstressms=2500", c) && c.stressMs == 2500 && !c.stress, "nanstressms is not nanstress");
    CHECK(ParseCfgLine("nanstress=1", c) && c.stress, "nanstress");
    CHECK(ParseCfgLine("nanstressv=12", c) && c.stressV == 12, "nanstressv");
    CHECK(ParseCfgLine("nanstressspin=4", c) && c.stressSpin == 4, "nanstressspin");
    CHECK(ParseCfgLine("nanstressdrop=3", c) && c.stressDrop == 3, "nanstressdrop");
    CHECK(ParseCfgLine("nanstresswheel=400", c) && c.stressWheel == 400, "nanstresswheel");
    CHECK(!ParseCfgLine("nantrap=1", c) && !ParseCfgLine("nanwatch", c) && !ParseCfgLine("nanwatch=x", c),
          "not ours / no value");
    CHECK(!ParseCfgLine(nullptr, c), "null");
    // A save writes back exactly what differs, and reading that back gives the same config.
    char lines[16][40];
    const int n = CfgLines(c, lines, 16);
    Config d;
    for (int i = 0; i < n; ++i) CHECK(ParseCfgLine(lines[i], d), "round trip line %s", lines[i]);
    CHECK(d.watch == c.watch && d.ring == c.ring && d.flat == c.flat && d.flatPct == c.flatPct &&
              d.stress == c.stress && d.stressMs == c.stressMs && d.stressV == c.stressV &&
              d.stressSpin == c.stressSpin && d.stressDrop == c.stressDrop && d.stressWheel == c.stressWheel,
          "round trip");
    CHECK(CfgLines(Config{}, lines, 16) == 0, "defaults write nothing");
}

static void trapcfg() {
    nantrap::Config c;
    CHECK(!c.overflow && c.skips == 0, "defaults");
    CHECK(nantrap::ParseCfgLine("nantrapov=1", c) && c.overflow && !c.on, "nantrapov does not turn nantrap on");
    CHECK(nantrap::ParseCfgLine("nantrap=1", c) && c.on && c.overflow, "nantrap=1 keeps overflow");
    CHECK(nantrap::ParseCfgLine("nantrapskip=0x1A0A8D, 1ad83d,0X2", c) && c.skips == 3 && c.skip[0] == 0x1A0A8D &&
              c.skip[1] == 0x1AD83D && c.skip[2] == 2,
          "skip list (got %d: %X %X %X)", c.skips, c.skip[0], c.skip[1], c.skip[2]);
    CHECK(nantrap::Skipped(c, 0x1AD83D) && !nantrap::Skipped(c, 0x1AD83E), "Skipped");
    CHECK(nantrap::ParseCfgLine("nantrapmax=4", c) && c.max == 4 && c.on, "nantrapmax still parsed");
    // The MXCSR: overflow unmasked only when asked, and the resume masks both.
    const uint32_t game = 0x1F80 | 0x3F;
    CHECK(nantrap::ArmedMxcsr(game) == ((0x1F80 & ~nantrap::kIM)), "invalid only (got %X)", nantrap::ArmedMxcsr(game));
    CHECK(nantrap::ArmedMxcsr(game, true) == (0x1F80 & ~nantrap::kIM & ~nantrap::kOM), "invalid + overflow");
    CHECK((nantrap::ResumeMxcsr(0x1F80 & ~nantrap::kIM & ~nantrap::kOM) & (nantrap::kIM | nantrap::kOM)) ==
              (nantrap::kIM | nantrap::kOM),
          "resume masks both");
    CHECK(nantrap::IsTrapCode(nantrap::kStatusOverflow) && std::strcmp(nantrap::TrapName(0xC0000091u), "overflow") == 0,
          "overflow code");
    CHECK(std::strcmp(nantrap::TrapName(0xC0000090u), "invalid") == 0, "invalid code name unchanged");
}

static void spawn_scale() {
    // The bytes at 0x267CF6 and the 11 before the return address 0x19F6A0, as in beta21e.
    const uint8_t at[] = {0xF3, 0x0F, 0x59, 0x02, 0xF3, 0x0F, 0x11, 0x01, 0xF3, 0x0F, 0x59, 0x4A, 0x04, 0xF3};
    const uint8_t caller[] = {0xF3, 0x41, 0x0F, 0x5E, 0x14, 0x24, 0xE8, 0x50, 0x86, 0x0C, 0x00};
    CHECK(nantrap::IsSpawnScale(at, sizeof(at), caller, sizeof(caller)), "the spawn 0*inf matches");
    uint8_t other[sizeof(caller)];
    std::memcpy(other, caller, sizeof(caller));
    other[2] = 0x59;   // mulss, not divss: someone else calling the scale
    CHECK(!nantrap::IsSpawnScale(at, sizeof(at), other, sizeof(other)), "another caller still traps");
    uint8_t moved[sizeof(caller)];
    std::memcpy(moved, caller, sizeof(caller));
    moved[7] = 0x11;   // the call's rel32 is not compared: a rebuilt exe still matches
    CHECK(nantrap::IsSpawnScale(at, sizeof(at), moved, sizeof(moved)), "rel32 ignored");
    uint8_t notScale[sizeof(at)];
    std::memcpy(notScale, at, sizeof(at));
    notScale[2] = 0x5E;
    CHECK(!nantrap::IsSpawnScale(notScale, sizeof(notScale), caller, sizeof(caller)), "other op at rip");
    CHECK(!nantrap::IsSpawnScale(at, 4, caller, sizeof(caller)), "too few bytes");
}

// A bike, a tyre geometry block and an ODE world with three bodies, laid out like the game's.
struct Fake {
    std::vector<uint8_t> bike, geom, world;
    std::vector<std::vector<uint8_t>> bodies;
    Fake() : bike(std::max(nanwatch::kBikeSpan, tyrelog::kBikeSpan) + 64, 0), geom(0x100, 0), world(0x40, 0) {
        put(geom, kGeomRadius, 0.33);
        put(geom, kGeomTorus, 0.075);
        for (int w = 0; w < 2; ++w) {
            const size_t b = tyrelog::kWheelStride * (size_t)w;
            put(bike, b + kWheelDefl, -0.03 * (w + 1));
            put(bike, b + kWheelDeflRate, 1.5);
            put(bike, b + kWheelContact, 1);
            put(bike, b + kWheelTyreGeom, (const uint8_t*)geom.data());
            put(bike, b + tyrelog::kWheelRadius, 0.33);
        }
        put(bike, kShockExt, 0.05);
        put(bike, kShockVel, -0.4);
        put(bike, kShockStroke, 0.12);
        put(bike, kBumpLen, 0.015f);
        put(bike, kBumpPen, 0.0f);
        bodies.assign(3, std::vector<uint8_t>(0x200, 0));
        for (int i = 0; i < 3; ++i) {
            put(bodies[i], kBodyWorld, (const uint8_t*)world.data());
            put(bodies[i], kBodyNext, i + 1 < 3 ? (const uint8_t*)bodies[i + 1].data() : (const uint8_t*)nullptr);
            const float p[3] = {1.0f * i, 2.0f, 3.0f}, v[3] = {0.0f, -2.0f * i, 0.0f}, a[3] = {0, 0, 10.0f * i};
            std::memcpy(bodies[i].data() + tyrelog::kBodyPos, p, 12);
            std::memcpy(bodies[i].data() + tyrelog::kBodyLvel, v, 12);
            std::memcpy(bodies[i].data() + tyrelog::kBodyAvel, a, 12);
        }
        put(world, kWorldFirst, (const uint8_t*)bodies[0].data());
        put(world, kWorldCount, (int32_t)3);
    }
};

static void read_extra() {
    Fake f;
    const uint8_t* bl[kMaxBodies];
    const int nb = WorldBodies(f.bodies[1].data(), bl, kMaxBodies);
    CHECK(nb == 3 && bl[0] == f.bodies[0].data() && bl[2] == f.bodies[2].data(), "world walk from any body (%d)", nb);
    Extra x;
    ReadExtra(f.bike.data(), bl, nb, x);
    CHECK(x.have == 1, "have");
    CHECK(x.wheel[0].defl == -0.03 && x.wheel[1].defl == -0.06 && x.wheel[1].torus == 0.075 &&
              x.wheel[0].radius0 == 0.33 && x.wheel[0].contact == 1,
          "wheels");
    CHECK(x.shockExt == 0.05 && x.shockVel == -0.4 && x.stroke == 0.12 && x.bumpLen == 0.015f, "shock");
    CHECK(x.bodies.n == 3 && x.bodies.nonfinite == 0 && x.bodies.iVel == 2 && x.bodies.maxVel == 4.0f &&
              x.bodies.iAvel == 2 && x.bodies.maxAvel == 20.0f,
          "bodies summary");
    // A NaN in body 1: counted, named, and kept out of the maxima.
    put(f.bodies[1], tyrelog::kBodyLvel + 4, std::numeric_limits<float>::quiet_NaN());
    ReadExtra(f.bike.data(), bl, nb, x);
    CHECK(x.bodies.nonfinite == 1 && x.bodies.firstBad == 1 && x.bodies.iVel == 2, "bad body");
    // Broken worlds: nothing walked.
    put(f.world, kWorldCount, (int32_t)0);
    CHECK(WorldBodies(f.bodies[0].data(), bl, kMaxBodies) == 0, "empty world");
    put(f.world, kWorldCount, (int32_t)(kMaxBodies + 1));
    CHECK(WorldBodies(f.bodies[0].data(), bl, kMaxBodies) == 0, "too many: not a bike's world");
    CHECK(WorldBodies(nullptr, bl, kMaxBodies) == 0, "no body");
}

static void angle() {
    CHECK(ContactAngle(0.33, 0.0, 0.075) == 0.0, "no deflection: 0");
    const double a = ContactAngle(0.33, -0.03, 0.075);
    CHECK(std::fabs(a - std::acos(0.30 / 0.33) * 57.29577951308232) < 1e-9, "acos((R-d)/R) (got %g)", a);
    const double clipped = ContactAngle(0.33, -0.2, 0.075);
    CHECK(std::fabs(clipped - std::acos((0.33 - 0.075) / 0.33) * 57.29577951308232) < 1e-9, "clipped at torus");
    CHECK(std::isnan(ContactAngle(0.33, kNaN, 0.075)), "nan in, nan out");
    CHECK(std::fabs(FlatRatio(-0.0675, 0.075) - 0.9) < 1e-12, "ratio");
}

static tyrelog::Step sane() {
    tyrelog::Step s;
    s.body = 1;
    s.pos[0] = 100; s.pos[1] = 2; s.pos[2] = -50;
    s.lvel[2] = 15;
    for (int w = 0; w < 2; ++w) {
        s.wheel[w].spin = 45;
        s.wheel[w].radius = 0.33;
        s.wheel[w].gap = 0.01;
        s.wheel[w].sample[0] = 100;
    }
    return s;
}

static void check() {
    Fake f;
    const uint8_t* bl[kMaxBodies];
    const int nb = WorldBodies(f.bodies[0].data(), bl, kMaxBodies);
    Extra x;
    ReadExtra(f.bike.data(), bl, nb, x);
    Limits L;
    Trip t[16];
    tyrelog::Step s = sane();
    CHECK(Check(s, x, L, t, 16) == 0, "a sane step trips nothing (got %d: %s)", Check(s, x, L, t, 16), t[0].field);

    // PiBoSo's case: the shock past the bump stop (negative length) is a limit trip.
    x.shockExt = -0.002;
    int n = Check(s, x, L, t, 16);
    CHECK(n == 1 && std::strcmp(t[0].field, "shock_ext") == 0 && !t[0].nonfinite && t[0].limit == 0.0,
          "shock_ext < 0 trips (%d %s)", n, n ? t[0].field : "");
    x.shockExt = 0.05;

    // Non-finite comes first even when a limit trips in an earlier field.
    s.lvel[0] = 900.0f;
    s.wheel[1].spin = kNaN;
    n = Check(s, x, L, t, 16);
    CHECK(n == 2 && t[0].nonfinite && std::strcmp(t[0].field, "w1_spin") == 0 && !t[1].nonfinite &&
              std::strcmp(t[1].field, "lvel_x") == 0,
          "non-finite first (%d: %s %s)", n, n ? t[0].field : "", n > 1 ? t[1].field : "");
    char text[96], kind[40];
    TripText(t[0], text, sizeof(text));
    CHECK(std::strcmp(text, "w1_spin=nan (non-finite)") == 0, "text %s", text);
    TripText(t[1], text, sizeof(text));
    CHECK(std::strcmp(text, "lvel_x=900 (limit 250)") == 0, "text %s", text);
    TripKind(t[0], kind, sizeof(kind));
    CHECK(std::strcmp(kind, "nonfinite:w1_spin") == 0, "kind %s", kind);

    // A sample radius running away with spin (the old cause-1 theory) is a limit trip.
    s = sane();
    s.wheel[0].radius = 0.45;
    n = Check(s, x, L, t, 16);
    CHECK(n == 1 && std::strcmp(t[0].field, "w0_radius") == 0, "radius growth (%s)", n ? t[0].field : "");

    // Tyre past flat, a deflection that is infinite, and a bad body.
    s = sane();
    x.wheel[1].defl = -0.09;   // 1.2 x torus
    x.wheel[0].defl = -kInf;
    put(f.bodies[2], tyrelog::kBodyPos, std::numeric_limits<float>::infinity());
    Summarise(bl, nb, x.bodies);
    n = Check(s, x, L, t, 16);
    bool sawDefl = false, sawBody = false, sawFlat = false;
    for (int i = 0; i < n; ++i) {
        sawDefl |= std::strcmp(t[i].field, "w0_defl") == 0 && t[i].nonfinite;
        sawBody |= std::strcmp(t[i].field, "body2") == 0 && t[i].nonfinite;
        sawFlat |= std::strcmp(t[i].field, "w1_flat") == 0 && !t[i].nonfinite;
    }
    CHECK(sawDefl && sawBody && sawFlat, "defl %d body %d flat %d (n=%d)", sawDefl, sawBody, sawFlat, n);
    Trip bt;
    std::snprintf(bt.field, sizeof(bt.field), "body7_vel");
    TripKind(bt, kind, sizeof(kind));
    CHECK(std::strcmp(kind, "limit:body_vel") == 0, "body kinds fold (%s)", kind);
}

static void flat() {
    Fake f;
    const uint8_t* bl[kMaxBodies];
    const int nb = WorldBodies(f.bodies[0].data(), bl, kMaxBodies);
    Extra x;
    ReadExtra(f.bike.data(), bl, nb, x);
    FlatTracker tr;
    Episode e;
    tyrelog::Step s = sane();
    const double defl[] = {-0.03, -0.069, -0.074, -0.0749, -0.07, -0.065, -0.02, -0.01};
    int ended = 0;
    for (int i = 0; i < 8; ++i) {
        x.wheel[1].defl = defl[i];
        s.bikeTime = 1.0f + 0.002f * i;
        s.wheel[1].spin = 40 + i;
        if (tr.Update(1, 100 + i, s, x, 0.9, e)) ++ended;
    }
    CHECK(ended == 1 && tr.episodes == 1, "one episode (%d)", ended);
    CHECK(e.wheel == 1 && e.startStep == 101 && e.endStep == 106, "steps %llu..%llu",
          (unsigned long long)e.startStep, (unsigned long long)e.endStep);
    CHECK(std::fabs(e.peakRatio - 0.0749 / 0.075) < 1e-9 && e.spinAtPeak == 43, "peak %g spin %g", e.peakRatio, e.spinAtPeak);
    CHECK(!e.nonfinite, "finite");
    char row[512];
    FlatCsv(e, row, sizeof(row));
    CHECK(commas(row) == commas(kFlatHeader), "flat csv columns (%d vs %d): %s", commas(row), commas(kFlatHeader), row);
    // Hysteresis: 0.87 stays in, 0.84 leaves.
    FlatTracker h;
    x.wheel[0].defl = -0.0690; CHECK(!h.Update(0, 1, s, x, 0.9, e) && h.in, "in at 0.92");
    x.wheel[0].defl = -0.0653; CHECK(!h.Update(0, 2, s, x, 0.9, e) && h.in, "0.87: still in");
    x.wheel[0].defl = -0.0630; CHECK(h.Update(0, 3, s, x, 0.9, e) && !h.in, "0.84: out");
    // A NaN opens an episode and marks it.
    FlatTracker q;
    x.wheel[0].defl = kNaN;
    CHECK(!q.Update(0, 1, s, x, 0.9, e) && q.in && q.cur.nonfinite, "nan episode");
}

static void stress() {
    // Rigid: two bodies keep their relative velocity under a pure lift, and the turn adds w x r.
    const float c[3] = {0, 1, 0};
    float p1[3] = {0, 1, 0}, v1[3] = {1, 0, 0}, a1[3] = {0, 0, 0};
    float p2[3] = {0, 1, 1}, v2[3] = {1, 0, 0}, a2[3] = {0, 0, 0};
    const float dv[3] = {0, 9, 0}, w[3] = {2, 0, 0};
    ApplyKick(p1, v1, a1, c, dv, w);
    ApplyKick(p2, v2, a2, c, dv, w);
    CHECK(v1[0] == 1 && v1[1] == 9 && v1[2] == 0, "centre body: just the lift");
    // w x r = (2,0,0) x (0,0,1) = (0*1-0*0, 0*0-2*1, 0) = (0,-2,0)
    CHECK(v2[1] == 7 && v2[2] == 0 && a2[0] == 2 && a1[0] == 2, "w x r and avel (%g %g)", v2[1], v2[2]);
    // The throw axes come from the chassis rotation's columns.
    const float R[12] = {0, 0, 1, 0,  0, 1, 0, 0,  -1, 0, 0, 0};   // local x -> world -z, local z -> world x
    float om[3];
    ThrowOmega(Throw::NoseDown, R, 3, om);
    CHECK(om[0] == 0 && om[1] == 0 && om[2] == -3, "pitch about local x (%g %g %g)", om[0], om[1], om[2]);
    ThrowOmega(Throw::RollLeft, R, 3, om);
    CHECK(om[0] == 3 && om[2] == 0, "roll about local z");
    ThrowOmega(Throw::Flat, R, 3, om);
    CHECK(om[0] == 0 && om[1] == 0 && om[2] == 0, "flat: no turn");
    CHECK(ThrowAt(0) == Throw::Flat && ThrowAt(5) == Throw::Flat && ThrowAt(3) == Throw::RollLeft, "cycle");
    // The wheel body: R from the first sample.
    const float pos[3][3] = {{0, 1, 0}, {0, 0.35f, 0.7f}, {0, 0.33f, -0.75f}};
    const float s0[3] = {0, 0.0f, -0.75f};
    CHECK(FindWheelBody(s0, 0.33, pos, 3, 0.03f) == 2, "rear wheel body");
    CHECK(FindWheelBody(s0, 0.5, pos, 3, 0.03f) == -1, "none within tolerance");
}

static void ring_csv() {
    auto r = std::make_unique<tyrelog::Ring<64>>();
    r->SetCap(8);
    for (int i = 1; i <= 20; ++i) { r->Next().step = (uint64_t)i; r->Commit(); }
    CHECK(r->Count() == 8 && r->At(0).step == 13 && r->At(7).step == 20, "cap 8 of 64");
    CHECK(r->Slot(0) == 20 % 8 && r->NextSlot() == 20 % 8, "slots");
    r->SetCap(1000);
    CHECK(r->cap == 64 && r->Count() == 0, "cap clamped, cleared");
    // Longer than the static ring: an outside, zeroed array.
    std::vector<tyrelog::Step> outside(300);
    r->SetCap(300, outside.data());
    for (int i = 1; i <= 310; ++i) { r->Next().step = (uint64_t)i; r->Commit(); }
    CHECK(r->cap == 300 && r->Count() == 300 && r->At(0).step == 11 && r->At(299).step == 310 &&
              outside[310 % 300 - 1].step == 310 && r->buf[0].step != 310,
          "outside rows used (%llu..%llu)", (unsigned long long)r->At(0).step, (unsigned long long)r->At(299).step);
    r->SetCap(8);
    CHECK(r->ext == nullptr && r->cap == 8, "back to its own rows");

    // Extra columns: the header and every row grow by the same count; maxSteps trims.
    static Extra extra[16];
    auto q = std::make_unique<tyrelog::Ring<16>>();
    for (int i = 1; i <= 10; ++i) {
        const size_t slot = q->NextSlot();
        tyrelog::Step& s = q->Next();
        s = sane();
        s.step = (uint64_t)i;
        s.dt = 0.002f;
        extra[slot] = Extra{};
        extra[slot].have = 1;
        extra[slot].shockExt = 0.01 * i;
        extra[slot].wheel[1].torus = 0.075;
        extra[slot].wheel[1].defl = i == 10 ? kNaN : -0.07;
        q->Commit();
    }
    tyrelog::Meta m;
    m.reason = "watch-nonfinite";
    m.extraHeader = kExtraHeader;
    m.extraRow = [](size_t slot, char* out, size_t n) { ExtraCsv(sane(), extra[slot], out, n); };
    m.maxSteps = 4;
    std::string out;
    tyrelog::WriteCsv(*q, m, collect, &out);
    std::vector<std::string> ls;
    for (size_t a = 0; a < out.size();) {
        size_t b = out.find('\n', a);
        ls.push_back(out.substr(a, b - a));
        a = b + 1;
    }
    CHECK(ls.size() == 3 + 4, "maxSteps=4: four rows (got %zu)", ls.size());
    if (ls.size() == 7) {
        const int cols = commas(ls[2]);
        CHECK(cols == commas(tyrelog::kHeader) + 1 + commas(kExtraHeader), "header grew (%d)", cols);
        for (size_t i = 3; i < ls.size(); ++i) CHECK(commas(ls[i]) == cols, "row %zu: %d vs %d", i, commas(ls[i]), cols);
        CHECK(ls[3].rfind("-6", 0) == 0 && ls[3].substr(ls[3].find(',') + 1, 2) == "7,",
              "oldest kept row is step 7 at -6 ms: %s", ls[3].substr(0, 24).c_str());
        CHECK(ls[6].find(",nan,") != std::string::npos, "the NaN deflection is spelled out");
        CHECK(ls[2].find("shock_ext") != std::string::npos && ls[2].find("w1_contact_deg") != std::string::npos,
              "extra names");
    }
    // Without the extra writer: the fixed 35 columns, as before.
    std::string plain;
    tyrelog::WriteCsv(*q, tyrelog::Meta{}, collect, &plain);
    CHECK(plain.find(kExtraHeader) == std::string::npos, "plain CSV unchanged");
}

// What a clean step pays for the check (every step but a few). Printed; the bound only
// catches something gone badly wrong.
static void cost() {
    Fake f;
    const uint8_t* bl[kMaxBodies];
    const int nb = WorldBodies(f.bodies[0].data(), bl, kMaxBodies);
    Extra x;
    tyrelog::Step s = sane();
    Limits L;
    Trip t[16];
    const int n = 200000;
    int trips = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        ReadExtra(f.bike.data(), bl, nb, x);
        trips += Check(s, x, L, t, 16);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / n;
    std::printf("nanwatch: %.0f ns per clean step (read + check, %d bodies), %.3f%% of a 2 ms step\n", ns, nb,
                ns / 2e6 * 100.0);
    CHECK(trips == 0 && ns < 20000.0, "clean and cheap (%d trips, %.0f ns)", trips, ns);
}

int main() {
    cfg();
    trapcfg();
    spawn_scale();
    read_extra();
    angle();
    check();
    flat();
    stress();
    ring_csv();
    cost();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("nanwatch: all checks passed\n");
    return 0;
}
