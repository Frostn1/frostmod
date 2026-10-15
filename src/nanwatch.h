// nanwatch: the NaN workshop. Diagnostic only, every key off by default.
//
// nantrap (src/nantrap.h) catches the first invalid FP op inside the sim step, and tyrelog
// (src/tyrelog.h) keeps the last 2 s of the bike's tyres. Neither answers the question the
// crash leaves open: WHICH value goes bad first, and what the bike was doing. This adds:
//
//   nanwatch=1     After every sim step, check the player's bike: chassis position, velocity
//                  and angular velocity, every ODE body in its world, both wheels' spin, tyre
//                  sample radius, sample point, ground gap, tyre deflection and its rate, the
//                  rear shock's extension and velocity and the bump-stop penetration. The
//                  first value that is not finite, and the first that passes a sanity limit,
//                  are logged with the step number and the values, and the ring (the previous
//                  N steps) is written next to the log.
//   nanring=N      Steps the ring keeps (default 1000 = 2 s; up to 15000 = 30 s).
//   nanflat=1      Logs every episode of a tyre near full deflection (PiBoSo's suspect): when
//                  it started, how long, the peak deflection over the tyre's section height,
//                  the contact half-angle, spin, sample radius and the shock, one CSV row each.
//   nanflatpct=N   What counts as near full (default 90 = 90 % of TorusRadius).
//   nanstress=1    Rider-free stress, OFFLINE only: every nanstressms of bike time the bike
//                  (every body near it, as one rigid body, so no joint is torn) is thrown up
//                  at nanstressv m/s with a rotation of nanstressspin rad/s that cycles
//                  through flat, nose down, nose up, left and right, so it lands flat or
//                  sideways. nanstressdrop=H also lifts it H metres first. nanstresswheel=W
//                  adds W rad/s of wheel spin just before it lands.
//
// The game layout, read off mxbikes.exe beta21e (RVAs). Each is checked against the running
// exe's bytes before anything reads it (kChecks); one mismatch and the extra fields stay off.
//   tyre block      = wheel + 0x60F8, passed as rdi to the tyre function 0x1A2FB0 (0x1A2FEA)
//   deflection      = double [wheel + 0x12878]: radial tyre deflection, negative = squashed
//                     (0x1A725E stores it; 0x1A3F6E reads it as -[rdi+0xC780] and clips it to
//                     TorusRadius before acos((R - d) / R), the contact half-angle, 0x1A3FBD)
//   deflection rate = double [wheel + 0x12880] (0x1A7267)
//   contact         = int [wheel + 0x12824], 0 with no ground (0x1A7677 zeroes the block)
//   tyre geometry   = [wheel + 0x120E8] (0x1A3F67 mov r13, [rdi+0xBFF0]); Radius double +0xA0,
//                     TorusRadius double +0xA8 (the .tyre loader, 0x18F85E / 0x18F875)
//   shock extension = double [bike + 0x5CC8] (0x1A2F23), velocity double [bike + 0x5CD0]
//                     (0x1A2F2C), stroke (.geom rear_length) double [bike + 0x5180] (0x1A2B20)
//   bump stop       = length float [bike + 0x5C4C] (0x1A2D54), penetration in mm, float
//                     [bike + 0x5C60] (0x1A2DF3), 0 outside it (0x1A2DFD)
//   ODE body        = world [body + 0x08], next [body + 0x10] (0x2AC745, 0x2ADD03)
//   ODE world       = first body [world + 0x20], body count [world + 0x30] (0x2ADCFB, 0x2ADD1D)
//   dBodySetPosition 0x2AC7A0 (moves its geoms too); velocities are plain stores, as
//   dBodySetLinearVel 0x2AC870 / dBodySetAngularVel 0x2AC890 do.
//
// Everything in this file is pure: tests/nanwatch_test.cpp runs it on a fake bike.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "tyrelog.h"

