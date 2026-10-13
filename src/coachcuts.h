#pragma once
// Cut markers: which piece of a track's centreline a rider gets cut flags on.
//
// A track's centreline is the list of straights and arcs the game hands TrackCenterline
// (SPluginsTrackSegment_t), the same records the track's .trh carries. Track makers lay it
// out marker by marker, so marker N here is the start of segment N, numbered from 0 in the
// order the game lists them. The game follows a rider along that line and calls it a cut when
// the rider's place on it moves on further than the ground they covered explains.
//
// Three sources, best first:
//   * GAME: RaceCommunication with m_iCommunication 2 (penalty) and m_iOffence 2 (cutting).
//     Race events only. Exact rider and lap; the position is where the rider was when it came.
//   * LAP:  RunLap's m_iInvalid on a lap with no penalty to explain it (testing, practice).
//     The spot is the biggest skip the watch saw on that lap.
//   * WATCH: our own reading of RunTelemetry's lap position against the ground ridden: progress
//     along the line minus distance travelled, over a few seconds. Live, so the HUD can flash
//     the marker the moment it happens; it is confirmed or dropped when the lap ends.
//
// Pure C++, no Win32: pinned by tests/coachcuts_test.cpp.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace coachcuts {

/// Where a marker is: its number, world x/z, and metres past the start line.
struct Marker {
    int   id = 0;
    float x = 0, z = 0, at_m = 0;
};

/// One cut, from any source.
enum Source { SRC_WATCH = 0, SRC_LAP = 1, SRC_GAME = 2 };
inline const char* SourceName(int s) { return s == SRC_GAME ? "game" : s == SRC_LAP ? "lap" : "watch"; }

struct Cut {
    int   source    = SRC_WATCH;
    int   lap       = -1;     // the game's lap index, -1 when unknown
    float time      = 0;      // track time, s
    float pos       = 0;      // lap position 0..1 where it was seen
    float x = 0, z = 0;       // world x/z
    int   from_id   = -1;     // the marker the rider left the line at
    int   to_id     = -1;     // the marker they came back at
    float gain_m    = 0;      // metres of line skipped beyond the ground ridden
    bool  confirmed = false;  // the game said so (penalty or an invalid lap)
    int   penalty_ms = 0;
};

// ---------------------------------------------------------------------------------------
// Which marker a lap position is on

/// The marker a point `m` metres past the line is on: the last marker at or before it, in lap
/// order, so the piece of line before marker 0 belongs to the last marker. -1 with none.
inline int MarkerAt(const std::vector<Marker>& ms, float m) {
    if (ms.empty() || !std::isfinite(m)) return -1;
    // `ms` is in centreline order; the start line can sit anywhere along it, so find the one
    // with the largest at_m that is still <= m, else the one with the largest at_m overall.
    int   best = -1, last = -1;
    float best_m = -1e30f, last_m = -1e30f;
    for (const Marker& k : ms) {
        if (k.at_m <= m && k.at_m > best_m) best = k.id, best_m = k.at_m;
        if (k.at_m > last_m) last = k.id, last_m = k.at_m;
    }
    return best >= 0 ? best : last;
}

// ---------------------------------------------------------------------------------------
// The watch

/// Metres of line skipped beyond the ground ridden before a skip counts. A tight hairpin's
/// inside line gains about 15 m on the centreline over its length, so this sits above that.
constexpr float kMinGainM = 25.0f;
/// How far back the gain is measured.
constexpr float kWindowS = 4.0f;
/// After a cut, this long before the next can fire: one skip is one cut.
constexpr float kCooldownS = 5.0f;
/// Samples further apart than this are a reset or a respawn, not riding.
constexpr float kTeleportM = 60.0f;
/// Samples further apart in time are a gap (the pits, a pause).
constexpr float kGapS = 3.0f;

class Watch {
public:
    void reset() {
        ring_.clear();
        have_ = false;
        lap_best_ = Cut{};
        lap_has_best_ = false;
        suspects_.clear();
    }

    /// The lap ended: the suspects seen on it, and the biggest skip of it. Clears both.
    void end_lap(std::vector<Cut>& suspects, Cut& best, bool& has_best) {
        suspects.swap(suspects_);
        suspects_.clear();
        best = lap_best_;
        has_best = lap_has_best_;
        lap_best_ = Cut{};
        lap_has_best_ = false;
    }

