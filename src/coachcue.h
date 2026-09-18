// coachcue.h - MXB Coach's live cues: the cue file the app writes, and the rules for when a
// cue shows during a lap.
//
// The app picks the cues: the places the rider loses time, rated by importance and filtered by
// the rider's level and how much coaching they asked for. It writes them per track and bike.
// The plugin only plays them: each one shows a moment before its spot, a few a lap at most,
// and only in practice. No Win32 here, so tests/coachcue_test.cpp runs anywhere.
//
// File layout (little-endian):
//   "MXCQ"  u32 format version
//   f32 track length m, f32 lead s, f32 show s, f32 gap s, u32 max cues a lap, u32 count
//   then count records:  f32 at m, u8 kind, u8 priority, u16 text length, utf8 text
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coachcue {

constexpr char     kMagic[4]  = {'M', 'X', 'C', 'Q'};
constexpr uint32_t kVersion   = 1;
constexpr uint32_t kMaxCues   = 64;
// SPluginString_t holds 100 bytes, the terminator included.
constexpr size_t kMaxText = 99;
constexpr size_t kHeader  = 32;

enum Kind : uint8_t {
    BRAKE      = 1,
    OFF_BRAKES = 2,
    THROTTLE   = 3,
    UPSHIFT    = 4,
    DOWNSHIFT  = 5,
    WIDE       = 6,
    INSIDE     = 7,
    SCRUB      = 8,
    STAND      = 9,
    SIT        = 10,
    CUSTOM     = 11,
    // Over-jumping: roll off before the lip. Spoken at the face, not the landing, because
    // the face is the last place the rider can change where they come down.
    ROLL       = 12,
};

struct Cue {
    float       at_m     = 0;
    uint8_t     kind     = CUSTOM;
    uint8_t     priority = 0;
    std::string text;
};

struct Sheet {
    float    track_len   = 0;
    float    lead_s      = 1.2f;  // how long before the spot a cue shows, at the bike's speed
    float    show_s      = 1.5f;  // how long it stays up
    float    gap_s       = 3.0f;  // at least this long between two cues
    uint32_t max_per_lap = 4;
    std::vector<Cue> cues;        // by position along the lap
};

inline uint32_t U32(const uint8_t* b) {
    uint32_t v;
    std::memcpy(&v, b, 4);
    return v;
}
inline float F32(const uint8_t* b) {
    float v;
    std::memcpy(&v, b, 4);
    return v;
}

/// Reads a cue file. Refuses anything that isn't exactly what the app writes, rather than
/// guessing at a damaged one.
inline bool Parse(const uint8_t* b, size_t n, Sheet& out) {
    if (!b || n < kHeader || std::memcmp(b, kMagic, 4) != 0 || U32(b + 4) != kVersion) return false;
    Sheet s;
    s.track_len   = F32(b + 8);
    s.lead_s      = F32(b + 12);
    s.show_s      = F32(b + 16);
    s.gap_s       = F32(b + 20);
    s.max_per_lap = U32(b + 24);
    const uint32_t count = U32(b + 28);
    if (count > kMaxCues || !std::isfinite(s.track_len) || s.track_len <= 0) return false;
    if (!std::isfinite(s.lead_s) || !std::isfinite(s.show_s) || !std::isfinite(s.gap_s)) return false;
    size_t at = kHeader;
    for (uint32_t i = 0; i < count; ++i) {
        if (at + 8 > n) return false;
        Cue c;
        c.at_m     = F32(b + at);
        c.kind     = b[at + 4];
        c.priority = b[at + 5];
        uint16_t len;
        std::memcpy(&len, b + at + 6, 2);
        at += 8;
        if (len > kMaxText || at + len > n) return false;
        if (!std::isfinite(c.at_m) || c.at_m < 0 || c.at_m > s.track_len) return false;
        c.text.assign(reinterpret_cast<const char*>(b + at), len);
        at += len;
        s.cues.push_back(std::move(c));
    }
    // Settings kept sane whatever the file says.
    s.lead_s      = std::clamp(s.lead_s, 0.0f, 5.0f);
    s.show_s      = std::clamp(s.show_s, 0.3f, 5.0f);
    s.gap_s       = std::clamp(s.gap_s, 0.0f, 30.0f);
    s.max_per_lap = std::clamp<uint32_t>(s.max_per_lap, 1, 16);
    std::stable_sort(s.cues.begin(), s.cues.end(), [](const Cue& x, const Cue& y) { return x.at_m < y.at_m; });
    out = std::move(s);
    return true;
}

// SPluginsBikeEvent_t (EventInit) and SPluginsBikeSession_t (RunInit) offsets, mxb_api.h.
constexpr size_t kEventBikeId      = 100;
constexpr size_t kEventTrackId     = 444;
constexpr size_t kEventTrackLength = 644;
constexpr size_t kEventType        = 648;
constexpr size_t kEventServer      = 652;
constexpr size_t kIdLen            = 100;
constexpr size_t kServerLen        = 64;

struct Event {
    std::string bike, track, server;
    float       track_len = 0;
    int         type      = 0;  // 1 testing, 2 race, 4 straight rhythm
};

/// A fixed-size string field in a raw game struct.
inline std::string Field(const uint8_t* b, size_t n, size_t at, size_t len) {
    if (!b || at >= n) return {};
    len = (std::min)(len, n - at);
    const char* p = reinterpret_cast<const char*>(b + at);
    return std::string(p, strnlen(p, len));
}

inline Event ReadEvent(const void* data, int size) {
    Event e;
    if (!data || size <= 0) return e;
    const auto* b = static_cast<const uint8_t*>(data);
    const size_t n = size_t(size);
    e.bike   = Field(b, n, kEventBikeId, kIdLen);
    e.track  = Field(b, n, kEventTrackId, kIdLen);
    e.server = Field(b, n, kEventServer, kServerLen);
    if (n >= kEventTrackLength + 4) e.track_len = F32(b + kEventTrackLength);
    if (n >= kEventType + 4) e.type = int(U32(b + kEventType));
    return e;
}

inline int ReadSession(const void* data, int size) {
    if (!data || size < 4) return 0;
    return int(U32(static_cast<const uint8_t*>(data)));
}

/// Cues only in practice: a testing event, or the practice session of a race event. Never in
/// qualifying or a race.
inline bool Practice(int event_type, int session) {
    return event_type == 1 || (event_type == 2 && session == 1);
}

/// An id made safe for a file name, the same way the app makes it.
inline std::string SafeName(const std::string& id) {
    std::string out;
    for (char ch : id) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                        ch == '_' || ch == '-' || ch == '.';
        out += ok ? ch : '_';
    }
    return out;
}

