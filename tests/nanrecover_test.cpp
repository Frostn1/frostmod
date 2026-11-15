// Invariants over the NaN bike reset in nanrecover.h.
//
// The offsets the check reads and the call it makes are proved against the bytes the install
// insists on, rather than restated. The check is run against a fake bike laid out the way the
// game lays one out: it must pass every finite state (random ones, extreme ones, and the last
// calm step of four real crashes from the rings riders sent), and fail every non-finite value
// in every field it covers, naming the field. The snapshot rules (where a bike is put back)
// and the heading convention are checked against the matrix place-bike builds.
//
// On Windows the game's own ODE body setters (dBodySetPosition, dBodySetLinearVel,
// dBodySetAngularVel, dBodyAddForce, dBodyAddTorque; ODE is open source and these are its
// bytes as compiled into the game) are run against the fake bike's bodies: what the game
// writes is what the check reads.

#include "../src/nanrecover.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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

using namespace nanrecover;

static const float kNaN = std::numeric_limits<float>::quiet_NaN();
static const float kInf = std::numeric_limits<float>::infinity();

static int32_t Disp(const char* p) {
    int32_t d = 0;
    for (int i = 0; i < 4; ++i) d |= (int32_t)((uint32_t)(uint8_t)p[i] << (8 * i));
    return d;
}
static const Check* Find(uint32_t rva) {
    for (const Check& c : kChecks)
        if (c.rva == rva) return &c;
    return nullptr;
}
// The target of a `call rel32` check at its first byte.
static uint32_t CallTo(const Check* c) {
    if (!c || (uint8_t)c->bytes[0] != 0xE8) return 0;
    return (uint32_t)((int64_t)c->rva + 5 + Disp(c->bytes + 1));
}

// ---- the bytes ---------------------------------------------------------------------------------
static void TestChecks() {
    for (const Check& c : kChecks) CHECK(c.len > 0 && c.len <= 64 && c.what && c.what[0], "check at 0x%X", c.rva);
    // place-bike, and what it calls first
    const Check* c = Find(0x1970A2);
    CHECK(CallTo(c) == 0x1BF1A0, "place-bike resolves its handle through 0x1BF1A0");
    CHECK(c && (uint8_t)c->bytes[5] == 0x4C && (uint8_t)c->bytes[7] == 0xE0, "and keeps the bike in r12");
    CHECK(CallTo(Find(0x197123)) == 0x196500, "the suspension reset 0x196500 runs first");
    CHECK(0x197123 < 0x197153 && 0x197123 < 0x197181, "before the fork and shock are read");
    CHECK(CallTo(Find(0x197771)) == 0x195D10, "then the bodies are placed (0x195D10)");
    CHECK(CallTo(Find(0x197A45)) == 0x1957D0, "and the rider (0x1957D0)");
    // the fields: r12-based disp32 at byte 6 for movsd, byte 4 for mov rcx, [r12+d]
    CHECK(Disp(Find(0x197153)->bytes + 6) == (int32_t)kForkLen, "fork at 0x4C78");
    CHECK(Disp(Find(0x197181)->bytes + 6) == (int32_t)kShockLen, "shock at 0x5CC8");
    CHECK(Disp(Find(0x197696)->bytes + 4) == (int32_t)kBodyMain, "main body at 0x430");
    CHECK(Disp(Find(0x195D3B)->bytes + 3) == (int32_t)kBodyFrame, "frame body at 0x3780");
    CHECK(Disp(Find(0x195D59)->bytes + 3) == (int32_t)kBodyFront, "front body at 0x4CF0");
    CHECK(Disp(Find(0x195D7C)->bytes + 3) == (int32_t)kBodyRear, "rear body at 0x5DA0");
    CHECK(Disp(Find(0x195D96)->bytes + 3) == (int32_t)kBodyWheel, "wheel 0 body at 0x140B0");
    CHECK(Disp(Find(0x195DAC)->bytes + 3) == (int32_t)(kBodyWheel + kWheelStride), "wheel 1 body one stride on");
    CHECK(Disp(Find(0x195A0B)->bytes + 3) == (int32_t)kBodyRider, "rider body at 0x23480");
    CHECK(Disp(Find(0x195D50)->bytes + 2) == (int32_t)kGateFront && Find(0x195D50)->bytes[6] == kGateFrontNone,
          "cmp [bike+0xBC], 3");
    CHECK(Disp(Find(0x195D73)->bytes + 2) == (int32_t)kGateRear && Find(0x195D73)->bytes[6] == kGateRearNone,
          "cmp [bike+0xF8], 5");
    // what 0x196500 zeroes: shock velocity, and per wheel (base bike+0x60F8) spin and deflection
    CHECK(Disp(Find(0x196B92)->bytes + 3) == (int32_t)kShockVel, "shock velocity zeroed");
    CHECK(Disp(Find(0x196BBD)->bytes + 4) == (int32_t)kShockLen, "shock length rewritten");
    const int32_t base = Disp(Find(0x196D4F)->bytes + 3);
    CHECK(base == 0x60F8, "wheel loop base");
    CHECK(base + Disp(Find(0x196E15)->bytes + 3) == (int32_t)kWheelSpin, "spin zeroed");
    CHECK(base + Disp(Find(0x196DE0)->bytes + 3) == (int32_t)kWheelDefl, "deflection zeroed");
    CHECK((uint8_t)Find(0x1981C7)->bytes[1] == 0xFE && Find(0x1981C7)->bytes[4] == (char)kBikeResets,
          "inc byte [r12+0x3C]: the reset counter");
    // ODE's dxBody
    CHECK(Disp(Find(0x2AC7AA)->bytes + 4) == (int32_t)kBodyPos, "pos");
    CHECK(Disp(Find(0x2AC8C0)->bytes + 3) == (int32_t)kBodyR, "R");
    CHECK(Disp(Find(0x2AC8D0)->bytes + 3) == (int32_t)kBodyQ, "q");
    CHECK(Disp(Find(0x2AC870)->bytes + 4) == (int32_t)kBodyLvel, "lvel");
    CHECK(Disp(Find(0x2AC890)->bytes + 4) == (int32_t)kBodyAvel, "avel");
    CHECK(Disp(Find(0x2AC990)->bytes + 4) == (int32_t)kBodyFacc, "facc");
    CHECK(Disp(Find(0x2AC9D0)->bytes + 4) == (int32_t)kBodyTacc, "tacc");
    CHECK(kBodyQ == kBodyR + 0x30 && kBodyLvel == kBodyQ + 0x10, "dxBody posr then q then lvel");
}