namespace nanwatch {

// ---- layout --------------------------------------------------------------------------------
inline constexpr size_t kWheelDefl     = 0x12878;
inline constexpr size_t kWheelDeflRate = 0x12880;
inline constexpr size_t kWheelContact  = 0x12824;
inline constexpr size_t kWheelTyreGeom = 0x120E8;
inline constexpr size_t kGeomRadius    = 0xA0;
inline constexpr size_t kGeomTorus     = 0xA8;
inline constexpr size_t kShockExt      = 0x5CC8;
inline constexpr size_t kShockVel      = 0x5CD0;
inline constexpr size_t kShockStroke   = 0x5180;
inline constexpr size_t kBumpLen       = 0x5C4C;
inline constexpr size_t kBumpPen       = 0x5C60;
inline constexpr size_t kBodyWorld     = 0x08;
inline constexpr size_t kBodyNext      = 0x10;
inline constexpr size_t kBodyR         = 0xD0;   // dMatrix3, 3 rows of 4 floats
inline constexpr size_t kWorldFirst    = 0x20;
inline constexpr size_t kWorldCount    = 0x30;
inline constexpr uint32_t kRvaSetPosition = 0x2AC7A0;
inline constexpr int kMaxBodies = 64;   // a world with more is not a bike's

/// The last byte a read touches past `bike` (wheel 1's deflection rate). Tests size the fake.
inline constexpr size_t kBikeSpan = tyrelog::kWheelStride + kWheelDeflRate + 8;

inline constexpr tyrelog::Check kChecks[] = {
    {0x1A2FEA, "\x48\x8D\xBC\x08\xF8\x60\x00\x00", 8, "tyre block = wheel + 0x60F8"},
    {0x1A725E, "\xF2\x0F\x11\x84\x2E\x78\x28\x01\x00", 9, "deflection [w+0x12878]"},
    {0x1A7267, "\xF2\x0F\x11\x8C\x2E\x80\x28\x01\x00", 9, "deflection rate [w+0x12880]"},
    {0x1A7677, "\x89\x84\x2E\x24\x28\x01\x00", 7, "contact [w+0x12824]"},
    {0x1A3F67, "\x4C\x8B\xAF\xF0\xBF\x00\x00", 7, "tyre geometry [w+0x120E8]"},
    {0x1A3F76, "\xF2\x41\x0F\x10\x85\xA8\x00\x00\x00", 9, "TorusRadius +0xA8"},
    {0x1A2F23, "\xF2\x44\x0F\x11\x9B\xC8\x5C\x00\x00", 9, "shock extension [bike+0x5CC8]"},
    {0x1A2F2C, "\xF2\x44\x0F\x11\x93\xD0\x5C\x00\x00", 9, "shock velocity [bike+0x5CD0]"},
    {0x1A2B20, "\xF2\x44\x0F\x10\x8B\x80\x51\x00\x00", 9, "shock stroke [bike+0x5180]"},
    {0x1A2D54, "\xF3\x0F\x10\x9B\x4C\x5C\x00\x00", 8, "bump stop length [bike+0x5C4C]"},
    {0x1A2DF3, "\xF3\x0F\x11\xA3\x60\x5C\x00\x00", 8, "bump stop penetration [bike+0x5C60]"},
    {0x2AC745, "\x48\x8B\x46\x08", 4, "ODE body world +0x08"},
    {0x2ADCFB, "\x48\x8B\x46\x20\x48\x8D\x4E\x20\x48\x8D\x57\x10", 12, "ODE world first body +0x20, next +0x10"},
    {0x2ADD1D, "\xFF\x46\x30", 3, "ODE world body count +0x30"},
    {0x2AC7A0, "\x40\x53\x48\x83\xEC\x20\x48\x8B\x59\x40\xF3\x0F\x11\x89\xC0\x00\x00\x00", 18,
     "dBodySetPosition"},
    {0x2AC870, "\xF3\x0F\x11\x89\x10\x01\x00\x00", 8, "dBodySetLinearVel"},
    {0x2AC890, "\xF3\x0F\x11\x89\x20\x01\x00\x00", 8, "dBodySetAngularVel"},
};

// ---- frostmod_radar.cfg ----------------------------------------------------------------------
inline constexpr int kRingDefault = (int)tyrelog::kSteps;
inline constexpr int kRingMin = 500;
inline constexpr int kRingMax = 15000;   // 30 s at 2 ms

struct Config {
    bool watch = false;        // nanwatch=1
    int ring = kRingDefault;   // nanring=N, steps
    bool flat = false;         // nanflat=1
    int flatPct = 90;          // nanflatpct=N
    bool stress = false;       // nanstress=1
    int stressMs = 4000;       // nanstressms=N, bike time between throws
    int stressV = 9;           // nanstressv=N, m/s up
    int stressSpin = 3;        // nanstressspin=N, rad/s
    int stressDrop = 0;        // nanstressdrop=N, metres lifted first
    int stressWheel = 0;       // nanstresswheel=N, rad/s of wheel spin before landing

