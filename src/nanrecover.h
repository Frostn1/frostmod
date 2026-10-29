// nanrecover: when a bike's physics state stops being numbers, put the bike back on the track
// with the game's own reset, before anything reads it.
//
// THE CRASH. The physics NaN is the biggest crash family in the reports: 3,749 of 8,232 MX Bikes
// crashes (46%) from 2026-10-03 to 10-28 come after "refused a NaN height query", from 1,792
// riders. The terrain guard (offsets.h RVA_TERRAIN_SAMPLE) and the shock guard (shockguard.h)
// each turn away one reader of the NaN, but the bike stays NaN and the next reader faults:
// +0x1A29AE (3,045), +0x268480 (402), +0x1EEA9C (136), +0x2469BB (41) and a tail.
//
// WHERE IT STARTS. The 2 s rings nantrap=1 riders sent with their crashes (tyrelog.h) say the
// same thing every time: the bike is calm one 2 ms step and NaN the next. Ring 14953 (washington,
// online, 10-28): 10.15 m/s, wheel spin 27/32 rad/s, both tyres on the ground, then pos, lvel,
// avel and both spins NaN in step 759731, and +0x1A29AE in step 759732. Ring 14917: 0.55 m/s,
// same pattern. Ring 14887: four steps after a spawn. The offline repro in
// research/nan-root-cause.md runs away for 34 steps first, and its first non-finite value is an
// overflow inside ODE's dxStepIsland_Stage0_Bodies (+0x2C254B, under the world step 0x2AD5F0
// called from the sim step at 0x1BE702). Either way the NaN is made inside one call of the sim
// step 0x1BE3A0 and read by the bike code (0x1ADA40, called from 0x1BE685) in the next one,
// which is where every one of those crashes is. Between the two there is exactly one place
// FrostMod can stand: right after the sim step returns.
//
// WHAT THIS DOES. After every sim step (and once before it, for a NaN made between steps by
// something else), each active bike in that world slot is checked: every float of its seven ODE
// bodies' position, rotation, quaternion and velocities, both wheels' spin, sample radius, first
// sample point and tyre deflection, and the shock and fork lengths. Integer tests on the bit
// patterns, no float arithmetic, so the check itself can never raise or round. A finite state,
// however large, is never touched.
//
// A non-finite one is reset with the game's own place-bike, 0x197060: the function the game's
// respawn runs (0x21E30 -> 0x1187E0 -> bus job 0x25A -> 0x197060, see tyrelog.h). It is
// everything a reset needs and nothing FrostMod has to guess at: it re-reads the suspension from
// the setup (0x196500 zeroes the shock velocity and writes a fresh shock length and stroke,
// both wheels' spin, tyre deflection and its rate, the fork), poses the bike on the ground under
// the point it is given (two ground queries, 0x1BB360), places all seven bodies with zero
// velocity (0x1B7540 via 0x195D10 and 0x1957D0), enables them, and bumps the bike's reset
// counter [bike+0x3C]. Its arguments are (handle, x, z, heading in degrees, y), the order the
// bus job unpacks them in (0x124A9A).
//
// The point it is given is where the bike was: a snapshot of the main body's position and
// heading, taken every 25 steps (50 ms) while the bike is finite and sane, and at least 100 ms
// old when used, so a bike that ran away over a few dozen steps (the repro's pattern) is not
// put back where it was already going wrong. A bike that needs it again within 2 s goes further
// back each time. With no snapshot at all the bike is left to the backstops.
//
// The terrain and shock guards stay: they catch whatever reads the NaN in the same step that
// made it, before this gets to run.
//
// No Win32 here, so tests/nanrecover_test.cpp runs anywhere; the live half is the NANRECOVER
// section of frostmod.cpp.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nanrecover {

// ---- the game, mxbikes.exe beta21e (image base 0x140000000) -------------------------------
/// place-bike: int (int handle, float x, float z, float headingDeg, float y). 0 placed, 1 no
/// such bike. The bus job 0x25A's handler (kRvaJob) is a straight call into it.
constexpr uintptr_t kRvaReset = 0x197060;
constexpr uintptr_t kRvaJob   = 0x124A9A;
using ResetFn = int32_t (*)(int32_t handle, float x, float z, float headingDeg, float y);