// The bus job: how the game itself calls place-bike. Its argument block holds the handle and
// four doubles; it hands place-bike (ecx = [0], xmm1 = [8], xmm2 = [0x10], xmm3 = [0x18],
// 5th on the stack = [0x20]). The respawn path fills them as (handle, x, z, heading, y)
// (0x22341..0x22385). ResetFn is declared in that order.
static void TestJob() {
    const Check* j = Find(kRvaJob);
    CHECK(j && j->len == 41, "the job is checked whole");
    if (!j) return;
    const uint8_t* b = (const uint8_t*)j->bytes;
    // cvtsd2ss xmm0, [rax+0x20] ; ... ; movss [rsp+0x20], xmm0: the 5th argument
    CHECK(b[8] == 0xF2 && b[11] == 0x40 && b[12] == 0x20, "xmm0 <- [+0x20]");
    CHECK(b[32] == 0x11 && b[33] == 0x44 && b[35] == 0x20, "5th argument <- xmm0");
    CHECK(b[16] == 0x48 && b[17] == 0x08, "xmm1 <- [+8]");
    CHECK(b[21] == 0x50 && b[22] == 0x10, "xmm2 <- [+0x10]");
    CHECK(b[23] == 0x8B && b[24] == 0x08, "ecx <- [+0] (the handle)");
    CHECK(b[28] == 0x58 && b[29] == 0x18, "xmm3 <- [+0x18]");
    const uint32_t to = (uint32_t)((int64_t)(kRvaJob + 36) + 5 + Disp(j->bytes + 37));
    CHECK(b[36] == 0xE8 && to == kRvaReset, "and calls place-bike (got 0x%X)", to);
    // Win64 passes ResetFn's (int, float, float, float, float) in ecx, xmm1, xmm2, xmm3 and
    // [rsp+0x20]: the same slots, so FrostMod's call is the job's call.
}

