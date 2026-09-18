// lean.h - where the rider is asking to be, for MXB Coach's recorder.
//
// The plugin API has no rider pose: every orientation value in the bike data is the chassis,
// so rider lean can't even be had by subtraction. But the rider moves their body with two
// controls of their own, and those are bound in the same controls.txt the Sit bind is read
// from. So the recorder watches them the way stance.h watches Sit:
//   controls.txt  CTRL_LRLEAN   the rider left and right: counter-lean
//                 CTRL_FBLEAN   the rider forward and back
//                 (CTRL_LEAN is the bike, not the rider, and is not read here.)
//   profile.ini   [aids] autoriderlrlean=1 / autoriderfblean=1
//                 the game moves the rider itself on that axis: it reads as unknown, the
//                 way autoridersit does for Sit.
//
// Both controls are two-sided, so a pad rider binds one axis and a keyboard rider binds one
// key per direction. Both are recorded as one number from -1 to +1.
//
// What is recorded is the rider's *input* - where they are asking to be - not a body angle.
// The body has its own rates and springs in the bike's own config, and the aids can override
// it. Nothing built on these records should be worded as though the rider's body is visible.
//
// The raw stick is recorded, not a curved one. The game puts the input through a deadzone,
// a linearity curve, a gain and asymmetric smoothing, all from the ten tuning values that
// close the control's own line; those are recorded beside it so the curve can be worked out
// later. Against the rider's own fast lap - the usual reference - the curve is the same on
// both laps and cancels, so the raw number already compares. Across riders it does not.
//
// No Win32 here, so tests/lean_test.cpp runs anywhere. mxbcoach.cpp reads the files, polls
// the binds and writes the records.
//
// Records in the .mxbc file (tags in coachrec.h; little-endian):
//   LEAN_BIND   196 bytes, once per stint, right after SESSION
//      0  u8   layout version (1)
//      1  u8[3] zero
//      4  left/right axis, 96 bytes as below
//      100 forward/back axis, 96 bytes as below
//    each axis:
//      0  u8   input: 0 none, 1 key, 2 controller button, 3 axis, 4 POV, 5 other
//      1  u8   aid: 0 off, 1 on (the game moves the rider), 2 unknown
//      2  u8   confidence: 0 none (every LEAN is unknown on this axis), 1 guess, 2 sure
//      3  u8   read through: 0 nothing, 1 keyboard, 2 DirectInput, 3 XInput
//      4  i32  scan code, button or axis number; -1 for none
//      8  i32  the other direction's code or button; -1 when the bind has one side
//      12 i32  the axis line's direction flag, 0 or 1; -1 when there isn't one
//      16 f32[10] the control's ten tuning values, in the game's own order
//      56 char[40] device GUID as the game wrote it, NUL-padded ("" for a key or other)
//   LEAN        16 bytes, with every sample whose stance is known on either axis
//      0  f32  track time s        (as in the SAMPLE it came with)
//      4  f32  lap position 0..1
//      8  f32  left/right, -1 fully left to +1 fully right; NaN unknown
//      12 f32  forward/back, -1 fully back to +1 fully forward; NaN unknown
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "stance.h"

