#pragma once
// ============================================================================
//  NaN watch - where a position stops being a number.
//
//  Two crashes in the reports come down to a position that is already NaN by the
//  time the game uses it: the terrain sample at mxbikes.exe+0x1F1923 (guarded since
//  v0.36) and the index at +0x1A29AE that faulted 44 times on OneTwoSix right after
//  that guard had refused 41 NaN height queries. The guard proves the NaN exists. It
//  cannot say whose position went bad, when, or where they were a moment before.
//
//  The plugin API already hands us every rider's position (RaceTrackPosition) and our
//  own (RunTelemetry) several times a second. So this watches them, and says so the
//  moment one goes from a number to not a number: who, where they last were, and how
//  long ago that was. It also says when one comes back, because a position that
//  recovers on its own is a different bug from one that stays NaN until the crash.
//
//  Header-only and free of Windows so the test can drive it. The caller logs.
// ============================================================================
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace frostmod::nanwatch {

/// `who` for our own bike, from telemetry. Race numbers are never negative.
constexpr int kMe = -1;
/// 64 riders is the most RaceTrackPosition carries (frostmod.cpp caps it there), plus us.
constexpr int kSlots = 65;
/// Transitions logged per run. A rider stuck flickering must not fill the log.
constexpr unsigned kMaxReports = 20;

inline bool Finite(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

enum class Edge { None, WentBad, Recovered };

struct Slot {
    int who = 0;
    bool used = false;
    bool bad = false;
    bool haveGood = false;
    float gx = 0, gy = 0, gz = 0;         // the last position that was a number
    unsigned long long goodAt = 0;        // when it was seen
    unsigned long long badAt = 0;         // when it stopped being one
};

class Watch {
public:
    /// One sighting of `who` at (x, y, z). Returns the transition it made, if any.
    Edge See(int who, float x, float y, float z, unsigned long long nowMs) {
        Slot* s = Find(who);
        if (!s) return Edge::None;   // table full: the 66th rider is not watched
        const bool finite = Finite(x, y, z);
        if (finite) {
            s->gx = x; s->gy = y; s->gz = z;
            s->haveGood = true;
            s->goodAt = nowMs;
            if (s->bad) { s->bad = false; return Edge::Recovered; }
            return Edge::None;
        }
        if (s->bad) return Edge::None;
        s->bad = true;
        s->badAt = nowMs;
        return Edge::WentBad;
    }

    /// Whether this transition still fits in the log budget. Counts it if so.
    bool Budget() { return reports_ < kMaxReports ? (++reports_, true) : false; }

    /// One line describing `who`'s last transition, for the log and the crash trail.
    int Describe(int who, Edge e, float x, float y, float z, unsigned long long nowMs,
                 char* out, size_t n) const {
        const Slot* s = Peek(who);
        char name[24];
        if (who == kMe) snprintf(name, sizeof(name), "our bike");
        else snprintf(name, sizeof(name), "rider #%d", who);
        if (!s) return snprintf(out, n, "%s: not watched", name);
        if (e == Edge::Recovered)
            return snprintf(out, n, "%s is a position again at (%.2f, %.2f, %.2f), after %llums "
                            "as not one", name, (double)x, (double)y, (double)z,
                            nowMs - s->badAt);
        if (!s->haveGood)
            return snprintf(out, n, "%s's position is not a number (%f, %f, %f), and it never "
                            "was one this run", name, (double)x, (double)y, (double)z);
        return snprintf(out, n, "%s's position stopped being a number: (%f, %f, %f). Last "
                        "real one (%.2f, %.2f, %.2f), %llums before", name, (double)x,
                        (double)y, (double)z, (double)s->gx, (double)s->gy, (double)s->gz,
                        nowMs - s->goodAt);
    }

    /// The crash trail's version: short enough for its line (crashreport.h), with the part
    /// that matters most - who, and how long ago they were last a position - first.
    int Brief(int who, Edge e, unsigned long long nowMs, char* out, size_t n) const {
        const Slot* s = Peek(who);
        char name[16];
        if (who == kMe) snprintf(name, sizeof(name), "our bike");
        else snprintf(name, sizeof(name), "#%d", who);
        if (!s) return snprintf(out, n, "pos %s not watched", name);
        if (e == Edge::Recovered)
            return snprintf(out, n, "pos %s a number again after %llums", name, nowMs - s->badAt);
        if (!s->haveGood) return snprintf(out, n, "pos %s NaN, never a number this run", name);
        return snprintf(out, n, "pos %s NaN, last real %llums before at (%.1f,%.1f,%.1f)", name,
                        nowMs - s->goodAt, (double)s->gx, (double)s->gy, (double)s->gz);
    }

    /// A rider left: free the slot, so the table never fills with the departed and a
    /// newcomer on the same number does not inherit their history.
    void Forget(int who) {
        for (Slot& s : slots_)
            if (s.used && s.who == who) s = Slot{};
    }

    /// Forget everyone: a new session reuses race numbers.
    void Reset() {
        for (Slot& s : slots_) s = Slot{};
    }

private:
    Slot* Find(int who) {
        Slot* free = nullptr;
        for (Slot& s : slots_) {
            if (s.used && s.who == who) return &s;
            if (!s.used && !free) free = &s;
        }
        if (free) { free->used = true; free->who = who; }
        return free;
    }
    const Slot* Peek(int who) const {
        for (const Slot& s : slots_)
            if (s.used && s.who == who) return &s;
        return nullptr;
    }

    Slot slots_[kSlots];
    unsigned reports_ = 0;
};

// ---- the float registers at a crash ----------------------------------------
//
// A NaN that reaches an index goes through `cvttss2si` or `cvttsd2si` first, and what
// comes out (0x80000000) is all the integer registers can show. The float it came from
// is still in an XMM register when the fault lands. So a report names which ones held
// something that is not a number, read both ways, since the game uses both widths.

/// The low 32 bits of an XMM register, as the float `cvttss2si` would read.
inline float LowFloat(uint64_t lo) {
    const uint32_t bits = (uint32_t)lo;
    float f;
    static_assert(sizeof(f) == sizeof(bits), "float is 32 bits");
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
/// The low 64 bits, as the double `cvttsd2si` would read.
inline double LowDouble(uint64_t lo) {
    double d;
    static_assert(sizeof(d) == sizeof(lo), "double is 64 bits");
    std::memcpy(&d, &lo, sizeof(d));
    return d;
}

/// "xmm3 (float NaN) xmm8 (double inf)" for every register whose low lane is not a
/// number, or "none". Returns how many.
inline int NonFiniteXmm(const uint64_t lows[16], char* out, size_t n) {
    int count = 0;
    size_t at = 0;
    out[0] = 0;
    for (int i = 0; i < 16; ++i) {
        const float f = LowFloat(lows[i]);
        const double d = LowDouble(lows[i]);
        const char* how = !std::isfinite(f) ? (std::isnan(f) ? "float NaN" : "float inf")
                        : !std::isfinite(d) ? (std::isnan(d) ? "double NaN" : "double inf")
                        : nullptr;
        if (!how) continue;
        ++count;
        if (at < n) {
            const int w = snprintf(out + at, n - at, "%sxmm%d (%s)", at ? " " : "", i, how);
            if (w > 0) at += (size_t)w;
        }
    }
    if (!count) snprintf(out, n, "none");
    return count;
}

}  // namespace frostmod::nanwatch