// ---- a fake bike ----------------------------------------------------------------------------------
struct FakeBike {
    std::vector<uint8_t> mem;
    uint8_t* bike;
    uint8_t* body[7];
    FakeBike() : mem(kBikeSpan + 7 * 0x200 + 64, 0) {
        bike = mem.data();
        const size_t at[7] = {kBodyMain, kBodyFrame, kBodyFront, kBodyRear, kBodyWheel,
                              kBodyWheel + kWheelStride, kBodyRider};
        for (int i = 0; i < 7; ++i) {
            body[i] = mem.data() + kBikeSpan + 0x200 * (size_t)i;
            std::memcpy(bike + at[i], &body[i], sizeof(void*));
        }
        int32_t one = 1;
        std::memcpy(bike + kBikeActive, &one, 4);
        int32_t gate = 0;
        std::memcpy(bike + kGateFront, &gate, 4);
        std::memcpy(bike + kGateRear, &gate, 4);
    }
    void F(uint8_t* p, size_t off, float v) { std::memcpy(p + off, &v, 4); }
    void D(uint8_t* p, size_t off, double v) { std::memcpy(p + off, &v, 8); }
    float& RF(uint8_t* p, size_t off) { return *(float*)(p + off); }
};

static uint32_t g_rng = 12345;
static uint32_t Rand() { g_rng = g_rng * 1664525u + 1013904223u; return g_rng; }
// Any finite float, weighted toward the edges: huge, tiny, denormal, signed zero.
static float AnyFinite() {
    for (;;) {
        uint32_t b = Rand();
        switch (Rand() % 6) {
            case 0: b = (b & 0x807FFFFFu); break;                 // denormal or zero
            case 1: b = (b & 0x80000000u) | 0x7F7FFFFFu; break;   // +-FLT_MAX
            case 2: b = b & 0x80000000u; break;                   // +-0
            default: break;
        }
        float f;
        std::memcpy(&f, &b, 4);
        if (!Bad32(b)) return f;
    }
}
static double AnyFiniteD() {
    for (;;) {
        uint64_t b = ((uint64_t)Rand() << 32) | Rand();
        if (Rand() % 4 == 0) b = (b & 0x8000000000000000ull) | 0x7FEFFFFFFFFFFFFFull;
        double d;
        std::memcpy(&d, &b, 8);
        if (!Bad64(b)) return d;
    }
}

// Every float/double the check covers, as (pointer, is-double, field, where).
struct Slot { uint8_t* p; bool dbl; Field f; uint32_t where; };
static std::vector<Slot> Slots(FakeBike& fb) {
    std::vector<Slot> s;
    const uint32_t off[7] = {(uint32_t)kBodyMain, (uint32_t)kBodyFrame, (uint32_t)kBodyFront,
                             (uint32_t)kBodyRear, (uint32_t)kBodyWheel,
                             (uint32_t)(kBodyWheel + kWheelStride), (uint32_t)kBodyRider};
    for (int i = 0; i < 7; ++i) {
        uint8_t* b = fb.body[i];
        for (int k = 0; k < 3; ++k) s.push_back({b + kBodyPos + 4 * k, false, Field::BodyPos, off[i]});
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) s.push_back({b + kBodyR + 16 * r + 4 * k, false, Field::BodyRot, off[i]});
        for (int k = 0; k < 4; ++k) s.push_back({b + kBodyQ + 4 * k, false, Field::BodyQuat, off[i]});
        for (int k = 0; k < 3; ++k) s.push_back({b + kBodyLvel + 4 * k, false, Field::BodyLvel, off[i]});
        for (int k = 0; k < 3; ++k) s.push_back({b + kBodyAvel + 4 * k, false, Field::BodyAvel, off[i]});
    }
    for (int w = 0; w < kWheels; ++w) {
        uint8_t* p = fb.bike + kWheelStride * (size_t)w;
        s.push_back({p + kWheelSpin, true, Field::WheelSpin, (uint32_t)w});
        s.push_back({p + kWheelRadius, true, Field::WheelRadius, (uint32_t)w});
        for (int k = 0; k < 3; ++k) s.push_back({p + kWheelSample0 + 4 * k, false, Field::WheelSample, (uint32_t)w});
        s.push_back({p + kWheelDefl, true, Field::WheelDefl, (uint32_t)w});
        s.push_back({p + kWheelDeflVel, true, Field::WheelDeflVel, (uint32_t)w});
    }
    s.push_back({fb.bike + kForkLen, true, Field::ForkLen, 0});
    s.push_back({fb.bike + kShockLen, true, Field::ShockLen, 0});
    s.push_back({fb.bike + kShockVel, true, Field::ShockVel, 0});
    return s;
}

static void FillFinite(std::vector<Slot>& s) {
    for (Slot& x : s) {
        if (x.dbl) { const double d = AnyFiniteD(); std::memcpy(x.p, &d, 8); }
        else { const float f = AnyFinite(); std::memcpy(x.p, &f, 4); }
    }
}

