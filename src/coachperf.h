// MXB Coach recorder: what the plugin costs the game's frame, and the rules that keep that cost
// near zero (v0.49.4).
//
// Sean, 0.48.2: with MXBMRP3 loaded beside the recorder the game fell to ~10 FPS with freezes of
// about two seconds; either plugin alone ran perfectly. Two releases cut what we thought was the
// cost and neither run said where the time went. This header is the part of the answer that can
// be decided without the game:
//
//   * Acc / FrameHist / Spike: per-hook and per-callback time (sum, max, count) and the frame time
//     seen at the swap (p50/p99/max, every frame over 50 ms with what we were doing in it). Plain
//     counters written by one thread each, relaxed atomics so a reader on another thread never
//     tears a value and nothing ever waits: no locks, no allocation, no I/O.
//   * HookGate: the GL hooks are passthroughs unless this is a riding frame with the line on AND
//     the game is in its 3D world pass (a perspective projection). The HUD pass (ortho), where the
//     engine draws MXBMRP3's thousands of primitives, costs one predictable branch per call.
//   * SearchBackoff: the camera search over every modelview load is bounded in time. Not found
//     within a few seconds, it rests and retries briefly every few seconds, instead of running on
//     every load for minutes (the 0.48.2 log: "camera=searching" for ten minutes straight).
//   * Safe mode (hud.ini safe_mode=auto|1|0): auto turns it on when MXBMRP3 is loaded.
//
// Pure C++: tests/coachperf_test.cpp pins it, tests/glhook_bench.cpp measures it.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coachperf {

// ---------------------------------------------------------------------------------------
// Where the time goes

/// Everything of ours that runs inside the game's frame or its callbacks, one slot each.
enum Slot {
    HK_PROJ,       // projection tracking: glMatrixMode/LoadMatrix/Frustum/Ortho/Mult on GL_PROJECTION
    HK_MODELVIEW,  // a modelview load offered to the camera search or follow
    HK_UNIFORM,    // a shader matrix upload recorded (not in safe mode)
    SWAP_WORK,     // the swap hook's own work: camera pick, ribbon, marks (under g_mu, try-lock)
    SWAP_DRAW,     // our ribbon and marks drawn into the game's frame (GL calls)
    SWAP_DEPTH,    // depth reads (glReadPixels): rare and budgeted, off in safe mode
    SWAP_PRESENT,  // the game's own SwapBuffers, timed so a driver/GPU stall shows as not ours
    CB_DRAW,       // the plugin API's Draw callback
    CB_TELEMETRY,  // RunTelemetry (the sampler's slice included)
    CB_OTHER,      // every other plugin callback
    SLOT_COUNT
};
inline const char* SlotName(int s) {
    static const char* const k[SLOT_COUNT] = {"hook.proj", "hook.modelview", "hook.uniform", "swap.work",
                                              "swap.draw", "swap.depth",     "swap.present", "cb.draw",
                                              "cb.telemetry", "cb.other"};
    return s >= 0 && s < SLOT_COUNT ? k[s] : "?";
}

