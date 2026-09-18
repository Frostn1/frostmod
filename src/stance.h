// stance.h - whether the rider sits or stands, for MXB Coach's recorder.
//
// The plugin API has no rider pose, and physics can't tell sitting from standing. So the
// recorder watches the rider's own Sit bind instead. The game has one Sit action, set up in
// the rider's profile:
//   profile.ini   [input] sit_direct=1   hold the bind to sit     (control CTRL_SITDirect)
//                 [input] sit_direct=0   press it to sit or stand (control CTRL_SIT)
//                 [aids]  autoridersit=1 the game sits the rider itself: stance unknown
//   controls.txt  one line per control, as the game's writer emits them: the case-sensitive
//                 CTRL_* name, the binding tokens, then always 10 tuning values
//                 (%f %f %f %d %f %f %d %f %f %f) that this reader never looks at:
//                   CTRL_SIT KEY <DIK scan code> [second code, two-sided controls]
//                   CTRL_SIT BUTTON <guid> <button>   DirectInput pad, rgbButtons index
//                   CTRL_SIT AXIS <guid> <n> <0|1>    DirectInput pad axis
//                   CTRL_SIT POV <guid> <n>           DirectInput pad hat
//                   CTRL_SIT C_BUTTON <plugin id> ... a .dli plugin controller, as are
//                                                     C_AXIS C_SLIDER C_POV C_DIAL: not
//                                                     readable through DirectInput
//                   CTRL_SIT 0.000000 ...             unbound: straight to the tuning values
//                 <guid> is the device's DIDEVICEINSTANCE.guidInstance, upper case, no braces.
//                 Only KEY and BUTTON can be polled; the rest record stance as unknown.
//   global.ini    lastprofile=<the profile folder in use>
//
// No Win32 here, so tests/stance_test.cpp runs anywhere. mxbcoach.cpp reads the files,
// polls the bind and writes the records.
//
// Records in the .mxbc file (tags in coachrec.h; little-endian):
//   STANCE_BIND  52 bytes, once per stint, right after SESSION
//      0  u8   layout version (1)
//      1  u8   input: 0 none, 1 key, 2 controller button, 3 axis, 4 POV,
//              5 other (a .dli plugin controller)
//      2  u8   mode: 0 hold, 1 toggle, 2 unknown
//      3  u8   auto-sit: 0 off, 1 on, 2 unknown
//      4  u8   confidence: 0 none (every STANCE is unknown), 1 guess, 2 sure
//      5  u8   read through: 0 nothing, 1 keyboard, 2 DirectInput, 3 XInput
//      6  u16  zero
//      8  i32  scan code for a key, button number for a controller, -1 for none
//      12 char[40] device GUID as the game wrote it, NUL-padded ("" for a key or other)
//   STANCE       12 bytes, on the stint's first sample and then only when stance changes
//      0  f32  track time s        (as in the SAMPLE it came with)
//      4  f32  lap position 0..1
//      8  u8   0 stand, 1 sit, 2 unknown
//      9  u8[3] zero
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace stance {

enum Input : uint8_t { IN_NONE = 0, IN_KEY = 1, IN_PAD_BUTTON = 2, IN_PAD_AXIS = 3, IN_PAD_POV = 4, IN_OTHER = 5 };
enum Mode : uint8_t { HOLD = 0, TOGGLE = 1, MODE_UNKNOWN = 2 };
enum Flag : uint8_t { FLAG_OFF = 0, FLAG_ON = 1, FLAG_UNKNOWN = 2 };
enum Confidence : uint8_t { CONF_NONE = 0, CONF_GUESS = 1, CONF_SURE = 2 };
enum Source : uint8_t { SRC_NONE = 0, SRC_KEYBOARD = 1, SRC_DIRECTINPUT = 2, SRC_XINPUT = 3 };
enum State : uint8_t { STAND = 0, SIT = 1, STANCE_UNKNOWN = 2 };

constexpr uint8_t kBindLayout = 1;
constexpr size_t  kBindSize   = 52;
constexpr size_t  kDeviceLen  = 40;
constexpr size_t  kEventSize  = 12;

constexpr const char* kHoldControl   = "CTRL_SITDirect";
constexpr const char* kToggleControl = "CTRL_SIT";

struct Bind {
    Input       input = IN_NONE;
    std::string device;       // controller GUID, as written
    int32_t     index  = -1;  // scan code or button number
    /// The second code of a two-sided control: the other direction's key, or the other
    /// button. Sit has one side and leaves this -1; the lean controls have two.
    int32_t     index2 = -1;
    /// An AXIS line's trailing direction flag, 0 or 1; -1 when there isn't one.
    int32_t     sign   = -1;
    /// The ten tuning values every line ends with: deadzone, linearity, gain and the
    /// smoothing either side, in the game's own order. Recorded rather than applied, so the
    /// curve can be worked out later without the recording having baked one in.
    float       tuning[10] = {};
    bool        has_tuning = false;
};

// ---------------------------------------------------------------------------------------
// Text

inline std::string Trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

inline bool IEq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

inline std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    size_t at = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        out.push_back(text.substr(at, nl - at));
        at = nl + 1;
    }
    return out;
}