static void TestScanFinite() {
    FakeBike fb;
    std::vector<Slot> s = Slots(fb);
    CHECK(s.size() == 7 * 22 + 2 * 7 + 3, "%zu values covered", s.size());
    Bad bad;
    CHECK(Scan(fb.bike, &bad) && bad.field == Field::None, "an all-zero bike is finite");
    int wrong = 0;
    for (int t = 0; t < 20000; ++t) {
        FillFinite(s);
        if (!Scan(fb.bike, &bad)) ++wrong;
    }
    CHECK(wrong == 0, "20000 random finite bikes (FLT_MAX, denormals, -0 included): %d flagged", wrong);
    // What the check does not read may hold anything: ODE's row padding, ints, pointers, the
    // force accumulators, bodies behind a closed gate.
    FillFinite(s);
    for (int i = 0; i < 7; ++i)
        for (int r = 0; r < 3; ++r) fb.F(fb.body[i], kBodyR + 16 * r + 12, kNaN);
    for (int i = 0; i < 7; ++i) { fb.F(fb.body[i], kBodyFacc, kNaN); fb.F(fb.body[i], kBodyTacc, kInf); }
    const uint32_t ones = 0xFFFFFFFFu;
    std::memcpy(fb.bike + kBikeHandle, &ones, 4);   // an int that is a NaN pattern
    CHECK(Scan(fb.bike, &bad), "padding, accumulators and ints are not read");
    // A closed gate: the body pointer there is not followed.
    int32_t gate = kGateFrontNone;
    std::memcpy(fb.bike + kGateFront, &gate, 4);
    fb.F(fb.body[2], kBodyPos, kNaN);
    CHECK(Scan(fb.bike, &bad), "front body behind gate 3 is skipped");
    gate = 0;
    std::memcpy(fb.bike + kGateFront, &gate, 4);
    CHECK(!Scan(fb.bike, &bad) && bad.where == kBodyFront, "and read with the gate open");
    fb.F(fb.body[2], kBodyPos, 1.0f);
    gate = kGateRearNone;
    std::memcpy(fb.bike + kGateRear, &gate, 4);
    fb.F(fb.body[3], kBodyLvel + 4, kInf);
    CHECK(Scan(fb.bike, &bad), "rear body behind gate 5 is skipped");
    gate = 0;
    std::memcpy(fb.bike + kGateRear, &gate, 4);
    fb.F(fb.body[3], kBodyLvel + 4, 0.0f);
    // A null body pointer is skipped.
    const void* null = nullptr;
    std::memcpy(fb.bike + kBodyRider, &null, sizeof(null));
    CHECK(Scan(fb.bike, &bad), "a bike without a rider body");
    CHECK(Scan(nullptr, &bad), "no bike, nothing to do");
}

static void TestScanBad() {
    FakeBike fb;
    std::vector<Slot> s = Slots(fb);
    const uint32_t f32[] = {0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0x7FBFFFFFu, 0x7F800000u, 0xFF800000u};
    const uint64_t f64[] = {0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x7FF0000000000001ull,
                            0x7FF0000000000000ull, 0xFFF0000000000000ull};
    int missed = 0, misnamed = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const int n = s[i].dbl ? 5 : 6;
        for (int k = 0; k < n; ++k) {
            FillFinite(s);
            if (s[i].dbl) std::memcpy(s[i].p, &f64[k], 8);
            else std::memcpy(s[i].p, &f32[k], 4);
            Bad bad;
            if (Scan(fb.bike, &bad)) { ++missed; continue; }
            if (bad.field != s[i].f || bad.where != s[i].where) ++misnamed;
        }
    }
    CHECK(missed == 0, "every non-finite value in every covered field is caught (%d missed)", missed);
    CHECK(misnamed == 0, "and named (%d misnamed)", misnamed);
}