    /// Whether anything here needs the sim-step hook and the ring.
    bool Any() const { return watch || flat || stress; }
};

inline int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/// One cfg line. Returns true if it was one of ours. Each key wants a plain integer after '='.
inline bool ParseCfgLine(const char* line, Config& c) {
    if (!line) return false;
    while (*line == ' ' || *line == '\t') ++line;
    struct Key { const char* name; int* i; bool* b; int lo, hi; };
    int watch = -1, flat = -1, stress = -1;
    const Key keys[] = {
        // Longest first: "nanstressms" must not be read as "nanstress" + "ms=...".
        {"nanstresswheel", &c.stressWheel, nullptr, 0, 3000},
        {"nanstressspin", &c.stressSpin, nullptr, 0, 30},
        {"nanstressdrop", &c.stressDrop, nullptr, 0, 30},
        {"nanstressms", &c.stressMs, nullptr, 1000, 60000},
        {"nanstressv", &c.stressV, nullptr, 0, 40},
        {"nanstress", &stress, nullptr, 0, 1},
        {"nanflatpct", &c.flatPct, nullptr, 50, 100},
        {"nanflat", &flat, nullptr, 0, 1},
        {"nanwatch", &watch, nullptr, 0, 1},
        {"nanring", &c.ring, nullptr, kRingMin, kRingMax},
    };
    for (const Key& k : keys) {
        const size_t n = std::strlen(k.name);
        if (std::strncmp(line, k.name, n) != 0) continue;
        const char* p = line + n;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '=') continue;   // a longer key with this one as its prefix
        ++p;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p < '0' || *p > '9') return false;
        long v = 0;
        while (*p >= '0' && *p <= '9' && v < 1000000) v = v * 10 + (*p++ - '0');
        if (k.i == &watch || k.i == &flat || k.i == &stress) {
            const bool on = v == 1;   // exactly 1, like nantrap=1
            if (k.i == &watch) c.watch = on;
            else if (k.i == &flat) c.flat = on;
            else c.stress = on;
        } else {
            *k.i = Clamp((int)v, k.lo, k.hi);
        }
        return true;
    }
    return false;
}

/// The lines a save writes back: only what differs from the default.
inline int CfgLines(const Config& c, char (*out)[40], int max) {
    const Config d;
    int k = 0;
    auto put = [&](const char* key, int v) {
        if (k < max) std::snprintf(out[k++], 40, "%s=%d", key, v);
    };
    if (c.watch) put("nanwatch", 1);
    if (c.ring != d.ring) put("nanring", c.ring);
    if (c.flat) put("nanflat", 1);
    if (c.flatPct != d.flatPct) put("nanflatpct", c.flatPct);
    if (c.stress) put("nanstress", 1);
    if (c.stressMs != d.stressMs) put("nanstressms", c.stressMs);
    if (c.stressV != d.stressV) put("nanstressv", c.stressV);
    if (c.stressSpin != d.stressSpin) put("nanstressspin", c.stressSpin);
    if (c.stressDrop != d.stressDrop) put("nanstressdrop", c.stressDrop);
    if (c.stressWheel != d.stressWheel) put("nanstresswheel", c.stressWheel);
    return k;
}

// ---- the extra fields of one step ---------------------------------------------------------
inline double ReadD(const uint8_t* p) { double v; std::memcpy(&v, p, 8); return v; }
inline float ReadF(const uint8_t* p) { float v; std::memcpy(&v, p, 4); return v; }
inline int32_t ReadI(const uint8_t* p) { int32_t v; std::memcpy(&v, p, 4); return v; }
inline const uint8_t* ReadP(const uint8_t* p) { const uint8_t* v; std::memcpy(&v, p, sizeof(v)); return v; }