inline std::vector<std::string> Tokens(const std::string& line) {
    std::vector<std::string> out;
    size_t at = 0;
    while (true) {
        at = line.find_first_not_of(" \t\r\n", at);
        if (at == std::string::npos) break;
        const size_t end = (std::min)(line.find_first_of(" \t\r\n", at), line.size());
        out.push_back(line.substr(at, end - at));
        at = end;
    }
    return out;
}

inline bool ToInt(const std::string& s, int32_t& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (!end || *end || v < INT32_MIN || v > INT32_MAX) return false;
    out = int32_t(v);
    return true;
}

inline bool ToFloat(const std::string& s, float& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (!end || *end) return false;
    out = float(v);
    return true;
}

/// A value from an .ini file. An empty section matches any section. "" when absent.
inline std::string IniValue(const std::string& text, const std::string& section, const std::string& key) {
    std::string cur;
    for (const std::string& raw : Lines(text)) {
        const std::string l = Trim(raw);
        if (l.empty() || l[0] == ';' || l[0] == '#') continue;
        if (l[0] == '[') {
            const size_t e = l.find(']');
            cur = Trim(l.substr(1, e == std::string::npos ? std::string::npos : e - 1));
            continue;
        }
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        if ((section.empty() || IEq(cur, section)) && IEq(Trim(l.substr(0, eq)), key)) return Trim(l.substr(eq + 1));
    }
    return {};
}

// ---------------------------------------------------------------------------------------
// controls.txt

/// Laid out like a Win32 GUID, so it compares with memcmp against one.
struct Guid {
    uint32_t d1 = 0;
    uint16_t d2 = 0, d3 = 0;
    uint8_t  d4[8] = {};
};
static_assert(sizeof(Guid) == 16, "Guid must match the Win32 layout");

/// "6F1D2B61-D5A0-11CF-BFC7-444553540000", with or without braces, any case.
inline bool ParseGuid(std::string s, Guid& out) {
    if (s.size() == 38 && s.front() == '{' && s.back() == '}') s = s.substr(1, 36);
    if (s.size() != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return false;
    uint8_t b[16];
    size_t  k = 0;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        const int c = std::tolower(static_cast<unsigned char>(s[i]));
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return false;
        if (k % 2 == 0) b[k / 2] = uint8_t(v << 4);
        else b[k / 2] |= uint8_t(v);
        ++k;
    }
    Guid g;
    g.d1 = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
    g.d2 = uint16_t(b[4] << 8 | b[5]);
    g.d3 = uint16_t(b[6] << 8 | b[7]);
    std::memcpy(g.d4, b + 8, 8);
    out = g;
    return true;
}

/// One controls.txt line: the control's name and its bind. False if it isn't a bind line.
/// Reads only the binding tokens after the name. A device must be a GUID and a number an
/// integer, so a tuning float is never taken for either; a line that goes straight from the
/// name to the tuning values is unbound.
inline bool ParseLine(const std::string& line, std::string& name, Bind& out) {
    const std::vector<std::string> t = Tokens(line);
    if (t.size() < 2) return false;
    name = t[0];
    out  = Bind{};
    const std::string& type = t[1];
    Guid       g;
    int32_t    n   = -1;
    const bool dev = t.size() >= 3 && ParseGuid(t[2], g);
    const bool num = dev && t.size() >= 4 && ToInt(t[3], n);
    // The ten tuning values close every line. Reading them from the end is what keeps the
    // binding tokens unambiguous: a tuning value is written as a float, so `ToInt` refuses
    // it and a missing optional token can never be mistaken for the first of the ten.
    if (t.size() >= 10) {
        float  tune[10];
        size_t at = t.size() - 10;
        bool   all = true;
        for (size_t i = 0; i < 10 && all; ++i) all = ToFloat(t[at + i], tune[i]);
        if (all) {
            std::memcpy(out.tuning, tune, sizeof(tune));
            out.has_tuning = true;
        }
    }
    int32_t second = -1;
    if (type == "KEY") {
        // A two-sided control binds one code per direction; Sit has one and leaves it -1.
        if (t.size() >= 3 && ToInt(t[2], n) && n > 0 && n < 256) {
            out.input = IN_KEY;
            out.index = n;
            if (t.size() >= 4 && ToInt(t[3], second) && second > 0 && second < 256) out.index2 = second;
        }
    } else if (type == "BUTTON") {
        if (num && n >= 0 && n < 128) {
            out.input  = IN_PAD_BUTTON;
            out.device = t[2];
            out.index  = n;
            if (t.size() >= 5 && ToInt(t[4], second) && second >= 0 && second < 128) out.index2 = second;
        }
    } else if (type == "AXIS" || type == "POV") {
        out.input = type == "AXIS" ? IN_PAD_AXIS : IN_PAD_POV;
        if (dev) out.device = t[2];
        if (num) out.index = n;
        // An axis line ends its binding with the direction flag the game applies to it.
        if (type == "AXIS" && num && t.size() >= 5 && ToInt(t[4], second) && (second == 0 || second == 1))
            out.sign = second;
    } else if (type == "C_BUTTON" || type == "C_AXIS" || type == "C_SLIDER" || type == "C_POV" ||
               type == "C_DIAL") {
        out.input = IN_OTHER;  // a .dli plugin controller: DirectInput can't see it
    }
    return true;
}