// The last calm step and the NaN step of four real crashes, from the 2 s rings riders' games
// wrote (crash ids in the comments). Main body pos, lvel, avel; per wheel spin, radius, sample.
struct RingStep { float v[19]; };
static const RingStep kRing[][2] = {
    // 14953 washington, online, 10.2 m/s, steps 759730 -> 759731
    {{{272.489044f, 32.1054497f, 489.046448f, -8.05733776f, -5.4842577f, -2.8471477f, -0.674026906f, 2.20417142f, 4.25714111f, -27.2112141f, 0.341933275f, 271.985016f, 31.4624481f, 489.002533f, 31.6916027f, 0.330705654f, 273.352539f, 32.0257034f, 489.622345f}},
     {{kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, 0.341933094f, 271.96817f, 31.4465523f, 489.000244f, kNaN, 0.330704374f, 273.33493f, 32.0210838f, 489.616913f}}},
    // 14917 online, 0.55 m/s, steps 514469 -> 514470
    {{{86.5325699f, 2.81835461f, 144.609238f, -0.199637279f, -0.138219357f, 0.496795267f, 0.64864713f, 0.505411744f, 0.588940263f, 2.06357455f, 0.341533038f, 87.2629166f, 2.83038926f, 144.927017f, 11.3106651f, 0.323380002f, 85.8784714f, 2.61026859f, 144.422684f}},
     {{kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, 0.341532857f, 87.2627716f, 2.83056283f, 144.927536f, kNaN, 0.323380002f, 85.8772812f, 2.60796785f, 144.424866f}}},
    // 14003 offline, standing (0.02 m/s), steps 370390 -> 370391
    {{{229.627899f, 3.35423541f, 192.024765f, -0.000828422373f, -0.00127660111f, 0.0189391598f, -0.00822522491f, 0.0355105475f, 0.00235581398f, -3.02244115f, 0.341553936f, 228.623795f, 3.40523386f, 192.006012f, 0.231967464f, 0.331803537f, 230.303757f, 3.05843019f, 192.594345f}},
     {{kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, 0.341548118f, 228.623764f, 3.40509582f, 192.006256f, kNaN, 0.331803706f, 230.303696f, 3.05841899f, 192.594467f}}},
    // 14887 offline, 4 steps after spawn, steps 3 -> 4
    {{{-291.811707f, 0.369590789f, -33.5807343f, -0.00204896834f, -0.0517848134f, -0.00252811005f, -0.0429898724f, 0.00803459343f, -0.0557350889f, -0.0180010889f, 0.0f, -292.162109f, 0.0377291776f, -32.9781303f, 0.00600000005f, 0.0f, -291.278656f, 0.0411891118f, -34.4973488f}},
     {{kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, 0.0f, -292.162109f, 0.0377803184f, -32.9781303f, kNaN, 0.0f, -291.278656f, 0.0410852432f, -34.4973335f}}},
};

static void Load(FakeBike& fb, const RingStep& r) {
    uint8_t* b = fb.body[0];
    for (int k = 0; k < 3; ++k) {
        fb.F(b, kBodyPos + 4 * k, r.v[k]);
        fb.F(b, kBodyLvel + 4 * k, r.v[3 + k]);
        fb.F(b, kBodyAvel + 4 * k, r.v[6 + k]);
    }
    for (int w = 0; w < 2; ++w) {
        uint8_t* p = fb.bike + kWheelStride * (size_t)w;
        const float* v = r.v + 9 + 5 * w;
        fb.D(p, kWheelSpin, v[0]);
        fb.D(p, kWheelRadius, v[1]);
        for (int k = 0; k < 3; ++k) fb.F(p, kWheelSample0 + 4 * k, v[2 + k]);
    }
}

static void TestRings() {
    for (const auto& ev : kRing) {
        FakeBike fb;
        Load(fb, ev[0]);
        Bad bad;
        CHECK(Scan(fb.bike, &bad), "the calm step before a real crash is left alone");
        // and is a place to put the bike back
        const float* v = ev[0].v;
        CHECK(Sane(v, v + 3, v + 6), "and is sane");
        Load(fb, ev[1]);
        CHECK(!Scan(fb.bike, &bad) && bad.field == Field::BodyPos && bad.where == kBodyMain,
              "the NaN step is caught at the main body");
    }
}

// ---- heading ---------------------------------------------------------------------------------
// The rotation 0x266BA0 makes for heading h (rows (c,0,s) (0,1,0) (-s,0,c)), times a pitch about
// the bike's own x axis, laid out as ODE's 3x4 R.
static void RotYX(float hDeg, float pDeg, float R[12]) {
    const double h = hDeg * 3.14159265358979 / 180.0, p = pDeg * 3.14159265358979 / 180.0;
    const double c = std::cos(h), s = std::sin(h), cp = std::cos(p), sp = std::sin(p);
    const double Y[3][3] = {{c, 0, s}, {0, 1, 0}, {-s, 0, c}};
    const double X[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double m = 0;
            for (int k = 0; k < 3; ++k) m += Y[i][k] * X[k][j];
            R[4 * i + j] = (float)m;
        }
        R[4 * i + 3] = 0;
    }
}