// The bike (the rcx of the bike sim 0x1ADA40; an item of [world+0x28], see tyrelog.h).
constexpr size_t kBikeHandle = 0x00;    // int, the handle place-bike takes
constexpr size_t kBikeActive = 0x38;    // int, the bike sim returns at once when 0
constexpr size_t kBikeResets = 0x3C;    // byte, +1 on every place-bike (0x1981C7)
constexpr size_t kBikeTime   = 0x40;    // float, seconds since spawn (place-bike leaves it)
constexpr size_t kGateFront  = 0xBC;    // int: 3 = no body at kBodyFront (0x195D50)
constexpr int    kGateFrontNone = 3;
constexpr size_t kGateRear   = 0xF8;    // int: 5 = no body at kBodyRear (0x195D73)
constexpr int    kGateRearNone = 5;
constexpr size_t kForkLen    = 0x4C78;  // double, fork length (0x197153)
constexpr size_t kShockLen   = 0x5CC8;  // double, rear shock length (0x197181, shockguard.h)
constexpr size_t kShockVel   = 0x5CD0;  // double, rear shock velocity (zeroed at 0x196B92)

// The seven ODE bodies place-bike positions, as pointers in the bike.
constexpr size_t kBodyMain   = 0x430;   // placed at frame [bike+0x3A0] (0x197696)
constexpr size_t kBodyFrame  = 0x3780;  // (0x195D3B)
constexpr size_t kBodyFront  = 0x4CF0;  // unless [bike+0xBC] == 3 (0x195D59)
constexpr size_t kBodyRear   = 0x5DA0;  // unless [bike+0xF8] == 5 (0x195D7C)
constexpr size_t kBodyWheel  = 0x140B0; // wheel 0; wheel 1 is + kWheelStride (0x195D96, 0x195DAC)
constexpr size_t kBodyRider  = 0x23480; // (0x195A0B, in 0x1957D0)

// The wheels, as tyrelog.h reads them (wheel i = bike + i * 0xE310).
constexpr int    kWheels       = 2;
constexpr size_t kWheelStride  = 0xE310;
constexpr size_t kWheelSpin    = 0x12208;  // double, zeroed by 0x196500 (0x196E15)
constexpr size_t kWheelRadius  = 0x121D8;  // double
constexpr size_t kWheelSample0 = 0x12210;  // float x, y, z
constexpr size_t kWheelDefl    = 0x12878;  // double, zeroed by 0x196500 (0x196DE0)
constexpr size_t kWheelDeflVel = 0x12880;  // double, zeroed by 0x196500

// An ODE dxBody (dBodyGet*/Set*/Add* at 0x2AC7A0..0x2AC9D0). dReal is float in this build.
constexpr size_t kBodyPos  = 0xC0;    // float[3]
constexpr size_t kBodyR    = 0xD0;    // float[12], 3x4 row-major, [3], [7], [11] unused
constexpr size_t kBodyQ    = 0x100;   // float[4]
constexpr size_t kBodyLvel = 0x110;   // float[3]
constexpr size_t kBodyAvel = 0x120;   // float[3]
constexpr size_t kBodyFacc = 0x130;   // float[3], force accumulator
constexpr size_t kBodyTacc = 0x140;   // float[3], torque accumulator
constexpr size_t kBodySpan = kBodyTacc + 12;

constexpr size_t kBikeSpan = kBodyRider + 8;   // the furthest byte the check reads in a bike