/// The tyre's contact half-angle in degrees, the way 0x1A3F67..0x1A3FC2 computes it:
/// acos((R - min(torus, d)) / R), d = -deflection. 0 with no deflection; NaN when it is.
inline double ContactAngle(double R, double defl, double torus) {
    const double d = -defl;
    if (std::isnan(d) || std::isnan(R) || std::isnan(torus)) return std::nan("");
    if (d <= 0) return 0.0;
    const double c = d < torus ? d : torus;
    if (!(R > 0)) return std::nan("");
    double x = (R - c) / R;
    if (x > 1) x = 1;
    if (x < -1) return std::nan("");   // what the game's acos would make of it
    return std::acos(x) * 57.29577951308232;
}

/// Deflection over the tyre's section height: 1.0 is fully flat (the game clips it there).
inline double FlatRatio(double defl, double torus) {
    if (!(torus > 0)) return std::nan("");
    return -defl / torus;
}

/// Body summary: the largest magnitudes over every body in the bike's ODE world.
struct Bodies {
    int16_t n = 0;            // bodies walked
    int16_t nonfinite = 0;    // bodies with any non-finite pos/vel/avel
    int16_t firstBad = -1;    // the first of those, in list order
    int16_t iPos = -1, iVel = -1, iAvel = -1;
    float maxPos = 0, maxVel = 0, maxAvel = 0;   // NaN-free: a bad body only counts above
};

inline bool Finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
inline float Mag3(const float* v) {
    return (float)std::sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]);
}

/// The world's bodies, in list order (newest first, as ODE links them). Returns how many were
/// written into `out` (at most `max`), stopping at a null link or kMaxBodies.
inline int WorldBodies(const uint8_t* body, const uint8_t** out, int max) {
    if (!body) return 0;
    const uint8_t* world = ReadP(body + kBodyWorld);
    if (!world) return 0;
    const int32_t count = ReadI(world + kWorldCount);
    if (count <= 0 || count > kMaxBodies) return 0;
    int k = 0;
    for (const uint8_t* b = ReadP(world + kWorldFirst); b && k < max && k < count; b = ReadP(b + kBodyNext))
        out[k++] = b;
    return k;
}

inline void Summarise(const uint8_t* const* bodies, int n, Bodies& o) {
    o = Bodies{};
    o.n = (int16_t)n;
    for (int i = 0; i < n; ++i) {
        float p[3], v[3], w[3];
        std::memcpy(p, bodies[i] + tyrelog::kBodyPos, 12);
        std::memcpy(v, bodies[i] + tyrelog::kBodyLvel, 12);
        std::memcpy(w, bodies[i] + tyrelog::kBodyAvel, 12);
        if (!Finite3(p) || !Finite3(v) || !Finite3(w)) {
            if (o.firstBad < 0) o.firstBad = (int16_t)i;
            ++o.nonfinite;
            continue;
        }
        const float mp = Mag3(p), mv = Mag3(v), mw = Mag3(w);
        if (mp > o.maxPos) { o.maxPos = mp; o.iPos = (int16_t)i; }
        if (mv > o.maxVel) { o.maxVel = mv; o.iVel = (int16_t)i; }
        if (mw > o.maxAvel) { o.maxAvel = mw; o.iAvel = (int16_t)i; }
    }
}

struct WheelX {
    double defl = 0;       // [w+0x12878], negative = squashed
    double deflRate = 0;   // [w+0x12880]
    double radius0 = 0;    // tyre Radius
    double torus = 0;      // tyre TorusRadius
    int32_t contact = 0;   // [w+0x12824]
};

struct Extra {
    int16_t have = 0;      // 1 once the fields below were read
    WheelX wheel[tyrelog::kWheels];
    double shockExt = 0, shockVel = 0, stroke = 0;
    float bumpLen = 0, bumpPen = 0;   // m, mm
    Bodies bodies;
};