static void TestHeading() {
    float R[12];
    double worst = 0;
    for (int h = -179; h <= 180; h += 7)
        for (int p = -60; p <= 60; p += 15) {
            RotYX((float)h, (float)p, R);
            double d = std::fabs(HeadingDeg(R) - (double)h);
            if (d > 180) d = 360 - d;
            if (d > worst) worst = d;
        }
    CHECK(worst < 1e-3, "heading back from place-bike's own rotation, pitched up to 60 deg: off by %g", worst);
    // Forward (0,0,1) under heading h is (sin h, 0, cos h): the respawn path's atan2(dx, dz).
    RotYX(90.0f, 0.0f, R);
    CHECK(std::fabs(R[2] - 1.0f) < 1e-6f && std::fabs(R[10]) < 1e-6f, "heading 90 faces +x");
    RotYX(0.0f, 90.0f, R);   // nose straight up: forward has no horizontal part
    CHECK(HeadingDeg(R) == HeadingDeg(R), "pointing up still gives a number");
}

// ---- where it goes back to ------------------------------------------------------------------------
static void TestTrack() {
    Track tr;
    tr.Clear((void*)0x1000, 7, 0.0f);
    const float lv[3] = {5, 0, 0}, av[3] = {0, 0, 0};
    float R[12];
    RotYX(30.0f, 0.0f, R);
    CHECK(tr.Pick(kMinAge) == nullptr, "nothing to go back to before the first step");
    // A spawn that goes NaN on its 4th step (crash 14887): the spawn snapshot is used.
    float pos[3] = {-291.8f, 0.37f, -33.58f};
    for (int i = 0; i < 3; ++i) tr.Passed(pos, lv, av, R);
    const Snap* s = tr.Pick(tr.MinAgeForNext());
    CHECK(s && s->pos[0] == pos[0] && std::fabs(s->heading - 30.0f) < 1e-3f, "a bike 3 steps old goes back to its spawn");

    // Riding along x at 1 cm a step for 10 s.
    tr.Clear((void*)0x1000, 7, 0.0f);
    for (int i = 0; i < 5000; ++i) {
        pos[0] = 0.01f * (float)i;
        tr.Passed(pos, lv, av, R);
    }
    s = tr.Pick(tr.MinAgeForNext());
    CHECK(s && tr.steps - s->step >= kMinAge && tr.steps - s->step < kMinAge + kSnapEvery,
          "first reset: the newest snapshot at least 100 ms old (%u steps)", s ? tr.steps - s->step : 0);
    tr.DidReset();
    for (int i = 0; i < 100; ++i) { pos[0] += 0.01f; tr.Passed(pos, lv, av, R); }
    CHECK(tr.MinAgeForNext() == 500, "again within 2 s: 1 s back");
    tr.DidReset();
    for (int i = 0; i < 10; ++i) { pos[0] += 0.01f; tr.Passed(pos, lv, av, R); }
    s = tr.Pick(tr.MinAgeForNext());
    CHECK(s && tr.steps - s->step >= (uint32_t)(kSnaps - 1) * kSnapEvery, "a third time: as far back as it holds");
    // 2 s later it starts over.
    for (int i = 0; i < 1100; ++i) { pos[0] += 0.01f; tr.Passed(pos, lv, av, R); }
    CHECK(tr.MinAgeForNext() == kMinAge, "a quiet 2 s resets the escalation");

    // The repro's runaway: 34 finite steps of exploding speed. None of it is kept.
    tr.Clear((void*)0x1000, 7, 0.0f);
    float p0[3] = {100, 5, 100};
    for (int i = 0; i < 200; ++i) tr.Passed(p0, lv, av, R);
    float lvRun[3] = {0, 0, 0};
    float pRun[3] = {100, 5, 100};
    for (int i = 0; i < 34; ++i) {
        lvRun[1] = 833.0f * std::pow(3.0f, (float)i);
        pRun[1] = 5.0f + 1e3f * std::pow(4.0f, (float)i);
        if (!(lvRun[1] < 3e38f)) lvRun[1] = 3e38f;
        if (!(pRun[1] < 3e38f)) pRun[1] = 3e38f;
        tr.Passed(pRun, lvRun, av, R);
    }
    s = tr.Pick(0);
    CHECK(s && s->pos[1] == 5.0f, "a runaway is never a snapshot: back to y %g", s ? (double)s->pos[1] : -1.0);
    // The game's reset counter: a respawn by the game drops what was kept; our own does not.
    tr.Clear((void*)0x1000, 7, 0.0f);
    float pc[3] = {10, 1, 10};
    tr.SawCounter(4);
    for (int i = 0; i < 300; ++i) { tr.SawCounter(4); tr.Passed(pc, lv, av, R); }
    CHECK(tr.Pick(kMinAge) != nullptr, "snapshots kept while the counter holds");
    tr.DidReset();
    tr.PlacedByUs(5);
    tr.SawCounter(5);
    CHECK(tr.Pick(kMinAge) != nullptr, "our own reset keeps them (a repeat goes further back)");
    tr.SawCounter(6);
    CHECK(tr.Pick(0) == nullptr, "a reset by the game drops them: that place is gone");
    tr.Passed(pc, lv, av, R);
    CHECK(tr.Pick(0) != nullptr, "and the next step starts a new history at once");
    const float spin[3] = {0, 0, 900};
    CHECK(!Sane(p0, lv, spin), "900 rad/s is not sane");
    const float distant[3] = {2e5f, 0, 0};
    CHECK(!Sane(distant, lv, av), "200 km out is not sane");
}

