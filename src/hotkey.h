// The key that opens the FrostMod menu (menukey= in frostmod_radar.cfg). F8 unless the
// player picks another: F8 is ReShade's screenshot key for some, and a menu that opens on
// every screenshot is a menu nobody can use.
//
// Pure (virtual-key codes as numbers, no <windows.h>) so the parse, the names and the
// "can this be the menu key" rule run in CI. The in-game half - polling the keyboard and the
// capture in the menu - is in frostmod.cpp next to the rest of the menu.
#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace frostmod {
namespace hotkey {

struct Hotkey {
    int  vk    = 0x77;     // VK_F8
    bool ctrl  = false;
    bool shift = false;
    bool alt   = false;
};

inline bool operator==(const Hotkey& a, const Hotkey& b) {
    return a.vk == b.vk && a.ctrl == b.ctrl && a.shift == b.shift && a.alt == b.alt;
}
inline bool operator!=(const Hotkey& a, const Hotkey& b) { return !(a == b); }

inline Hotkey Default() { return Hotkey{}; }

// One int, so the renderers can read the key the input thread set without a lock.
inline int Pack(const Hotkey& k) {
    return (k.vk & 0xFF) | (k.ctrl ? 0x100 : 0) | (k.shift ? 0x200 : 0) | (k.alt ? 0x400 : 0);
}
inline Hotkey Unpack(int p) {
    Hotkey k;
    k.vk = p & 0xFF; k.ctrl = (p & 0x100) != 0; k.shift = (p & 0x200) != 0; k.alt = (p & 0x400) != 0;
    return k;
}

// Names for the keys that are not a letter, a digit or an F-key. The first name for a code
// is the one written back; the rest are accepted when read.
struct KeyName { int vk; const char* name; };
inline const KeyName* NamedKeys(int* count) {
    static const KeyName k[] = {
        {0x08, "Backspace"}, {0x09, "Tab"},       {0x0D, "Enter"},    {0x13, "Pause"},
        {0x14, "CapsLock"},  {0x1B, "Esc"},       {0x1B, "Escape"},   {0x20, "Space"},
        {0x21, "PageUp"},    {0x21, "PgUp"},      {0x22, "PageDown"}, {0x22, "PgDn"},
        {0x23, "End"},       {0x24, "Home"},      {0x25, "Left"},     {0x26, "Up"},
        {0x27, "Right"},     {0x28, "Down"},      {0x2D, "Insert"},   {0x2D, "Ins"},
        {0x2E, "Delete"},    {0x2E, "Del"},       {0x90, "NumLock"},  {0x91, "ScrollLock"},
        {0x6A, "NumMultiply"}, {0x6B, "NumAdd"},  {0x6D, "NumSubtract"},
        {0x6E, "NumDecimal"},  {0x6F, "NumDivide"},
        {0xBA, ";"}, {0xBB, "="}, {0xBC, ","}, {0xBD, "-"}, {0xBE, "."}, {0xBF, "/"},
        {0xC0, "`"}, {0xDB, "["}, {0xDC, "\\"}, {0xDD, "]"}, {0xDE, "'"},
        // Modifiers and mouse buttons only so a refusal can name them.
        {0x10, "Shift"}, {0x11, "Ctrl"}, {0x12, "Alt"},
        {0xA0, "LShift"}, {0xA1, "RShift"}, {0xA2, "LCtrl"}, {0xA3, "RCtrl"},
        {0xA4, "LAlt"}, {0xA5, "RAlt"}, {0x5B, "LWin"}, {0x5C, "RWin"},
        {0x01, "Mouse1"}, {0x02, "Mouse2"}, {0x04, "Mouse3"}, {0x05, "Mouse4"}, {0x06, "Mouse5"},
    };
    *count = (int)(sizeof(k) / sizeof(k[0]));
    return k;
}

inline bool EqNoCase(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) return false;
    return *a == *b;
}