/// The extra fields of one bike. Plain loads; nothing computed on what is read except the
/// body summary (which only compares and adds, so a NaN raises nothing that is armed: it runs
/// after the step, under the game's own MXCSR).
inline void ReadExtra(const uint8_t* bike, const uint8_t* const* bodies, int nBodies, Extra& x) {
    for (int i = 0; i < tyrelog::kWheels; ++i) {
        const uint8_t* w = bike + tyrelog::kWheelStride * (size_t)i;
        WheelX& o = x.wheel[i];
        o.defl = ReadD(w + kWheelDefl);
        o.deflRate = ReadD(w + kWheelDeflRate);
        o.contact = ReadI(w + kWheelContact);
        const uint8_t* g = ReadP(w + kWheelTyreGeom);
        o.radius0 = g ? ReadD(g + kGeomRadius) : 0.0;
        o.torus = g ? ReadD(g + kGeomTorus) : 0.0;
    }
    x.shockExt = ReadD(bike + kShockExt);
    x.shockVel = ReadD(bike + kShockVel);
    x.stroke = ReadD(bike + kShockStroke);
    x.bumpLen = ReadF(bike + kBumpLen);
    x.bumpPen = ReadF(bike + kBumpPen);
    Summarise(bodies, nBodies, x.bodies);
    x.have = 1;
}

// ---- the check ------------------------------------------------------------------------------
/// Sanity limits. Past one is not a NaN yet; it is the runaway the NaN grows out of (positions
/// reach 1e24 before the first NaN in the crashes we have).
struct Limits {
    double pos = 1e5;          // m, any body
    double vel = 250;          // m/s, any body
    double avelChassis = 200;  // rad/s, the chassis
    double avelBody = 2000;    // rad/s, any body (wheels spin ~300 at top speed)
    double spin = 1500;        // rad/s, the tyre model's wheel spin
    double radiusGrow = 0.05;  // m, sample radius over the tyre's Radius
    double gap = -0.3;         // m, a sample this far under the ground
    double flatOver = 1.05;    // deflection / TorusRadius (1.0 = the game's clip)
    double shockVel = 50;      // m/s
    double shockOver = 0.05;   // m past full extension
};

struct Trip {
    char field[24] = "";
    double value = 0;
    double limit = 0;          // NaN for a non-finite trip
    bool nonfinite = false;
};

/// Every field of `s` that is not finite or past a limit, non-finite first, in a fixed field
/// order. Returns how many were written (at most `max`).
inline int Check(const tyrelog::Step& s, const Extra& x, const Limits& L, Trip* out, int max) {
    int k = 0;
    Trip nf[48], lim[48];
    int nn = 0, nl = 0;
    auto test = [&](const char* name, double v, double limit, bool over) {
        if (!std::isfinite(v)) {
            if (nn < 48) { Trip& t = nf[nn++]; std::snprintf(t.field, sizeof(t.field), "%s", name);
                           t.value = v; t.limit = std::nan(""); t.nonfinite = true; }
        } else if (over ? v > limit : v < limit) {
            if (nl < 48) { Trip& t = lim[nl++]; std::snprintf(t.field, sizeof(t.field), "%s", name);
                           t.value = v; t.limit = limit; }
        }
    };
    static const char* axis[3] = {"x", "y", "z"};
    char name[24];
    if (s.body) {
        for (int c = 0; c < 3; ++c) { std::snprintf(name, sizeof(name), "pos_%s", axis[c]); test(name, std::fabs((double)s.pos[c]), L.pos, true); }
        for (int c = 0; c < 3; ++c) { std::snprintf(name, sizeof(name), "lvel_%s", axis[c]); test(name, std::fabs((double)s.lvel[c]), L.vel, true); }
        for (int c = 0; c < 3; ++c) { std::snprintf(name, sizeof(name), "avel_%s", axis[c]); test(name, std::fabs((double)s.avel[c]), L.avelChassis, true); }
    }
    for (int w = 0; w < tyrelog::kWheels; ++w) {
        const tyrelog::Wheel& o = s.wheel[w];
        std::snprintf(name, sizeof(name), "w%d_spin", w); test(name, std::fabs(o.spin), L.spin, true);
        const double r0 = x.have ? x.wheel[w].radius0 : 0.0;
        std::snprintf(name, sizeof(name), "w%d_radius", w);
        test(name, o.radius, r0 > 0 ? r0 + L.radiusGrow : 1e9, true);
        for (int c = 0; c < 3; ++c) { std::snprintf(name, sizeof(name), "w%d_s0_%s", w, axis[c]); test(name, std::fabs((double)o.sample[c]), L.pos, true); }
        std::snprintf(name, sizeof(name), "w%d_gap", w); test(name, o.gap, L.gap, false);
        if (x.have) {
            const WheelX& e = x.wheel[w];
            std::snprintf(name, sizeof(name), "w%d_defl", w); test(name, e.defl, -1e9, false);
            if (e.torus > 0) {
                std::snprintf(name, sizeof(name), "w%d_flat", w); test(name, FlatRatio(e.defl, e.torus), L.flatOver, true);
            }
            std::snprintf(name, sizeof(name), "w%d_defl_rate", w); test(name, e.deflRate, -1e9, false);
        }
    }
    if (x.have) {
        // Extension 0 is the bump stop fully squashed; below it the shock has a negative
        // length (PiBoSo: by then the sim has already exploded).
        test("shock_ext", x.shockExt, 0.0, false);
        test("shock_ext_max", x.shockExt, x.stroke + L.shockOver, true);
        test("shock_vel", std::fabs(x.shockVel), L.shockVel, true);
        test("bump_pen_mm", (double)x.bumpPen, -1e9, false);
        const Bodies& b = x.bodies;
        if (b.nonfinite > 0 && nn < 48) {
            Trip& t = nf[nn++];
            std::snprintf(t.field, sizeof(t.field), "body%d", (int)b.firstBad);
            t.value = std::nan(""); t.limit = std::nan(""); t.nonfinite = true;
        }
        if (b.n > 0) {
            std::snprintf(name, sizeof(name), "body%d_pos", (int)b.iPos); test(name, b.maxPos, L.pos, true);
            std::snprintf(name, sizeof(name), "body%d_vel", (int)b.iVel); test(name, b.maxVel, L.vel, true);
            std::snprintf(name, sizeof(name), "body%d_avel", (int)b.iAvel); test(name, b.maxAvel, L.avelBody, true);
        }
    }
    for (int i = 0; i < nn && k < max; ++i) out[k++] = nf[i];
    for (int i = 0; i < nl && k < max; ++i) out[k++] = lim[i];
    return k;
}