// ---- what the install insists on ------------------------------------------------------------
struct Check {
    uint32_t rva;
    const char* bytes;
    int len;
    const char* what;
};
inline constexpr Check kChecks[] = {
    // place-bike: its prologue through the handle lookup, then the suspension reset before it
    // reads the fork and shock, the body placements, and the reset counter.
    {0x197060, "\x41\x54\x48\x81\xEC\x40\x0C\x00\x00", 9, "place-bike prologue"},
    {0x1970A2, "\xE8\xF9\x80\x02\x00\x4C\x8B\xE0", 8, "handle -> bike (0x1BF1A0)"},
    {0x197123, "\xE8\xD8\xF3\xFF\xFF", 5, "suspension reset (0x196500) first"},
    {0x197153, "\xF2\x41\x0F\x10\x84\x24\x78\x4C\x00\x00", 10, "fork [bike+0x4C78]"},
    {0x197181, "\xF2\x41\x0F\x10\x8C\x24\xC8\x5C\x00\x00", 10, "shock [bike+0x5CC8]"},
    {0x197696, "\x49\x8B\x8C\x24\x30\x04\x00\x00", 8, "main body [bike+0x430]"},
    {0x197771, "\xE8\x9A\xE5\xFF\xFF", 5, "place bodies (0x195D10)"},
    {0x197A45, "\xE8\x86\xDD\xFF\xFF", 5, "place rider (0x1957D0)"},
    {0x1981C7, "\x41\xFE\x44\x24\x3C", 5, "reset counter [bike+0x3C]"},
    // the bus job: (handle, x, z, heading, y) from its argument block, straight into place-bike
    {0x124A9A, "\x48\x8D\x84\x24\xD8\x02\x00\x00\xF2\x0F\x5A\x40\x20\xF2\x0F\x5A\x48\x08"
               "\xF2\x0F\x5A\x50\x10\x8B\x08\xF2\x0F\x5A\x58\x18\xF3\x0F\x11\x44\x24\x20"
               "\xE8\x9D\x25\x07\x00", 41, "bus job 0x25A -> place-bike"},
    // 0x196500 zeroes what we check and it does not place
    {0x196B92, "\x4C\x89\xAD\xD0\x5C\x00\x00", 7, "shock velocity zeroed"},
    {0x196BBD, "\xF2\x0F\x11\xAD\xC8\x5C\x00\x00", 8, "shock length written"},
    {0x196D4F, "\x48\x8D\x9D\xF8\x60\x00\x00", 7, "wheel loop base bike+0x60F8"},
    {0x196DE0, "\x4C\x89\xAB\x80\xC7\x00\x00", 7, "tyre deflection zeroed"},
    {0x196E15, "\x4C\x89\xAB\x10\xC1\x00\x00", 7, "wheel spin zeroed"},
    // the bodies
    {0x195D3B, "\x48\x8B\x89\x80\x37\x00\x00", 7, "frame body [bike+0x3780]"},
    {0x195D50, "\x83\xBF\xBC\x00\x00\x00\x03", 7, "front gate [bike+0xBC] == 3"},
    {0x195D59, "\x48\x8B\x8F\xF0\x4C\x00\x00", 7, "front body [bike+0x4CF0]"},
    {0x195D73, "\x83\xBF\xF8\x00\x00\x00\x05", 7, "rear gate [bike+0xF8] == 5"},
    {0x195D7C, "\x48\x8B\x8F\xA0\x5D\x00\x00", 7, "rear body [bike+0x5DA0]"},
    {0x195D96, "\x48\x8B\x8F\xB0\x40\x01\x00", 7, "wheel 0 body [bike+0x140B0]"},
    {0x195DAC, "\x48\x8B\x8F\xC0\x23\x02\x00", 7, "wheel 1 body [bike+0x223C0]"},
    {0x195A0B, "\x48\x8B\x8F\x80\x34\x02\x00", 7, "rider body [bike+0x23480]"},
    // ODE's dxBody
    {0x2AC7AA, "\xF3\x0F\x11\x89\xC0\x00\x00\x00", 8, "dBodySetPosition +0xC0"},
    {0x2AC8C0, "\x48\x8D\x81\xD0\x00\x00\x00", 7, "dBodyGetRotation +0xD0"},
    {0x2AC8D0, "\x48\x8D\x81\x00\x01\x00\x00", 7, "dBodyGetQuaternion +0x100"},
    {0x2AC870, "\xF3\x0F\x11\x89\x10\x01\x00\x00", 8, "dBodySetLinearVel +0x110"},
    {0x2AC890, "\xF3\x0F\x11\x89\x20\x01\x00\x00", 8, "dBodySetAngularVel +0x120"},
    {0x2AC990, "\xF3\x0F\x58\x89\x30\x01\x00\x00", 8, "dBodyAddForce +0x130"},
    {0x2AC9D0, "\xF3\x0F\x58\x89\x40\x01\x00\x00", 8, "dBodyAddTorque +0x140"},
};