/// The bind of one control, by exact name. IN_NONE if it isn't there or isn't bound.
inline Bind FindBind(const std::string& controls, const std::string& control) {
    for (const std::string& line : Lines(controls)) {
        std::string name;
        Bind b;
        if (ParseLine(line, name, b) && name == control) return b;
    }
    return {};
}

// ---------------------------------------------------------------------------------------
// The rider's setup

struct Setup {
    Mode mode     = MODE_UNKNOWN;
    Flag auto_sit = FLAG_UNKNOWN;
    Bind bind;  // of the control the mode uses
};

inline Setup ReadSetup(const std::string& profile_ini, const std::string& controls) {
    Setup s;
    const std::string direct = IniValue(profile_ini, "input", "sit_direct");
    if (direct == "1") s.mode = HOLD;
    else if (direct == "0") s.mode = TOGGLE;
    const std::string autosit = IniValue(profile_ini, "aids", "autoridersit");
    if (autosit == "1") s.auto_sit = FLAG_ON;
    else if (autosit == "0") s.auto_sit = FLAG_OFF;
    if (s.mode != MODE_UNKNOWN) s.bind = FindBind(controls, s.mode == HOLD ? kHoldControl : kToggleControl);
    return s;
}

/// How far a stint's stance can be trusted, given what could be polled.
/// Toggle is a guess because a missed press flips every sample after it. XInput is a guess
/// because it is the first pad, not necessarily the one bound.
inline Confidence Rate(const Setup& s, Source src) {
    if (s.mode == MODE_UNKNOWN || s.auto_sit == FLAG_ON || src == SRC_NONE) return CONF_NONE;
    if (s.bind.input != IN_KEY && s.bind.input != IN_PAD_BUTTON) return CONF_NONE;
    if (s.mode == TOGGLE || src == SRC_XINPUT || s.auto_sit == FLAG_UNKNOWN) return CONF_GUESS;
    return CONF_SURE;
}

// ---------------------------------------------------------------------------------------
// Devices

/// An Xbox pad's DirectInput button number as an XINPUT_GAMEPAD button bit. 0 past 9.
inline uint16_t XInputMask(int32_t button) {
    // A B X Y LB RB Back Start LS RS
    static const uint16_t kMask[10] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0100,
                                       0x0200, 0x0020, 0x0010, 0x0040, 0x0080};
    return button >= 0 && button < 10 ? kMask[button] : 0;
}