// The name of a key on its own: "F8", "K", "7", "Num4", "Insert", "VK_E2".
inline void KeyNameOf(int vk, char* out, size_t n) {
    if (!out || n == 0) return;
    if (vk >= 0x70 && vk <= 0x87) { std::snprintf(out, n, "F%d", vk - 0x70 + 1); return; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { std::snprintf(out, n, "%c", (char)vk); return; }
    if (vk >= 0x60 && vk <= 0x69) { std::snprintf(out, n, "Num%d", vk - 0x60); return; }
    int c = 0; const KeyName* k = NamedKeys(&c);
    for (int i = 0; i < c; ++i) if (k[i].vk == vk) { std::snprintf(out, n, "%s", k[i].name); return; }
    std::snprintf(out, n, "VK_%02X", vk & 0xFF);
}

// The inverse of KeyNameOf for one key name. Case does not matter; "VK_xx" and "0xNN" take a
// raw virtual-key code for anything without a name.
inline bool ParseKeyName(const char* s, int* vk) {
    if (!s || !*s) return false;
    const size_t len = std::strlen(s);
    if (len == 1) {
        const int c = std::toupper((unsigned char)s[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) { *vk = c; return true; }
    }
    if ((s[0] == 'F' || s[0] == 'f') && len >= 2 && len <= 3 &&
        std::isdigit((unsigned char)s[1]) && (len == 2 || std::isdigit((unsigned char)s[2]))) {
        const int f = std::atoi(s + 1);
        if (f >= 1 && f <= 24) { *vk = 0x70 + f - 1; return true; }
        return false;
    }
    if (len == 4 && (s[0] == 'N' || s[0] == 'n') && (s[1] == 'U' || s[1] == 'u') &&
        (s[2] == 'M' || s[2] == 'm') && std::isdigit((unsigned char)s[3])) {
        *vk = 0x60 + (s[3] - '0'); return true;
    }
    const bool vkForm  = len == 5 && (s[0] == 'V' || s[0] == 'v') && (s[1] == 'K' || s[1] == 'k') && s[2] == '_';
    const bool hexForm = len == 4 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    if (vkForm || hexForm) {
        const char* digits = s + (vkForm ? 3 : 2);
        if (!std::isxdigit((unsigned char)digits[0]) || !std::isxdigit((unsigned char)digits[1]))
            return false;
        const long v = std::strtol(digits, nullptr, 16);
        if (v > 0 && v < 0xFF) { *vk = (int)v; return true; }
        return false;
    }
    int c = 0; const KeyName* k = NamedKeys(&c);
    for (int i = 0; i < c; ++i) if (EqNoCase(k[i].name, s)) { *vk = k[i].vk; return true; }
    return false;
}

// "F8", "Ctrl+F9", "ctrl + shift + insert". Modifiers first, in any order, then one key.
inline bool Parse(const char* text, Hotkey* out) {
    if (!text || !out) return false;
    char buf[64];
    size_t j = 0;
    for (const char* p = text; *p && j + 1 < sizeof(buf); ++p)
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') buf[j++] = *p;
    buf[j] = 0;
    if (!buf[0]) return false;

    Hotkey k; k.vk = 0;
    char* part = buf;
    for (;;) {
        if (!*part) return false;                  // "Ctrl+" with no key after it
        char* plus = std::strchr(part, '+');
        if (!plus) break;
        *plus = 0;
        if      (EqNoCase(part, "Ctrl") || EqNoCase(part, "Control")) k.ctrl  = true;
        else if (EqNoCase(part, "Shift"))                             k.shift = true;
        else if (EqNoCase(part, "Alt"))                               k.alt   = true;
        else return false;
        part = plus + 1;
    }
    if (!ParseKeyName(part, &k.vk)) return false;
    *out = k;
    return true;
}

// "Ctrl+Shift+F9". Parse(Format(k)) == k for every key KeyNameOf names.
inline void Format(const Hotkey& k, char* out, size_t n) {
    if (!out || n == 0) return;
    char key[24]; KeyNameOf(k.vk, key, sizeof(key));
    std::snprintf(out, n, "%s%s%s%s", k.ctrl ? "Ctrl+" : "", k.shift ? "Shift+" : "",
                  k.alt ? "Alt+" : "", key);
}

inline bool IsModifier(int vk) {
    return vk == 0x10 || vk == 0x11 || vk == 0x12 || (vk >= 0xA0 && vk <= 0xA5) ||
           vk == 0x5B || vk == 0x5C;
}
inline bool IsMouse(int vk) { return vk >= 0x01 && vk <= 0x06 && vk != 0x03; }

// Whether a key may open the menu. A refusal says why, for the status line and the log.
// Esc cancels the capture and closes the menu; a bare letter or digit is a menu row (and
// typing in chat); Enter, Backspace, Space, Tab and the arrows drive the open panels; F7 is
// clean view and Ctrl+Shift+H hides the Coach overlay.
inline bool Bindable(const Hotkey& k, const char** why) {
    const char* w = nullptr;
    const bool bare = !k.ctrl && !k.shift && !k.alt;
    if (k.vk <= 0 || k.vk >= 0xFF || k.vk == 0x03)  w = "not a key";
    else if (IsMouse(k.vk))                          w = "mouse buttons can't open the menu";
    else if (IsModifier(k.vk))                       w = "add a key to the modifier";
    else if (k.vk == 0x1B)                           w = "Esc closes the menu";
    else if (bare && ((k.vk >= 'A' && k.vk <= 'Z') || (k.vk >= '0' && k.vk <= '9')))
        w = "letters and digits need Ctrl, Shift or Alt";
    else if (bare && (k.vk == 0x08 || k.vk == 0x09 || k.vk == 0x0D || k.vk == 0x20 ||
                      (k.vk >= 0x25 && k.vk <= 0x28)))
        w = "that key drives the menu's lists";
    else if (bare && k.vk == 0x76)                   w = "F7 is clean view";
    else if (k.ctrl && k.shift && !k.alt && k.vk == 'H') w = "Ctrl+Shift+H hides the Coach overlay";
    if (why) *why = w;
    return w == nullptr;
}

// A press of the menu key. The plain key ignores the modifiers, which is how F8 has always
// behaved; a combination needs exactly its modifiers held.
inline bool Matches(const Hotkey& k, bool ctrl, bool shift, bool alt) {
    if (!k.ctrl && !k.shift && !k.alt) return true;
    return k.ctrl == ctrl && k.shift == shift && k.alt == alt;
}

}  // namespace hotkey
}  // namespace frostmod
