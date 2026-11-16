// sessionlog.h - what frostmod.log says when the session changes.
//
// The plugin API tells FrostMod when the session moves on (practice, warmup, race) and when the
// game drops a rider from the entry list. Both used to go only to the crash trail, so a rider
// who saw everyone vanish at the race start had nothing in the log to show for it. These are
// the pure parts of logging them; tests/sessionlog_test.cpp runs them anywhere.
#pragma once

#include <cstdint>
#include <cstring>

namespace sessionlog {

inline bool ReadSession(const void* data, int size, int* session, int* state) {
    if (!data || size < 8 || !session || !state) return false;
    std::int32_t values[2];
    std::memcpy(values, data, sizeof(values));
    *session = values[0];
    *state = values[1];
    return true;
}

inline const char* SessionName(int session) {
    static const char* const names[] = {
        "waiting", "practice", "pre-qualify", "qualify practice",
        "qualify", "warmup", "race 1", "race 2"
    };
    return session >= 0 && session < 8 ? names[session] : "unknown";
}

inline bool ShouldLogRemove(std::uint32_t n) {
    return n > 0 && (n <= 64 || (n & (n - 1)) == 0);
}

} // namespace sessionlog