/// A DirectInput scan code as a Win32 virtual key, for keys that sit in the same place on
/// every layout. 0 for letters, digits and punctuation: those follow the layout, so the
/// plugin asks Windows (MapVirtualKey) for them.
inline int FixedVk(int32_t dik) {
    switch (dik) {
        case 0x01: return 0x1B;  // Esc
        case 0x0E: return 0x08;  // Backspace
        case 0x0F: return 0x09;  // Tab
        case 0x1C: return 0x0D;  // Enter
        case 0x1D: return 0xA2;  // left Ctrl
        case 0x2A: return 0xA0;  // left Shift
        case 0x36: return 0xA1;  // right Shift
        case 0x37: return 0x6A;  // numpad *
        case 0x38: return 0xA4;  // left Alt
        case 0x39: return 0x20;  // Space
        case 0x3A: return 0x14;  // Caps Lock
        case 0x45: return 0x90;  // Num Lock
        case 0x46: return 0x91;  // Scroll Lock
        case 0x47: return 0x67;  // numpad 7
        case 0x48: return 0x68;  // numpad 8
        case 0x49: return 0x69;  // numpad 9
        case 0x4A: return 0x6D;  // numpad -
        case 0x4B: return 0x64;  // numpad 4
        case 0x4C: return 0x65;  // numpad 5
        case 0x4D: return 0x66;  // numpad 6
        case 0x4E: return 0x6B;  // numpad +
        case 0x4F: return 0x61;  // numpad 1
        case 0x50: return 0x62;  // numpad 2
        case 0x51: return 0x63;  // numpad 3
        case 0x52: return 0x60;  // numpad 0
        case 0x53: return 0x6E;  // numpad .
        case 0x57: return 0x7A;  // F11
        case 0x58: return 0x7B;  // F12
        case 0x9C: return 0x0D;  // numpad Enter
        case 0x9D: return 0xA3;  // right Ctrl
        case 0xB5: return 0x6F;  // numpad /
        case 0xB7: return 0x2C;  // Print Screen
        case 0xB8: return 0xA5;  // right Alt
        case 0xC5: return 0x13;  // Pause
        case 0xC7: return 0x24;  // Home
        case 0xC8: return 0x26;  // Up
        case 0xC9: return 0x21;  // Page Up
        case 0xCB: return 0x25;  // Left
        case 0xCD: return 0x27;  // Right
        case 0xCF: return 0x23;  // End
        case 0xD0: return 0x28;  // Down
        case 0xD1: return 0x22;  // Page Down
        case 0xD2: return 0x2D;  // Insert
        case 0xD3: return 0x2E;  // Delete
        case 0xDB: return 0x5B;  // left Windows
        case 0xDC: return 0x5C;  // right Windows
        case 0xDD: return 0x5D;  // Menu
        default: break;
    }
    if (dik >= 0x3B && dik <= 0x44) return 0x70 + (dik - 0x3B);  // F1-F10
    return 0;
}

// ---------------------------------------------------------------------------------------
// Stance over a stint

/// Turns bind polls into stance. Hold: sitting while the bind is down. Toggle: each press
/// flips it, and a crash puts the rider back to standing, as the game does.
class Tracker {
public:
    /// A new stint. `usable` false means every sample is unknown.
    void configure(Mode mode, bool usable) {
        mode_   = mode;
        usable_ = usable && mode != MODE_UNKNOWN;
        reset();
    }

    void reset() {
        sat_    = false;
        down_   = false;
        primed_ = false;
        have_   = false;
        state_  = STANCE_UNKNOWN;
    }

    /// One sample. `readable` false when the bind couldn't be read now (the game lost focus,
    /// the pad went away). True when the stance to record changed, and on the first sample.
    bool update(bool readable, bool pressed, bool crashed) {
        State next = STANCE_UNKNOWN;
        if (usable_ && readable) {
            if (mode_ == HOLD) sat_ = pressed;
            else if (primed_ && pressed && !down_ && !crashed) sat_ = !sat_;
            if (crashed) sat_ = false;
            down_   = pressed;
            primed_ = true;
            next    = sat_ ? SIT : STAND;
        } else {
            // A press seen only after a gap may have started inside it: don't count it.
            primed_ = false;
        }
        const bool changed = !have_ || next != state_;
        have_  = true;
        state_ = next;
        return changed;
    }

    State state() const { return state_; }

private:
    Mode  mode_   = MODE_UNKNOWN;
    bool  usable_ = false;
    bool  sat_    = false;  // toggle mode's own count of presses
    bool  down_   = false;
    bool  primed_ = false;
    bool  have_   = false;
    State state_  = STANCE_UNKNOWN;
};

// ---------------------------------------------------------------------------------------
// Payloads

inline std::vector<uint8_t> BindPayload(const Setup& s, Source src, Confidence c) {
    std::vector<uint8_t> p(kBindSize, 0);
    p[0] = kBindLayout;
    p[1] = s.bind.input;
    p[2] = s.mode;
    p[3] = s.auto_sit;
    p[4] = c;
    p[5] = src;
    const int32_t index = s.bind.input == IN_NONE ? -1 : s.bind.index;
    std::memcpy(&p[8], &index, 4);
    std::memcpy(&p[12], s.bind.device.data(), (std::min)(s.bind.device.size(), kDeviceLen - 1));
    return p;
}

inline std::array<uint8_t, kEventSize> EventPayload(float t, float pos, State st) {
    std::array<uint8_t, kEventSize> p{};
    std::memcpy(&p[0], &t, 4);
    std::memcpy(&p[4], &pos, 4);
    p[8] = st;
    return p;
}

}  // namespace stance