static void TestRates() {
    int lines = 0;
    for (uint64_t n = 1; n <= 30000; ++n) lines += ShouldLog(n) ? 1 : 0;
    CHECK(lines <= 20, "a minute of resets every step logs %d lines", lines);
    CHECK(!ShouldLog(0) && ShouldLog(1) && ShouldLog(3) && !ShouldLog(5) && ShouldLog(8), "1, 2, 3, then 2^k");
}

#ifdef _WIN32
// ---- the game's own ODE setters, on the fake bike's bodies --------------------------------
static const uint8_t kSetPosition[] = {   // 0x2AC7A0 dBodySetPosition(b, x, y, z)
    0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x59, 0x40, 0xf3, 0x0f, 0x11, 0x89, 0xc0, 0x00,
    0x00, 0x00, 0xf3, 0x0f, 0x11, 0x91, 0xc4, 0x00, 0x00, 0x00, 0xf3, 0x0f, 0x11, 0x99, 0xc8, 0x00,
    0x00, 0x00, 0x48, 0x85, 0xdb, 0x74, 0x18, 0x48, 0x8b, 0xcb, 0xe8, 0x01, 0x34, 0x00, 0x00, 0x48,
    0x8b, 0xcb, 0xe8, 0x49, 0x1b, 0x00, 0x00, 0x48, 0x8b, 0xd8, 0x48, 0x85, 0xc0, 0x75, 0xe8, 0x48,
    0x83, 0xc4, 0x20, 0x5b, 0xc3};
static const uint8_t kSetLinearVel[] = {   // 0x2AC870
    0xf3, 0x0f, 0x11, 0x89, 0x10, 0x01, 0x00, 0x00, 0xf3, 0x0f, 0x11, 0x91, 0x14, 0x01, 0x00, 0x00,
    0xf3, 0x0f, 0x11, 0x99, 0x18, 0x01, 0x00, 0x00, 0xc3};
static const uint8_t kSetAngularVel[] = {   // 0x2AC890
    0xf3, 0x0f, 0x11, 0x89, 0x20, 0x01, 0x00, 0x00, 0xf3, 0x0f, 0x11, 0x91, 0x24, 0x01, 0x00, 0x00,
    0xf3, 0x0f, 0x11, 0x99, 0x28, 0x01, 0x00, 0x00, 0xc3};
static const uint8_t kAddForce[] = {   // 0x2AC990
    0xf3, 0x0f, 0x58, 0x89, 0x30, 0x01, 0x00, 0x00, 0xf3, 0x0f, 0x58, 0x91, 0x34, 0x01, 0x00, 0x00,
    0xf3, 0x0f, 0x58, 0x99, 0x38, 0x01, 0x00, 0x00, 0xf3, 0x0f, 0x11, 0x89, 0x30, 0x01, 0x00, 0x00,
    0xf3, 0x0f, 0x11, 0x91, 0x34, 0x01, 0x00, 0x00, 0xf3, 0x0f, 0x11, 0x99, 0x38, 0x01, 0x00, 0x00, 0xc3};
static const uint8_t kAddTorque[] = {   // 0x2AC9D0
    0xf3, 0x0f, 0x58, 0x89, 0x40, 0x01, 0x00, 0x00, 0xf3, 0x0f, 0x58, 0x91, 0x44, 0x01, 0x00, 0x00,
    0xf3, 0x0f, 0x58, 0x99, 0x48, 0x01, 0x00, 0x00, 0xf3, 0x0f, 0x11, 0x89, 0x40, 0x01, 0x00, 0x00,
    0xf3, 0x0f, 0x11, 0x91, 0x44, 0x01, 0x00, 0x00, 0xf3, 0x0f, 0x11, 0x99, 0x48, 0x01, 0x00, 0x00, 0xc3};