/// The field name without its body/wheel index, so "body3_vel" and "body5_vel" count as one
/// kind of trip for the once-a-session log.
inline void TripKind(const Trip& t, char* out, size_t n) {
    const char* f = t.field;
    if (std::strncmp(f, "body", 4) == 0) {
        const char* u = std::strchr(f, '_');
        std::snprintf(out, n, "%s%s%s", t.nonfinite ? "nonfinite:" : "limit:", "body", u ? u : "");
    } else {
        std::snprintf(out, n, "%s%s", t.nonfinite ? "nonfinite:" : "limit:", f);
    }
}

/// One line for the log: "w1_spin=nan (non-finite)" / "shock_ext=-0.0213 (limit -0.015)".
inline void TripText(const Trip& t, char* out, size_t n) {
    char v[32];
    if (std::isnan(t.value)) std::snprintf(v, sizeof(v), "nan");
    else if (std::isinf(t.value)) std::snprintf(v, sizeof(v), t.value > 0 ? "inf" : "-inf");
    else std::snprintf(v, sizeof(v), "%.6g", t.value);
    if (t.nonfinite) std::snprintf(out, n, "%s=%s (non-finite)", t.field, v);
    else std::snprintf(out, n, "%s=%s (limit %.6g)", t.field, v, t.limit);
}

// ---- the extra CSV columns ---------------------------------------------------------------------
inline const char* kExtraHeader =
    "w0_defl,w0_defl_rate,w0_flat,w0_contact_deg,w0_contact,w0_torus,"
    "w1_defl,w1_defl_rate,w1_flat,w1_contact_deg,w1_contact,w1_torus,"
    "shock_ext,shock_vel,stroke,bump_len,bump_pen_mm,"
    "bodies,bodies_bad,body_max_pos,body_max_vel,body_max_avel";