// ---- the check -------------------------------------------------------------------------------
/// Exponent all ones: an infinity or a NaN. Bits, not floats: nothing here can raise.
inline bool Bad32(uint32_t b) { return (b & 0x7F800000u) == 0x7F800000u; }
inline bool Bad64(uint64_t b) { return (b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull; }

enum class Field : uint8_t {
    None = 0,
    BodyPos, BodyRot, BodyQuat, BodyLvel, BodyAvel,
    WheelSpin, WheelRadius, WheelSample, WheelDefl, WheelDeflVel,
    ForkLen, ShockLen, ShockVel,
};
inline const char* FieldName(Field f) {
    switch (f) {
        case Field::BodyPos: return "body position";
        case Field::BodyRot: return "body rotation";
        case Field::BodyQuat: return "body quaternion";
        case Field::BodyLvel: return "body velocity";
        case Field::BodyAvel: return "body spin";
        case Field::WheelSpin: return "wheel spin";
        case Field::WheelRadius: return "tyre sample radius";
        case Field::WheelSample: return "tyre sample point";
        case Field::WheelDefl: return "tyre deflection";
        case Field::WheelDeflVel: return "tyre deflection rate";
        case Field::ForkLen: return "fork length";
        case Field::ShockLen: return "shock length";
        case Field::ShockVel: return "shock velocity";
        default: return "nothing";
    }
}

/// What was found: the first non-finite field and where. `where` is the body's offset in the
/// bike for a body field, the wheel index for a wheel field, 0 otherwise.
struct Bad {
    Field field = Field::None;
    uint32_t where = 0;
    uint32_t bits = 0;   // the low 32 bits of the value, to tell NaN from inf in the log
};

inline uint32_t U32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t U64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline const uint8_t* Ptr(const uint8_t* p) { const uint8_t* v; std::memcpy(&v, p, sizeof(v)); return v; }

/// The bodies place-bike places, in the order it places them. Null where the bike has none
/// (the two gates, or a pointer the game has not filled). Returns how many slots it wrote (7).
inline int Bodies(const uint8_t* bike, const uint8_t* out[7], uint32_t offs[7]) {
    const size_t at[7] = {kBodyMain, kBodyFrame, kBodyFront, kBodyRear, kBodyWheel,
                          kBodyWheel + kWheelStride, kBodyRider};
    const bool noFront = (int32_t)U32(bike + kGateFront) == kGateFrontNone;
    const bool noRear  = (int32_t)U32(bike + kGateRear) == kGateRearNone;
    for (int i = 0; i < 7; ++i) {
        offs[i] = (uint32_t)at[i];
        out[i] = ((at[i] == kBodyFront && noFront) || (at[i] == kBodyRear && noRear))
                     ? nullptr : Ptr(bike + at[i]);
    }
    return 7;
}

inline bool ScanFloats(const uint8_t* p, int n, Field f, uint32_t where, Bad* bad) {
    uint32_t any = 0, first = 0;
    for (int i = 0; i < n; ++i) {
        const uint32_t b = U32(p + 4 * i);
        if (Bad32(b) && !any) first = b;
        any |= Bad32(b) ? 1u : 0u;
    }
    if (any && bad) { bad->field = f; bad->where = where; bad->bits = first; }
    return !any;
}
inline bool ScanDouble(const uint8_t* p, Field f, uint32_t where, Bad* bad) {
    const uint64_t b = U64(p);
    if (!Bad64(b)) return true;
    if (bad) { bad->field = f; bad->where = where; bad->bits = (uint32_t)(b >> 32); }
    return false;
}

/// True when every value of `bike` this checks is finite. Otherwise false, with the first
/// non-finite one in `*bad`. Order: bodies (what ODE makes), wheels, suspension.
inline bool Scan(const uint8_t* bike, Bad* bad) {
    if (!bike) return true;
    const uint8_t* body[7];
    uint32_t off[7];
    Bodies(bike, body, off);
    for (int i = 0; i < 7; ++i) {
        const uint8_t* b = body[i];
        if (!b) continue;
        if (!ScanFloats(b + kBodyPos, 3, Field::BodyPos, off[i], bad)) return false;
        // The rotation's three rows; the fourth float of each is ODE padding, never read.
        if (!ScanFloats(b + kBodyR, 3, Field::BodyRot, off[i], bad) ||
            !ScanFloats(b + kBodyR + 16, 3, Field::BodyRot, off[i], bad) ||
            !ScanFloats(b + kBodyR + 32, 3, Field::BodyRot, off[i], bad)) return false;
        if (!ScanFloats(b + kBodyQ, 4, Field::BodyQuat, off[i], bad)) return false;
        if (!ScanFloats(b + kBodyLvel, 3, Field::BodyLvel, off[i], bad)) return false;
        if (!ScanFloats(b + kBodyAvel, 3, Field::BodyAvel, off[i], bad)) return false;
    }
    for (int w = 0; w < kWheels; ++w) {
        const uint8_t* p = bike + kWheelStride * (size_t)w;
        if (!ScanDouble(p + kWheelSpin, Field::WheelSpin, (uint32_t)w, bad)) return false;
        if (!ScanDouble(p + kWheelRadius, Field::WheelRadius, (uint32_t)w, bad)) return false;
        if (!ScanFloats(p + kWheelSample0, 3, Field::WheelSample, (uint32_t)w, bad)) return false;
        if (!ScanDouble(p + kWheelDefl, Field::WheelDefl, (uint32_t)w, bad)) return false;
        if (!ScanDouble(p + kWheelDeflVel, Field::WheelDeflVel, (uint32_t)w, bad)) return false;
    }
    if (!ScanDouble(bike + kForkLen, Field::ForkLen, 0, bad)) return false;
    if (!ScanDouble(bike + kShockLen, Field::ShockLen, 0, bad)) return false;
    if (!ScanDouble(bike + kShockVel, Field::ShockVel, 0, bad)) return false;
    return true;
}

// ---- where to put it back --------------------------------------------------------------------
constexpr uint32_t kSnapEvery = 25;     // steps between snapshots: 50 ms
constexpr int      kSnaps     = 64;     // 3.2 s of them
constexpr uint32_t kMinAge    = 50;     // steps: a snapshot used for a reset is >= 100 ms old
constexpr uint32_t kRepeatWindow = 1000;   // steps: a second reset inside 2 s goes further back

// Sanity for a snapshot (not for the check: a finite state is never reset, however wild).
// The repro ran away through finite values for 34 steps; a snapshot taken then is not where
// to put the bike back.
constexpr float kMaxCoord = 1.0e5f;     // m: no track is 100 km from its origin
constexpr float kMaxSpeed = 200.0f;     // m/s
constexpr float kMaxSpin  = 200.0f;     // rad/s, the main body

struct Snap {
    float pos[3] = {0, 0, 0};
    float heading = 0;   // degrees, place-bike's convention
    uint32_t step = 0;   // the per-bike step counter it was taken at
    bool valid = false;
};

/// Heading in degrees from a body's 3x4 rotation, the way place-bike builds it: 0x266BA0 makes
/// rows (c, 0, s) (0, 1, 0) (-s, 0, c) for heading h, and the body gets those rows (0x1B7540),
/// so the bike's forward axis is column 2 and h = atan2(R[0][2], R[2][2]). That is also how the
/// respawn path turns a direction into a heading (atan2(dx, dz) * 57.29578 at 0x22359).
inline float HeadingDeg(const float R[12]) {
    const float s = R[2], c = R[10];
    if (!(s * s + c * c > 1e-6f)) return 0.0f;   // pointing straight up or down: any heading
    return (float)(std::atan2((double)s, (double)c) * 57.29577951308232);
}

/// Whether a finite main body is somewhere sane to put the bike back. Assumes Scan passed.
inline bool Sane(const float pos[3], const float lvel[3], const float avel[3]) {
    for (int i = 0; i < 3; ++i)
        if (!(std::fabs(pos[i]) < kMaxCoord)) return false;
    const float v2 = lvel[0] * lvel[0] + lvel[1] * lvel[1] + lvel[2] * lvel[2];
    const float w2 = avel[0] * avel[0] + avel[1] * avel[1] + avel[2] * avel[2];
    return v2 < kMaxSpeed * kMaxSpeed && w2 < kMaxSpin * kMaxSpin;
}

/// One bike's history. Keyed by the bike pointer and handle; a fresh spawn (time going
/// backwards) or another bike in the slot starts it over.
struct Track {
    const void* bike = nullptr;
    int32_t handle = 0;
    float lastTime = 0;
    uint32_t steps = 0;        // post-step checks of this bike that passed
    uint32_t lastSnapAt = 0;
    Snap snaps[kSnaps];
    int next = 0;              // where the next snapshot goes
    uint32_t lastResetAt = 0;  // steps value at the last reset, 0 = none
    int streak = 0;            // resets inside kRepeatWindow of each other
    uint32_t resets = 0;       // this bike, this spawn
    uint8_t counter = 0;       // [bike+0x3C] as last seen
    bool haveCounter = false;

    void Clear(const void* b, int32_t h, float t) {
        *this = Track{};
        bike = b; handle = h; lastTime = t;
    }
    /// The game's reset counter, each passing step. A change we did not make is the game (or the
    /// player) moving the bike: what we kept is of a place it no longer is, so it goes.
    void SawCounter(uint8_t c) {
        if (haveCounter && c != counter) {
            for (Snap& s : snaps) s.valid = false;
            lastSnapAt = 0;
        }
        counter = c;
        haveCounter = true;
    }
    /// After our own place-bike: the counter moved, and the snapshots stay (a second reset
    /// soon after goes further back through them).
    void PlacedByUs(uint8_t c) { counter = c; haveCounter = true; }
    /// Called after a passing check. Takes a snapshot every kSnapEvery steps when sane.
    void Passed(const float pos[3], const float lvel[3], const float avel[3], const float R[12]) {
        ++steps;
        if (lastSnapAt && steps - lastSnapAt < kSnapEvery) return;
        if (!Sane(pos, lvel, avel)) return;
        Snap& s = snaps[next];
        std::memcpy(s.pos, pos, sizeof(s.pos));
        s.heading = HeadingDeg(R);
        s.step = steps;
        s.valid = true;
        next = (next + 1) % kSnaps;
        lastSnapAt = steps;
    }
    /// The snapshot to reset to: the newest one at least `minAge` steps old, or failing that
    /// the oldest held. Null when there are none.
    const Snap* Pick(uint32_t minAge) const {
        const Snap* best = nullptr;
        const Snap* oldest = nullptr;
        for (int i = 0; i < kSnaps; ++i) {
            const Snap& s = snaps[i];
            if (!s.valid) continue;
            if (!oldest || s.step < oldest->step) oldest = &s;
            if (steps - s.step >= minAge && (!best || s.step > best->step)) best = &s;
        }
        return best ? best : oldest;
    }
    /// How far back a reset goes: 100 ms the first time; 1 s if it is needed again within 2 s;
    /// as far back as the ring holds after that.
    uint32_t MinAgeForNext() const {
        const int k = (lastResetAt && steps - lastResetAt < kRepeatWindow) ? streak + 1 : 0;
        return k == 0 ? kMinAge : k == 1 ? 500u : (uint32_t)kSnaps * kSnapEvery;
    }
    void DidReset() {
        streak = (lastResetAt && steps - lastResetAt < kRepeatWindow) ? streak + 1 : 0;
        lastResetAt = steps ? steps : 1;
        ++resets;
    }
};

/// Rate limit for the log: the first three in full, then on each power of two.
inline bool ShouldLog(uint64_t n) { return n > 0 && (n <= 3 || (n & (n - 1)) == 0); }

}  // namespace nanrecover