    const std::vector<Cut>& pending() const { return suspects_; }
    /// The latest suspect, taken off the list (the game has just confirmed it). Call only with
    /// one pending.
    Cut take_last() {
        Cut c = suspects_.back();
        suspects_.pop_back();
        return c;
    }

    /// One telemetry sample. True, with `out`, when a skip has just crossed kMinGainM.
    bool sample(float t, float pos, float x, float z, bool crashed, float track_len,
                const std::vector<Marker>& ms, Cut& out) {
        if (!std::isfinite(t) || !std::isfinite(pos) || !std::isfinite(x) || !std::isfinite(z) ||
            !(track_len > 0)) return false;
        if (crashed) {
            // A crash puts the rider back on the line wherever the game likes: start again.
            ring_.clear();
            have_ = false;
            return false;
        }
        if (have_) {
            const float dx = x - last_x_, dz = z - last_z_;
            const float step = std::sqrt(dx * dx + dz * dz);
            if (t < last_t_ || t - last_t_ > kGapS || step > kTeleportM) {
                ring_.clear();
                have_ = false;
            } else {
                float d = pos - last_pos_;
                if (d < -0.5f) d += 1.0f;  // over the line, forwards
                if (d > 0.5f) d -= 1.0f;   // over the line, backwards
                prog_ += d * track_len;
                ground_ += step;
                const float jump = d * track_len - step;
                Sample s{t, prog_, ground_, pos, x, z, last_pos_, jump};
                ring_.push_back(s);
            }
        }
        if (!have_) {
            prog_ = ground_ = 0;
            ring_.push_back(Sample{t, 0, 0, pos, x, z, pos, 0});
        }
        have_ = true;
        last_t_ = t, last_pos_ = pos, last_x_ = x, last_z_ = z;
        while (!ring_.empty() && t - ring_.front().t > kWindowS) ring_.pop_front();
        if (ring_.size() < 2) return false;
        const Sample& a = ring_.front();
        const Sample& b = ring_.back();
        const float gain = (b.prog - a.prog) - (b.ground - a.ground);
        if (gain > lap_best_.gain_m || !lap_has_best_) {
            if (gain > 0) {
                lap_best_ = Describe(gain, t, track_len, ms);
                lap_has_best_ = true;
            }
        }
        if (gain < kMinGainM) return false;
        if (have_cut_ && t >= last_cut_t_ && t - last_cut_t_ < kCooldownS) return false;
        out = Describe(gain, t, track_len, ms);
        have_cut_ = true;
        last_cut_t_ = t;
        suspects_.push_back(out);
        // What is in the window is spent: the same skip must not fire again as it slides out.
        ring_.clear();
        ring_.push_back(b);
        return true;
    }

private:
    struct Sample {
        float t, prog, ground, pos, x, z, prev_pos, jump;
    };
    /// The cut the window holds: at the single biggest step, from the marker before it to the
    /// marker after it.
    Cut Describe(float gain, float t, float track_len, const std::vector<Marker>& ms) const {
        size_t k = ring_.size() - 1;
        float  top = -1e30f;
        for (size_t i = 1; i < ring_.size(); ++i)
            if (ring_[i].jump > top) top = ring_[i].jump, k = i;
        const Sample& s = ring_[k];
        Cut c;
        c.source  = SRC_WATCH;
        c.time    = t;
        c.pos     = s.pos;
        c.x       = s.x;
        c.z       = s.z;
        c.gain_m  = gain;
        c.from_id = MarkerAt(ms, ring_.front().pos * track_len);
        c.to_id   = MarkerAt(ms, ring_.back().pos * track_len);
        // The step itself is the better "from" when it is the skip: the marker just before it.
        if (top > 5.0f) c.from_id = MarkerAt(ms, s.prev_pos * track_len);
        return c;
    }

    std::deque<Sample> ring_;
    bool               have_ = false, have_cut_ = false, lap_has_best_ = false;
    float              last_t_ = 0, last_pos_ = 0, last_x_ = 0, last_z_ = 0;
    float              prog_ = 0, ground_ = 0, last_cut_t_ = 0;
    Cut                lap_best_;
    std::vector<Cut>   suspects_;
};

// ---------------------------------------------------------------------------------------
// The game's own word