/// Cue files to try, best first: this bike on this track, then the track for any bike.
inline std::vector<std::string> SheetNames(const Event& e) {
    if (e.track.empty()) return {};
    std::vector<std::string> out;
    if (!e.bike.empty()) out.push_back(SafeName(e.track) + "." + SafeName(e.bike) + ".cue");
    out.push_back(SafeName(e.track) + ".cue");
    return out;
}

/// A sheet belongs to a track only if it was made for one of the same length.
inline bool Fits(const Sheet& s, const Event& e) {
    return e.track_len > 0 && std::fabs(s.track_len - e.track_len) <= 1.0f;
}

/// Plays a sheet during laps: which cue shows, and when.
class Player {
public:
    void load(Sheet s) {
        sheet_ = std::move(s);
        fired_.assign(sheet_.cues.size(), false);
        reset_lap();
        showing_ = -1;
        any_ = false;
        last_pos_ = -1;
    }
    void clear() { load(Sheet{}); }

    void set_practice(bool on) {
        practice_ = on;
        if (!on) showing_ = -1;
    }
    bool active() const { return practice_ && !sheet_.cues.empty(); }
    const Sheet& sheet() const { return sheet_; }

    /// One telemetry sample: lap position 0..1, speed m/s, track time s.
    void on_sample(float pos, float speed, float t, bool crashed) {
        if (!active()) return;
        // The position wraps back to 0 at the line: a new lap.
        if (last_pos_ >= 0 && pos + 0.5f < last_pos_) reset_lap();
        last_pos_ = pos;
        if (showing_ >= 0 && t - shown_at_ >= sheet_.show_s) showing_ = -1;
        if (crashed) {
            showing_ = -1;
            return;
        }
        if (fired_count_ >= sheet_.max_per_lap || (any_ && t - last_fire_ < sheet_.gap_s)) return;
        const float m    = pos * sheet_.track_len;
        const float lead = (std::max)(0.0f, speed) * sheet_.lead_s;
        int best = -1;
        for (size_t i = 0; i < sheet_.cues.size(); ++i) {
            const Cue& c = sheet_.cues[i];
            // Due from the lead distance before the spot to just past it. Further past, it's
            // missed for this lap: late would be worse than not at all.
            if (fired_[i] || m < c.at_m - lead || m > c.at_m + kLateM) continue;
            if (best < 0 || c.priority > sheet_.cues[size_t(best)].priority) best = int(i);
        }
        if (best < 0) return;
        // A more important cue coming due within the gap this one would open would be shut out
        // by it: hold, and let that one through.
        const float horizon = m + (std::max)(0.0f, speed) * sheet_.gap_s;
        for (size_t i = 0; i < sheet_.cues.size(); ++i) {
            const Cue& c = sheet_.cues[i];
            const float due = c.at_m - lead;
            if (!fired_[i] && c.priority > sheet_.cues[size_t(best)].priority && due > m && due <= horizon) return;
        }
        fired_[size_t(best)] = true;
        ++fired_count_;
        ++fires_;
        any_       = true;
        last_fire_ = t;
        showing_   = best;
        shown_at_  = t;
    }

    /// A lap completed: every cue can show again.
    void on_lap() { reset_lap(); }

    /// Counts every cue fired: when it changes, the one showing() has just come up.
    uint32_t fires() const { return fires_; }

    /// The cue to show now, or null.
    const Cue* showing() const {
        return active() && showing_ >= 0 ? &sheet_.cues[size_t(showing_)] : nullptr;
    }

private:
    static constexpr float kLateM = 3.0f;

    void reset_lap() {
        std::fill(fired_.begin(), fired_.end(), false);
        fired_count_ = 0;
    }

    Sheet             sheet_;
    std::vector<bool> fired_;
    bool              practice_    = false;
    bool              any_         = false;
    uint32_t          fired_count_ = 0;
    uint32_t          fires_       = 0;
    float             last_pos_    = -1;
    float             last_fire_   = 0;
    float             shown_at_    = 0;
    int               showing_     = -1;
};

}  // namespace coachcue