inline void ExtraCsv(const tyrelog::Step& s, const Extra& x, char* out, size_t n) {
    char f[24][32];
    int k = 0;
    for (int w = 0; w < tyrelog::kWheels; ++w) {
        const WheelX& e = x.wheel[w];
        tyrelog::Num(e.defl, f[k++], 32);
        tyrelog::Num(e.deflRate, f[k++], 32);
        tyrelog::Num(FlatRatio(e.defl, e.torus), f[k++], 32);
        tyrelog::Num(ContactAngle(s.wheel[w].radius, e.defl, e.torus), f[k++], 32);
        std::snprintf(f[k++], 32, "%d", (int)e.contact);
        tyrelog::Num(e.torus, f[k++], 32);
    }
    tyrelog::Num(x.shockExt, f[k++], 32);
    tyrelog::Num(x.shockVel, f[k++], 32);
    tyrelog::Num(x.stroke, f[k++], 32);
    tyrelog::Num(x.bumpLen, f[k++], 32);
    tyrelog::Num(x.bumpPen, f[k++], 32);
    std::snprintf(f[k++], 32, "%d", (int)x.bodies.n);
    std::snprintf(f[k++], 32, "%d", (int)x.bodies.nonfinite);
    tyrelog::Num(x.bodies.maxPos, f[k++], 32);
    tyrelog::Num(x.bodies.maxVel, f[k++], 32);
    tyrelog::Num(x.bodies.maxAvel, f[k++], 32);
    size_t w = 0;
    out[0] = 0;
    for (int i = 0; i < k && w < n; ++i)
        w += (size_t)std::snprintf(out + w, n - w, i ? ",%s" : "%s", f[i]);
}

// ---- tyre-flat episodes (nanflat=1) -----------------------------------------------------------
struct Episode {
    int wheel = 0;
    uint64_t startStep = 0, endStep = 0;
    float startT = 0, endT = 0;           // bike time
    double peakRatio = 0;                 // deflection / TorusRadius
    double peakAngle = 0;                 // contact half-angle, degrees
    double peakDefl = 0;                  // m (positive)
    double spinAtPeak = 0, radiusAtPeak = 0, gapAtPeak = 0, deflRateAtPeak = 0;
    double shockExtAtPeak = 0;
    float bumpPenAtPeak = 0;
    float speedAtStart = 0, vyAtStart = 0;
    bool nonfinite = false;               // a non-finite value was seen during it
};

/// One wheel's episodes. Enter at `on`, leave below `on - 0.05` (so a ratio dithering on the
/// line does not make a thousand one-step episodes).
struct FlatTracker {
    bool in = false;
    Episode cur;
    unsigned episodes = 0;

    /// Feed one step. Returns true when an episode just ended; it is then in `done`.
    bool Update(int wheel, uint64_t step, const tyrelog::Step& s, const Extra& x, double on, Episode& done) {
        const WheelX& e = x.wheel[wheel];
        const tyrelog::Wheel& t = s.wheel[wheel];
        const double ratio = FlatRatio(e.defl, e.torus);
        const bool bad = !std::isfinite(ratio) || !std::isfinite(t.spin) || !std::isfinite(t.radius);
        if (!in) {
            if (!(ratio >= on) && !bad) return false;
            in = true;
            cur = Episode{};
            cur.wheel = wheel;
            cur.startStep = step;
            cur.startT = s.bikeTime;
            cur.speedAtStart = (float)tyrelog::Len3(s.lvel);
            cur.vyAtStart = s.lvel[1];
        }
        cur.endStep = step;
        cur.endT = s.bikeTime;
        if (bad) cur.nonfinite = true;
        if (std::isfinite(ratio) && ratio >= cur.peakRatio) {
            cur.peakRatio = ratio;
            cur.peakDefl = -e.defl;
            cur.peakAngle = ContactAngle(t.radius, e.defl, e.torus);
            cur.spinAtPeak = t.spin;
            cur.radiusAtPeak = t.radius;
            cur.gapAtPeak = t.gap;
            cur.deflRateAtPeak = e.deflRate;
            cur.shockExtAtPeak = x.shockExt;
            cur.bumpPenAtPeak = x.bumpPen;
        }
        if (!bad && ratio < on - 0.05) {
            in = false;
            ++episodes;
            done = cur;
            return true;
        }
        return false;
    }
};

inline const char* kFlatHeader =
    "wheel,start_step,steps,start_bike_t,ms,peak_flat,peak_defl_m,peak_contact_deg,spin,radius,gap,"
    "defl_rate,shock_ext,bump_pen_mm,speed_at_start,vy_at_start,nonfinite";