/// SPluginsRaceCommunication_t (mxb_api.h): session, race number, communication (2 = penalty),
/// state, reason, offence (2 = cutting), lap, start, type, time (ms). True for a cut penalty.
inline bool CutPenalty(const void* data, int size, int& race_num, int& lap, int& time_ms) {
    if (!data || size < 40) return false;
    int32_t v[10];
    std::memcpy(v, data, sizeof(v));
    if (v[2] != 2 || v[5] != 2) return false;
    race_num = v[1];
    lap      = v[6];
    time_ms  = v[9];
    return true;
}

/// SPluginsBikeLap_t (mxb_api.h): lap number, invalid, lap time, best.
inline bool LapInvalid(const void* data, int size, int& lap, bool& invalid) {
    if (!data || size < 8) return false;
    int32_t v[2];
    std::memcpy(v, data, sizeof(v));
    lap     = v[0];
    invalid = v[1] != 0;
    return true;
}

// ---------------------------------------------------------------------------------------
// The tally the HUD and MXB Coach show

/// Cuts per marker this session, most first, ties by marker number.
class Tally {
public:
    void clear() { by_.clear(), total_ = 0; }
    void add(int id) {
        if (id < 0) return;
        ++by_[id];
        ++total_;
    }
    int total() const { return total_; }
    int count(int id) const {
        auto it = by_.find(id);
        return it == by_.end() ? 0 : it->second;
    }
    std::vector<std::pair<int, int>> top(size_t n) const {
        std::vector<std::pair<int, int>> v(by_.begin(), by_.end());
        std::sort(v.begin(), v.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        if (v.size() > n) v.resize(n);
        return v;
    }
    /// "#12 x3  #27 x1", for the HUD.
    std::string Line(size_t n) const {
        std::string s;
        for (const auto& kv : top(n)) {
            if (!s.empty()) s += "  ";
            s += "#" + std::to_string(kv.first) + " x" + std::to_string(kv.second);
        }
        return s;
    }

private:
    std::map<int, int> by_;
    int                total_ = 0;
};

// ---------------------------------------------------------------------------------------
// The files MXB Coach reads

/// A JSON string body: quotes, backslashes and control characters escaped.
inline std::string JsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += char(c);
        else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof(b), "\\u%04x", c);
            o += b;
        } else o += char(c);
    }
    return o;
}

/// One line of `<save>\mxbcoach\cuts\cuts.jsonl`.
/// `session` is the stamp of the event the cut came in (EventInit), so a reader can show one ride.
inline std::string CutLine(const Cut& c, const std::string& stamp, const std::string& session, const std::string& track,
                           const std::string& rider, bool self) {
    char b[1024];
    std::snprintf(b, sizeof(b),
                  "{\"at\":\"%s\",\"session\":\"%s\",\"track\":\"%s\",\"rider\":\"%s\",\"self\":%s,\"source\":\"%s\",\"confirmed\":%s,"
                  "\"lap\":%d,\"marker\":%d,\"to\":%d,\"x\":%.1f,\"z\":%.1f,\"pos\":%.4f,\"gain_m\":%.1f,\"penalty_ms\":%d}",
                  JsonEscape(stamp).c_str(), JsonEscape(session).c_str(), JsonEscape(track).c_str(), JsonEscape(rider).c_str(), self ? "true" : "false",
                  SourceName(c.source), c.confirmed ? "true" : "false", c.lap, c.from_id, c.to_id, double(c.x),
                  double(c.z), double(c.pos), double(c.gain_m), c.penalty_ms);
    return b;
}

/// `<save>\mxbcoach\cuts\<track>.markers.json`: every marker, for MXB Coach's map.
inline std::string MarkersJson(const std::string& track, float length_m, const std::vector<Marker>& ms) {
    std::string o = "{\"track\":\"" + JsonEscape(track) + "\",\"length_m\":" + std::to_string(length_m) + ",\"markers\":[";
    char b[96];
    for (size_t i = 0; i < ms.size(); ++i) {
        std::snprintf(b, sizeof(b), "%s{\"id\":%d,\"x\":%.2f,\"z\":%.2f,\"at_m\":%.1f}", i ? "," : "", ms[i].id,
                      double(ms[i].x), double(ms[i].z), double(ms[i].at_m));
        o += b;
    }
    return o + "]}";
}

/// A track name safe as a file name: anything but letters, digits, space, '-', '_' and '.'
/// becomes '_'.
inline std::string FileSafe(const std::string& s) {
    std::string o;
    for (unsigned char c : s)
        o += (std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.') ? char(c) : '_';
    return o.empty() ? std::string("track") : o;
}

}  // namespace coachcuts