using Set3 = void (*)(void* body, float x, float y, float z);

static void TestLive() {
    uint8_t* mem = (uint8_t*)VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    CHECK(mem != nullptr, "VirtualAlloc");
    if (!mem) return;
    size_t o = 0;
    auto put = [&](const uint8_t* b, size_t n) { uint8_t* at = mem + o; std::memcpy(at, b, n); o = (o + n + 15) & ~(size_t)15; return (Set3)(void*)at; };
    const Set3 setPos = put(kSetPosition, sizeof(kSetPosition));
    const Set3 setLvel = put(kSetLinearVel, sizeof(kSetLinearVel));
    const Set3 setAvel = put(kSetAngularVel, sizeof(kSetAngularVel));
    const Set3 addForce = put(kAddForce, sizeof(kAddForce));
    const Set3 addTorque = put(kAddTorque, sizeof(kAddTorque));
    FlushInstructionCache(GetCurrentProcess(), mem, 0x1000);
    // The bytes the install checks are these functions' own.
    CHECK(std::memcmp(kSetPosition + 10, Find(0x2AC7AA)->bytes, 8) == 0, "dBodySetPosition matches its check");
    CHECK(std::memcmp(kSetLinearVel, Find(0x2AC870)->bytes, 8) == 0, "dBodySetLinearVel matches its check");
    CHECK(std::memcmp(kAddForce, Find(0x2AC990)->bytes, 8) == 0, "dBodyAddForce matches its check");

    FakeBike fb;   // geom pointer [body+0x40] is null: dBodySetPosition writes and returns
    Bad bad;
    for (int i = 0; i < 7; ++i) {
        setPos(fb.body[i], 1.0f, 2.0f, 3.0f);
        setLvel(fb.body[i], -4.0f, 5.0f, 6e30f);
        setAvel(fb.body[i], 7.0f, -8.0f, 9.0f);
    }
    CHECK(fb.RF(fb.body[0], kBodyPos + 8) == 3.0f && fb.RF(fb.body[6], kBodyLvel + 8) == 6e30f, "the game's setters wrote where we read");
    CHECK(Scan(fb.bike, &bad), "a bike the game's setters made finite passes");
    addForce(fb.body[1], kNaN, 0, 0);
    addTorque(fb.body[1], 0, kInf, 0);
    CHECK(Scan(fb.bike, &bad), "a NaN force (cleared by the step, and by us before a reset) is not a reset");
    for (int i = 0; i < 7; ++i) {
        setLvel(fb.body[i], 0, kNaN, 0);
        CHECK(!Scan(fb.bike, &bad) && bad.field == Field::BodyLvel, "dBodySetLinearVel NaN on body %d", i);
        setLvel(fb.body[i], 0, 0, 0);
        setAvel(fb.body[i], -kInf, 0, 0);
        CHECK(!Scan(fb.bike, &bad) && bad.field == Field::BodyAvel, "dBodySetAngularVel -inf on body %d", i);
        setAvel(fb.body[i], 0, 0, 0);
        setPos(fb.body[i], 0, 0, kInf);
        CHECK(!Scan(fb.bike, &bad) && bad.field == Field::BodyPos, "dBodySetPosition inf on body %d", i);
        setPos(fb.body[i], 0, 0, 0);
    }
    CHECK(Scan(fb.bike, &bad), "and finite again once the game writes numbers");

    // The cost, on this machine: the check of a finite bike.
    LARGE_INTEGER f{}, a{}, b{};
    QueryPerformanceFrequency(&f);
    const int reps = 200000;
    int pass = 0;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < reps; ++i) pass += Scan(fb.bike, nullptr) ? 1 : 0;
    QueryPerformanceCounter(&b);
    const double ns = (double)(b.QuadPart - a.QuadPart) * 1e9 / (double)f.QuadPart / reps;
    CHECK(pass == reps, "all passes");
    std::printf("nanrecover: one check of a finite bike costs %.0f ns here (a step is 2,000,000 ns)\n", ns);
    CHECK(ns < 5000.0, "the check is cheap (%.0f ns)", ns);
    VirtualFree(mem, 0, MEM_RELEASE);
}
#endif

int main() {
    TestChecks();
    TestJob();
    TestScanFinite();
    TestScanBad();
    TestRings();
    TestHeading();
    TestTrack();
    TestRates();
#ifdef _WIN32
    TestLive();
#endif
    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("nanrecover: all checks passed\n");
    return 0;
}
