// keybinds.h - the editor's keys, and what they are called in the config file.
//
// The replay camera editor reads the keyboard directly (GetAsyncKeyState), which means the
// game reads the same key on the same frame. If you have `S` bound to move the camera
// backwards, pressing `S` to save a path also moves the camera - and no modifier fixes
// that, because the game does not care whether Ctrl was held. The only real fix is being
// able to move an action onto a key the game does not use, which is what this is for.
//
// Bindings live in the existing `frostmod_radar.cfg` as `rcam_save=F9` lines. Kept apart
// from Win32 so the name<->key table can be tested, since a name that silently fails to
// parse would strand someone with a key that does nothing.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace kb {

/// Modifiers are matched, not required: they let one physical key carry two actions where
/// the game leaves that key alone. They do NOT hide the key from the game.
struct Bind {
    uint16_t vk    = 0;      // Win32 virtual-key code; 0 = unbound
    bool     ctrl  = false;
    bool     alt   = false;
    bool     shift = false;

    bool bound() const { return vk != 0; }
    bool operator==(const Bind& o) const {
        return vk == o.vk && ctrl == o.ctrl && alt == o.alt && shift == o.shift;
    }
};

enum Action {
    RcSetKey = 0, RcDelete, RcClear, RcPlay, RcPrev, RcNext, RcSave, RcLoad,
    ActionCount
};

struct ActionInfo {
    const char* id;       // config key, without the `rcam_` prefix it is stored under
    const char* label;    // shown in the editor panel
    uint16_t    defVk;
    bool        defCtrl;
};

/// Defaults are the letters the feature shipped with. They are a starting point, not a
/// recommendation: whether they collide depends on your own camera bindings.
inline const ActionInfo* Actions() {
    static const ActionInfo kActions[ActionCount] = {
        { "setkey", "set key",    'K',  false },
        { "delete", "delete",     'X',  false },
        { "clear",  "clear",      'C',  false },
        { "play",   "play/stop",  'P',  false },
        { "prev",   "prev key",   0xBC, false },   // VK_OEM_COMMA
        { "next",   "next key",   0xBE, false },   // VK_OEM_PERIOD
        { "save",   "save",       'S',  false },
        { "load",   "load",       'L',  false },
    };
    return kActions;
}

inline Bind DefaultBind(int action) {
    const ActionInfo& a = Actions()[action];
    Bind b; b.vk = a.defVk; b.ctrl = a.defCtrl;
    return b;
}

// ---------------------------------------------------------------------------
// key names
// ---------------------------------------------------------------------------

struct NamedKey { const char* name; uint16_t vk; };

/// Keys that are not a plain letter or digit. First name for a vk is the one we print.
inline const NamedKey* NamedKeys(int* count) {
    static const NamedKey kKeys[] = {
        { "Space", 0x20 }, { "Tab", 0x09 }, { "Enter", 0x0D }, { "Backspace", 0x08 },
        { "Insert", 0x2D }, { "Delete", 0x2E }, { "Home", 0x24 }, { "End", 0x23 },
        { "PageUp", 0x21 }, { "PageDown", 0x22 },
        { "Left", 0x25 }, { "Up", 0x26 }, { "Right", 0x27 }, { "Down", 0x28 },
        { "F1", 0x70 }, { "F2", 0x71 }, { "F3", 0x72 }, { "F4", 0x73 },
        { "F5", 0x74 }, { "F6", 0x75 }, { "F7", 0x76 }, { "F8", 0x77 },
        { "F9", 0x78 }, { "F10", 0x79 }, { "F11", 0x7A }, { "F12", 0x7B },
        { "F13", 0x7C }, { "F14", 0x7D }, { "F15", 0x7E }, { "F16", 0x7F },
        { "Numpad0", 0x60 }, { "Numpad1", 0x61 }, { "Numpad2", 0x62 }, { "Numpad3", 0x63 },
        { "Numpad4", 0x64 }, { "Numpad5", 0x65 }, { "Numpad6", 0x66 }, { "Numpad7", 0x67 },
        { "Numpad8", 0x68 }, { "Numpad9", 0x69 },
        { "NumpadMultiply", 0x6A }, { "NumpadAdd", 0x6B }, { "NumpadSubtract", 0x6D },
        { "NumpadDecimal", 0x6E }, { "NumpadDivide", 0x6F },
        { "Comma", 0xBC }, { "Period", 0xBE }, { "Semicolon", 0xBA }, { "Slash", 0xBF },
        { "Backtick", 0xC0 }, { "Minus", 0xBD }, { "Equals", 0xBB },
        { "LeftBracket", 0xDB }, { "Backslash", 0xDC }, { "RightBracket", 0xDD },
        { "Quote", 0xDE },
        // aliases accepted on the way in
        { ",", 0xBC }, { ".", 0xBE }, { ";", 0xBA }, { "/", 0xBF }, { "`", 0xC0 },
        { "-", 0xBD }, { "=", 0xBB }, { "[", 0xDB }, { "\\", 0xDC }, { "]", 0xDD },
        { "'", 0xDE }, { "Esc", 0x1B }, { "Escape", 0x1B },
    };
    *count = (int)(sizeof(kKeys) / sizeof(kKeys[0]));
    return kKeys;
}