/// One slot's running totals. A single thread writes it; any thread may read it. Relaxed atomics
/// compile to plain loads and stores on x64: no lock prefix, no fence, no waiting.
struct Acc {
    std::atomic<uint64_t> n{0}, sum{0}, max{0}, pass{0};
    void add(uint64_t ticks) {
        n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        sum.store(sum.load(std::memory_order_relaxed) + ticks, std::memory_order_relaxed);
        if (ticks > max.load(std::memory_order_relaxed)) max.store(ticks, std::memory_order_relaxed);
    }
    /// A call that took the passthrough: counted, not timed (timing it would cost more than it).
    void skip() { pass.store(pass.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }
};

/// A slot's totals at one moment, and the difference from the last report.
struct AccSnap {
    uint64_t n = 0, sum = 0, max = 0, pass = 0;
};
inline AccSnap Take(Acc& a, AccSnap& prev) {
    AccSnap now;
    now.n    = a.n.load(std::memory_order_relaxed);
    now.sum  = a.sum.load(std::memory_order_relaxed);
    now.pass = a.pass.load(std::memory_order_relaxed);
    // The max is per window: read and cleared. A value written between the two is lost to this
    // window at worst, never torn.
    now.max = a.max.exchange(0, std::memory_order_relaxed);
    AccSnap d;
    d.n    = now.n - prev.n;
    d.sum  = now.sum - prev.sum;
    d.pass = now.pass - prev.pass;
    d.max  = now.max;
    prev   = now;
    return d;
}

// ---------------------------------------------------------------------------------------
// Frame time

/// Frame times in microseconds: 0.1 ms buckets to 50 ms, 10 ms buckets to 2 s, then one more.
/// Percentiles come from the buckets (to their upper edge); the max is exact.
class FrameHist {
public:
    static constexpr int kFine = 500, kCoarse = 195, kBuckets = kFine + kCoarse + 1;
    void add(uint64_t us) {
        ++n_;
        sum_ += us;
        if (us > max_) max_ = us;
        if (us >= 50000) ++over50_;
        ++b_[Bucket(us)];
    }
    void reset() { *this = FrameHist{}; }
    uint64_t count() const { return n_; }
    uint64_t max_us() const { return max_; }
    uint64_t over50() const { return over50_; }
    double mean_us() const { return n_ ? double(sum_) / double(n_) : 0.0; }
    /// The frame time `q` (0..1) of the frames fall under, in microseconds.
    uint64_t percentile_us(double q) const {
        if (!n_) return 0;
        const uint64_t want = uint64_t(std::ceil(q * double(n_)));
        uint64_t       seen = 0;
        for (int i = 0; i < kBuckets; ++i) {
            seen += b_[i];
            if (seen >= want && b_[i]) return (std::min)(Upper(i), max_);
        }
        return max_;
    }
    static int Bucket(uint64_t us) {
        if (us < 50000) return int(us / 100);
        if (us < 2000000) return kFine + int((us - 50000) / 10000);
        return kBuckets - 1;
    }
    static uint64_t Upper(int i) {
        if (i < kFine) return uint64_t(i + 1) * 100;
        if (i < kBuckets - 1) return 50000 + uint64_t(i - kFine + 1) * 10000;
        return UINT64_MAX;
    }

private:
    uint64_t n_ = 0, sum_ = 0, max_ = 0, over50_ = 0;
    uint32_t b_[kBuckets] = {};
};

/// One frame over 50 ms and what we were doing in it.
struct Spike {
    uint64_t at_ms     = 0;  // GetTickCount64 at the swap that ended it
    uint64_t frame_us  = 0;
    uint64_t ours_us   = 0;  // our time on the render thread in that frame (hooks + swap work + draw)
    int      top       = -1; // the slot that took most of it
    uint64_t top_us    = 0;
    uint64_t present_us = 0; // the previous SwapBuffers call itself
    uint32_t loads     = 0;  // modelview loads the game made in the frame
    uint32_t offered   = 0;  // of those, offered to the camera search
    uint32_t hud_calls = 0;  // matrix calls made in the HUD (ortho) pass, passed straight through
    uint64_t draw_cb_us = 0; // the longest Draw callback in the frame
    uint64_t tele_cb_us = 0; // the longest RunTelemetry since the last frame (any thread)
    uint32_t lock_miss = 0;  // try-locks on the plugin's state that failed in the frame
    int      source    = 0;  // the camera source: 0 searching, 1 uniform, 2 modelview, 3 fallback
    bool     live      = false;  // a riding frame with the line on
    bool     resting   = false;  // the camera search was resting
    bool     depth     = false;  // a depth read ran in the frame
};

/// The last kN spikes, written by the render thread, read by it at report time.
class SpikeRing {
public:
    static constexpr int kN = 8;
    void add(const Spike& s) {
        r_[w_ % kN] = s;
        ++w_;
    }
    /// Spikes since `since` (a running count), oldest first, at most kN.
    template <class F>
    void each_since(uint64_t since, F f) const {
        const uint64_t from = w_ > uint64_t(kN) && since < w_ - kN ? w_ - kN : since;
        for (uint64_t i = from; i < w_; ++i) f(r_[i % kN]);
    }
    uint64_t written() const { return w_; }

private:
    Spike    r_[kN];
    uint64_t w_ = 0;
};

// ---------------------------------------------------------------------------------------
// When the GL hooks do anything

/// A projection that maps eye depth straight to clip depth (no perspective divide): glOrtho's, or
/// an identity. The HUD and other 2D passes are drawn through one.
inline bool IsOrtho(const float* m) {
    return std::fabs(m[3]) < 1e-6f && std::fabs(m[7]) < 1e-6f && std::fabs(m[11]) < 1e-6f && std::fabs(m[15]) > 1e-6f;
}
/// One with a perspective divide (w from -z): a 3D camera's.
inline bool IsPerspective(const float* m) { return std::fabs(m[11]) > 1e-6f && std::fabs(m[15]) < 1e-3f; }

/// Whether a GL hook does more than forward the call. The game's frame is a 3D world pass through
/// a perspective projection, then a 2D pass through an ortho one, where it draws its own HUD and
/// every plugin's (MXBMRP3's thousands of primitives among them). The camera is only ever in the
/// first, and only wanted while riding with the line on; everything else is a passthrough.
struct HookGate {
    bool live   = false;  // set at each swap: the next frame is a riding frame with the line on
    bool search = false;  // and its modelview loads are wanted (a camera followed, or a search not resting)
    bool world  = true;   // in the 3D world pass: no ortho projection since the last perspective one
    /// Whether a modelview load (or a shader upload) is looked at at all.
    bool offer() const { return live && search && world; }
    void frame(bool live_next, bool search_next) {
        live   = live_next;
        search = live_next && search_next;
        world  = true;  // a frame starts with the world, until an ortho projection says otherwise
    }
    void ortho() { world = false; }
    void frustum() { world = true; }
    /// A projection matrix loaded (or multiplied in): perspective starts the world pass, ortho or
    /// identity starts a 2D one. Anything else leaves it as it was.
    void projection(const float* m) {
        if (IsPerspective(m)) world = true;
        else if (IsOrtho(m)) world = false;
    }
};

/// The camera search, bounded. It runs on every modelview load of every riding frame while no
/// camera is held; when none turns up within kFirstMs it rests kRestMs, then looks again for
/// kRetryMs, and so on. A camera found (or the rider stopping) starts it over.
class SearchBackoff {
public:
    static constexpr uint64_t kFirstMs = 3000, kRetryMs = 500, kRestMs = 3000;
    // Safe mode: shorter looks, longer rests.
    static constexpr uint64_t kSafeFirstMs = 1000, kSafeRetryMs = 250, kSafeRestMs = 5000;
    void set_safe(bool safe) {
        first_ = safe ? kSafeFirstMs : kFirstMs;
        retry_ = safe ? kSafeRetryMs : kRetryMs;
        rest_  = safe ? kSafeRestMs : kRestMs;
    }
    void reset() {
        since_ = 0, rest_until_ = 0, retried_ = false;
    }
    /// Once a frame, at the swap: whether loads should be searched in the next frame. `want`: a
    /// riding frame with the line on; `found`: a camera from GL was held this frame.
    bool frame(uint64_t now, bool want, bool found) {
        if (!want || found) {
            reset();
            return want;
        }
        if (rest_until_) {
            if (now < rest_until_) return false;
            rest_until_ = 0;
            since_      = now;
            return true;
        }
        if (!since_) since_ = now;
        if (now - since_ < (retried_ ? retry_ : first_)) return true;
        since_      = 0;
        rest_until_ = now + rest_;
        retried_    = true;
        ++rests_;
        return false;
    }
    bool     resting() const { return rest_until_ != 0; }
    uint64_t rests() const { return rests_; }

private:
    uint64_t since_ = 0, rest_until_ = 0, rests_ = 0;
    uint64_t first_ = kFirstMs, retry_ = kRetryMs, rest_ = kRestMs;
    bool     retried_ = false;
};

// ---------------------------------------------------------------------------------------
// Safe mode

enum SafeSetting { SAFE_OFF = 0, SAFE_ON = 1, SAFE_AUTO = 2 };
/// hud.ini safe_mode: 1 / on, 0 / off, auto (the default, and anything unrecognised).
inline SafeSetting ParseSafe(const std::string& v) {
    if (v == "1" || v == "on" || v == "yes" || v == "true") return SAFE_ON;
    if (v == "0" || v == "off" || v == "no" || v == "false") return SAFE_OFF;
    return SAFE_AUTO;
}
inline bool SafeOn(SafeSetting s, bool mxbmrp3_loaded) { return s == SAFE_ON || (s == SAFE_AUTO && mxbmrp3_loaded); }
/// What safe mode leaves out, for the log: it says so whenever it changes.
inline const char* SafeDrops() {
    return "depth reads (glReadPixels and the glGet calls around them: the line is depth-tested the way the game "
           "tests, without the snap correction), the shader-uniform hooks (14 driver entry points; glUseProgram "
           "stays), the 30 s camera diagnostics, and a shorter camera search (1 s, then 250 ms every 5 s)";
}

// ---------------------------------------------------------------------------------------
// The report

inline double Ms(uint64_t us) { return double(us) / 1000.0; }

/// "hook.modelview n=1200 pass=40000 sum=0.42ms max=3us" for one slot over a window.
inline std::string SlotText(int slot, const AccSnap& d, double us_per_tick) {
    char b[200];
    std::snprintf(b, sizeof(b), "%s n=%llu pass=%llu sum=%.2fms max=%.0fus", SlotName(slot),
                  (unsigned long long)d.n, (unsigned long long)d.pass, double(d.sum) * us_per_tick / 1000.0,
                  double(d.max) * us_per_tick);
    return b;
}

/// The frame line: how many frames, how long they took, how many were over 50 ms.
inline std::string FrameText(const FrameHist& h, double window_s) {
    char b[220];
    std::snprintf(b, sizeof(b), "frames=%llu fps=%.1f frame p50=%.1fms p99=%.1fms max=%.1fms mean=%.1fms over50ms=%llu",
                  (unsigned long long)h.count(), window_s > 0 ? double(h.count()) / window_s : 0.0,
                  Ms(h.percentile_us(0.50)), Ms(h.percentile_us(0.99)), Ms(h.max_us()), h.mean_us() / 1000.0,
                  (unsigned long long)h.over50());
    return b;
}

inline std::string SpikeText(const Spike& s) {
    char b[400];
    std::snprintf(b, sizeof(b),
                  "%.1f ms frame: ours %.2f ms (top %s %.2f ms), present %.2f ms, Draw cb %.2f ms, telemetry cb %.2f ms, "
                  "loads %u offered %u hud-pass calls %u, lock misses %u, camera=%d live=%d resting=%d depth_read=%d",
                  Ms(s.frame_us), Ms(s.ours_us), s.top >= 0 ? SlotName(s.top) : "none", Ms(s.top_us), Ms(s.present_us),
                  Ms(s.draw_cb_us), Ms(s.tele_cb_us), s.loads, s.offered, s.hud_calls, s.lock_miss, s.source,
                  s.live ? 1 : 0, s.resting ? 1 : 0, s.depth ? 1 : 0);
    return b;
}

}  // namespace coachperf
