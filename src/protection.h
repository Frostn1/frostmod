// protection.h - what FrostMod's crash guards caught on this PC, kept for MXB App to show.
//
// Every guard already logs and leaves a crash-trail note when it fires. This keeps the part a
// player cares about: which crash it was, when, and on which track and server. MXB App reads the
// file for its home card ("FrostMod protected you N times this week"), the note after a session
// and the Crash protection page.
//
// WHAT COUNTS AS ONE. A guard fires where the game would have gone to desktop, so the first
// fire of a kind in a session is one crash that did not happen; whatever the same kind does
// later in that session happened only because the first was stopped, and is not counted again.
// "Session" is the game's event (EventInit .. EventDeinit), or the menus outside one. The
// physics NaN is one kind even though four guards meet it (terrain, second grid, shock,
// nanrecover): they are four readers of the same NaN, one crash.
//
// Not counted at all: the map guard's NULL on the no-buffer path. It fires up to 262144 times
// a run on an empty batch that writes nothing and never crashed.
//
// HOW. Catch() is one atomic store, safe from an exception handler and from a stub's tick.
// Flush() runs from the frame tick: it turns the flags into lines appended to
// frostmod-protection.jsonl beside the crash reports, one JSON object per line:
//   {"t":1794531999,"kind":"render-batch","site":"mxbikes.exe+0x24085C","track":"...","server":"...","v":"0.52.0"}
// Append-only, so a crash mid-write loses at most the line being written; the reader skips a
// line it cannot parse. Trim() keeps the file to the newest kMaxLines at start-up.
//
// No Win32 here, so tests/protection_test.cpp runs anywhere.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace protection {

enum class Kind : int {
    PhysicsNan = 0,   // +0x1A29AE, +0x1F1923, +0x268480, +0x1EEA9C, +0x2469BB ...
    RenderBatch,      // +0x24085C, +0x2408C4, +0x240904
    BadText,          // msvcr90+0x36EDE (tyre fallback, trainer clean)
    TrainerClose,     // +0x11D753
    BikeList,         // +0x45B0
    Wake,             // +0x3B940
    ListBox,          // +0x1FED9E
    StackPointer,     // +0x2A42F0
    RiderInfo,        // +0x7A860, +0x7A8E0
    Count
};
constexpr int kKinds = (int)Kind::Count;

struct KindInfo {
    const char* id;     // stable: MXB App keys on it
    const char* site;   // the crash it stands for, as the reports name it
    const char* name;   // plain words, for the log line
};
inline const KindInfo& Info(Kind k) {
    static const KindInfo t[kKinds] = {
        {"physics-nan",   "mxbikes.exe+0x1A29AE", "bike physics went NaN (crash or fence hit)"},
        {"render-batch",  "mxbikes.exe+0x24085C", "track objects drawn to bad memory"},
        {"bad-text",      "MSVCR90.dll+0x36EDE",  "tyre or trainer name the game could not print"},
        {"trainer-close", "mxbikes.exe+0x11D753", "old trainer file closed twice"},
        {"bike-list",     "mxbikes.exe+0x45B0",   "bike list read after leaving a session"},
        {"wake",          "mxbikes.exe+0x3B940",  "water wake after the track unloaded"},
        {"list-box",      "mxbikes.exe+0x1FED9E", "menu list line naming a missing item"},
        {"stack-pointer", "mxbikes.exe+0x2A42F0", "pointer cut to 32 bits at launch"},
        {"rider-info",    "mxbikes.exe+0x7A860",  "someone joined your server mid-handshake"},
    };
    const int i = (int)k;
    return t[i >= 0 && i < kKinds ? i : 0];
}

constexpr size_t kMaxLines = 2000;

/// The per-session bookkeeping: which kinds fired (set from anywhere) and which of those have
/// already been written this session.
class Ledger {
public:
    void Catch(Kind k) {
        const int i = (int)k;
        if (i >= 0 && i < kKinds) fired_[i].store(true, std::memory_order_relaxed);
    }
    /// A new session: every kind may count once more.
    void NewSession() {
        for (int i = 0; i < kKinds; ++i) written_[i] = false;
    }
    /// The kinds to write now, as a bitmask; clears the flags and marks them written. A kind
    /// already written this session is dropped.
    uint32_t Take() {
        uint32_t mask = 0;
        for (int i = 0; i < kKinds; ++i) {
            if (!fired_[i].exchange(false, std::memory_order_relaxed)) continue;
            if (written_[i]) continue;
            written_[i] = true;
            mask |= 1u << i;
        }
        return mask;
    }
private:
    std::atomic<bool> fired_[kKinds] = {};
    bool written_[kKinds] = {};
};

/// JSON string body: quotes, backslashes and control characters escaped, at most `max` bytes in.
inline std::string Escape(const char* s, size_t max = 95) {
    std::string out;
    if (!s) return out;
    for (size_t i = 0; s[i] && i < max; ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += (char)c;
    }
    return out;
}

/// One line of the file, newline included.
inline std::string Line(Kind k, int64_t unixTime, const char* track, const char* server,
                        const char* version) {
    const KindInfo& in = Info(k);
    std::string s = "{\"t\":" + std::to_string((long long)unixTime) + ",\"kind\":\"" + in.id +
                    "\",\"site\":\"" + in.site + "\",\"track\":\"" + Escape(track) +
                    "\",\"server\":\"" + Escape(server) + "\",\"v\":\"" + Escape(version, 31) +
                    "\"}\n";
    return s;
}

/// Of `text` (the whole file), the newest `keep` lines. Unchanged when it is already short.
inline std::string KeepNewest(const std::string& text, size_t keep) {
    size_t lines = 0;
    for (char c : text) if (c == '\n') ++lines;
    if (lines <= keep) return text;
    size_t skip = lines - keep, i = 0;
    while (skip && i < text.size()) { if (text[i++] == '\n') --skip; }
    return text.substr(i);
}

}  // namespace protection