inline void FlatCsv(const Episode& e, char* out, size_t n) {
    char f[12][32];
    int k = 0;
    tyrelog::Num(e.peakRatio, f[k++], 32);
    tyrelog::Num(e.peakDefl, f[k++], 32);
    tyrelog::Num(e.peakAngle, f[k++], 32);
    tyrelog::Num(e.spinAtPeak, f[k++], 32);
    tyrelog::Num(e.radiusAtPeak, f[k++], 32);
    tyrelog::Num(e.gapAtPeak, f[k++], 32);
    tyrelog::Num(e.deflRateAtPeak, f[k++], 32);
    tyrelog::Num(e.shockExtAtPeak, f[k++], 32);
    tyrelog::Num(e.bumpPenAtPeak, f[k++], 32);
    tyrelog::Num(e.speedAtStart, f[k++], 32);
    tyrelog::Num(e.vyAtStart, f[k++], 32);
    const unsigned long long steps = e.endStep >= e.startStep ? e.endStep - e.startStep + 1 : 0;
    std::snprintf(out, n, "%d,%llu,%llu,%.4f,%.1f,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%d", e.wheel,
                  (unsigned long long)e.startStep, steps, (double)e.startT, (double)(e.endT - e.startT) * 1000.0,
                  f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], e.nonfinite ? 1 : 0);
}

// ---- stress (nanstress=1) ---------------------------------------------------------------------
/// The throws cycle through these. The rotation axis is in the bike's own frame: x across the
/// bike (pitch), z along it (roll).
enum class Throw { Flat, NoseDown, NoseUp, RollLeft, RollRight };
inline constexpr int kThrows = 5;
inline Throw ThrowAt(unsigned n) { return (Throw)(n % kThrows); }
inline const char* ThrowName(Throw t) {
    switch (t) {
        case Throw::Flat: return "flat";
        case Throw::NoseDown: return "nose down";
        case Throw::NoseUp: return "nose up";
        case Throw::RollLeft: return "roll left";
        default: return "roll right";
    }
}

/// The world-frame angular velocity of a throw, from the chassis rotation (ODE dMatrix3: three
/// rows of four floats; column j is the body's local axis j in the world).
inline void ThrowOmega(Throw t, const float* R, float spin, float* w) {
    int axis = -1;
    float sign = 1;
    switch (t) {
        case Throw::NoseDown: axis = 0; sign = 1; break;
        case Throw::NoseUp: axis = 0; sign = -1; break;
        case Throw::RollLeft: axis = 2; sign = 1; break;
        case Throw::RollRight: axis = 2; sign = -1; break;
        default: break;
    }
    for (int r = 0; r < 3; ++r) w[r] = axis < 0 ? 0.0f : sign * spin * R[r * 4 + axis];
}

/// A rigid-body velocity change for one body: v += dv + w x (p - c), avel += w. Every body of
/// the bike gets the same field, so the joints between them see no change at all.
inline void ApplyKick(const float* p, float* v, float* avel, const float* c, const float* dv, const float* w) {
    const float r[3] = {p[0] - c[0], p[1] - c[1], p[2] - c[2]};
    const float x[3] = {w[1] * r[2] - w[2] * r[1], w[2] * r[0] - w[0] * r[2], w[0] * r[1] - w[1] * r[0]};
    for (int i = 0; i < 3; ++i) {
        v[i] += dv[i] + x[i];
        avel[i] += w[i];
    }
}

/// The wheel's own body: the one whose position is R from that wheel's first tyre sample (the
/// samples sit on a circle of radius R about the wheel centre). -1 if none is within `tol`.
inline int FindWheelBody(const float* sample0, double R, const float (*pos)[3], int n, float tol) {
    int best = -1;
    double bestErr = tol;
    for (int i = 0; i < n; ++i) {
        const double dx = pos[i][0] - sample0[0], dy = pos[i][1] - sample0[1], dz = pos[i][2] - sample0[2];
        const double err = std::fabs(std::sqrt(dx * dx + dy * dy + dz * dz) - R);
        if (err < bestErr) { bestErr = err; best = i; }
    }
    return best;
}

}  // namespace nanwatch