namespace lean {

using stance::Bind;
using stance::Confidence;
using stance::Flag;
using stance::Input;
using stance::Source;

constexpr const char* kLeftRight   = "CTRL_LRLEAN";
constexpr const char* kForwardBack = "CTRL_FBLEAN";

constexpr uint8_t kBindLayout = 1;
constexpr size_t  kAxisSize   = 96;
constexpr size_t  kBindSize   = 4 + kAxisSize * 2;
constexpr size_t  kEventSize  = 16;

/// Unknown, as the records carry it: the one value that can't be confused with a reading.
inline float Unknown() { return std::nanf(""); }
inline bool  IsKnown(float v) { return !std::isnan(v); }

/// One of the two controls: how it is bound, and whether the game is moving the rider on it.
struct Axis {
    Bind bind;
    Flag aid = stance::FLAG_UNKNOWN;
};

struct Setup {
    Axis lr;  // CTRL_LRLEAN
    Axis fb;  // CTRL_FBLEAN
};

inline Setup ReadSetup(const std::string& profile_ini, const std::string& controls) {
    Setup s;
    const auto flag = [&profile_ini](const char* key) {
        const std::string v = stance::IniValue(profile_ini, "aids", key);
        if (v == "1") return stance::FLAG_ON;
        if (v == "0") return stance::FLAG_OFF;
        return stance::FLAG_UNKNOWN;
    };
    s.lr.aid  = flag("autoriderlrlean");
    s.fb.aid  = flag("autoriderfblean");
    s.lr.bind = stance::FindBind(controls, kLeftRight);
    s.fb.bind = stance::FindBind(controls, kForwardBack);
    return s;
}

/// How far one axis can be trusted, given what could be polled.
///
/// An aid on that axis is the end of it: the game is moving the rider, so the control says
/// nothing about where the rider is. An axis read over XInput is no reading at all - XInput
/// is the first pad rather than the one bound, and for a stick it would also be a guess at
/// which stick - so it rates none rather than a guess. A pair of buttons or keys survives
/// that, because which of two the rider pressed is not in doubt.
inline Confidence Rate(const Axis& a, Source src) {
    if (a.aid == stance::FLAG_ON || src == stance::SRC_NONE) return stance::CONF_NONE;
    switch (a.bind.input) {
        case stance::IN_PAD_AXIS:
            if (src != stance::SRC_DIRECTINPUT) return stance::CONF_NONE;
            break;
        case stance::IN_KEY:
            if (src != stance::SRC_KEYBOARD) return stance::CONF_NONE;
            break;
        case stance::IN_PAD_BUTTON:
            if (src != stance::SRC_DIRECTINPUT && src != stance::SRC_XINPUT) return stance::CONF_NONE;
            break;
        default:
            // Unbound, a POV, or a .dli plugin controller DirectInput can't see.
            return stance::CONF_NONE;
    }
    // A two-sided control bound on one side only gives half an axis, which is a reading but
    // not a whole one. So is any axis whose aid setting could not be read.
    const bool one_sided = a.bind.input != stance::IN_PAD_AXIS && a.bind.index2 < 0;
    if (one_sided || a.aid == stance::FLAG_UNKNOWN || src == stance::SRC_XINPUT) return stance::CONF_GUESS;
    return stance::CONF_SURE;
}

/// A raw device axis as -1..+1, given the range the device was set to report. `sign` is the
/// control line's own direction flag: 1 turns the axis round, as the game does.
inline float FromAxis(int32_t raw, int32_t lo, int32_t hi, int32_t sign) {
    if (hi <= lo) return Unknown();
    const float mid = float(lo) + float(hi - lo) * 0.5f;
    const float half = float(hi - lo) * 0.5f;
    float v = (float(raw) - mid) / half;
    if (v < -1.0f) v = -1.0f;
    if (v > 1.0f) v = 1.0f;
    return sign == 1 ? -v : v;
}

/// The two sides of a key or button pair as -1..+1. Both down is neither, the way holding
/// both directions on a keyboard leaves the rider in the middle.
inline float FromPair(bool negative, bool positive) {
    return float(int(positive) - int(negative));
}

/// Writes one axis of a LEAN_BIND record. `out` must have room for `kAxisSize`.
inline void WriteAxis(uint8_t* out, const Axis& a, Confidence conf, Source src) {
    std::memset(out, 0, kAxisSize);
    out[0] = uint8_t(a.bind.input);
    out[1] = uint8_t(a.aid);
    out[2] = uint8_t(conf);
    out[3] = uint8_t(src);
    const int32_t nums[3] = {a.bind.index, a.bind.index2, a.bind.sign};
    std::memcpy(out + 4, nums, sizeof(nums));
    std::memcpy(out + 16, a.bind.tuning, sizeof(a.bind.tuning));
    const size_t n = a.bind.device.size() < stance::kDeviceLen ? a.bind.device.size() : stance::kDeviceLen;
    std::memcpy(out + 56, a.bind.device.data(), n);
}

/// The whole LEAN_BIND record. `out` must have room for `kBindSize`.
inline void WriteBind(uint8_t* out, const Setup& s, Source src) {
    std::memset(out, 0, kBindSize);
    out[0] = kBindLayout;
    WriteAxis(out + 4, s.lr, Rate(s.lr, src), src);
    WriteAxis(out + 4 + kAxisSize, s.fb, Rate(s.fb, src), src);
}

/// One LEAN record. `out` must have room for `kEventSize`.
inline void WriteEvent(uint8_t* out, float time, float lap_pos, float lr, float fb) {
    const float v[4] = {time, lap_pos, lr, fb};
    std::memcpy(out, v, sizeof(v));
}

}  // namespace lean