inline bool EqualsNoCase(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) return false;
    return *a == *b;
}

/// Virtual key for a key name, or 0. Single letters and digits map to themselves.
inline uint16_t VkFromName(const char* name) {
    if (!name || !*name) return 0;
    if (name[1] == '\0') {
        const unsigned char c = (unsigned char)name[0];
        if (std::isalpha(c)) return (uint16_t)std::toupper(c);
        if (std::isdigit(c)) return (uint16_t)c;
    }
    int n = 0;
    const NamedKey* keys = NamedKeys(&n);
    for (int i = 0; i < n; ++i)
        if (EqualsNoCase(keys[i].name, name)) return keys[i].vk;
    return 0;
}

/// The name we print for a virtual key. Empty for one we have no name for, which is how an
/// unknown key is kept out of a config file we would not be able to read back.
inline const char* NameFromVk(uint16_t vk, char* scratch, size_t n) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        std::snprintf(scratch, n, "%c", (char)vk);
        return scratch;
    }
    int count = 0;
    const NamedKey* keys = NamedKeys(&count);
    for (int i = 0; i < count; ++i)
        if (keys[i].vk == vk) return keys[i].name;
    return "";
}

// ---------------------------------------------------------------------------
// text form: "F9", "Ctrl+S", "Ctrl+Shift+Numpad1", "none"
// ---------------------------------------------------------------------------

inline bool ParseBind(const char* text, Bind* out) {
    if (!text || !out) return false;
    Bind b;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s", text);

    // trim
    char* s = buf;
    while (*s == ' ' || *s == '\t') ++s;
    size_t len = std::strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = '\0';
    if (!*s) return false;

    if (EqualsNoCase(s, "none")) { *out = Bind{}; return true; }

    // modifiers, then the key; "+" alone is a valid key name only as the last token
    for (;;) {
        char* plus = std::strchr(s, '+');
        if (!plus || plus == s || plus[1] == '\0') break;
        *plus = '\0';
        if      (EqualsNoCase(s, "ctrl")  || EqualsNoCase(s, "control")) b.ctrl = true;
        else if (EqualsNoCase(s, "alt"))                                 b.alt = true;
        else if (EqualsNoCase(s, "shift"))                               b.shift = true;
        else return false;
        s = plus + 1;
        while (*s == ' ') ++s;
    }

    b.vk = VkFromName(s);
    if (!b.vk) return false;
    *out = b;
    return true;
}

inline void FormatBind(const Bind& b, char* out, size_t n) {
    if (!b.bound()) { std::snprintf(out, n, "none"); return; }
    char scratch[8];
    const char* key = NameFromVk(b.vk, scratch, sizeof(scratch));
    if (!*key) { std::snprintf(out, n, "none"); return; }
    std::snprintf(out, n, "%s%s%s%s", b.ctrl ? "Ctrl+" : "", b.alt ? "Alt+" : "",
                  b.shift ? "Shift+" : "", key);
}

/// Config key for an action, e.g. `rcam_save`.
inline void ConfigKey(int action, char* out, size_t n) {
    std::snprintf(out, n, "rcam_%s", Actions()[action].id);
}

/// Read one `rcam_<id>=<bind>` line into `binds`. Returns whether the line was one of ours
/// AND parsed - so a malformed value leaves the default in place rather than unbinding it.
inline bool ApplyConfigLine(const char* line, Bind* binds) {
    if (!line || !binds) return false;
    const char* eq = std::strchr(line, '=');
    if (!eq) return false;
    char name[64];
    const size_t nameLen = (size_t)(eq - line);
    if (nameLen == 0 || nameLen >= sizeof(name)) return false;
    std::memcpy(name, line, nameLen);
    name[nameLen] = '\0';

    for (int a = 0; a < ActionCount; ++a) {
        char key[64];
        ConfigKey(a, key, sizeof(key));
        if (!EqualsNoCase(key, name)) continue;
        Bind parsed;
        if (!ParseBind(eq + 1, &parsed)) return false;
        binds[a] = parsed;
        return true;
    }
    return false;
}

inline void LoadDefaults(Bind* binds) {
    for (int a = 0; a < ActionCount; ++a) binds[a] = DefaultBind(a);
}

/// Index of an action bound to the same key as `action`, or -1. Two actions on one key is
/// allowed by the file format but means one of them can never be pressed alone.
inline int Conflict(const Bind* binds, int action) {
    if (!binds[action].bound()) return -1;
    for (int a = 0; a < ActionCount; ++a)
        if (a != action && binds[a].bound() && binds[a] == binds[action]) return a;
    return -1;
}

} // namespace kb
