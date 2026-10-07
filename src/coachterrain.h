// The game's own ground, for the line on the track (MXB Coach).
//
// The line sits on the track's ground grid. Coach makes that grid (`<track>.ground`) from the
// track's .trh, which it can only do for a track it can read: not a locked one, not a secured
// (.mxbsecure) one, where the line fell back on the lap's own heights and a clamped depth snap.
// But the game has the same heightfield loaded whatever the track is, and a function that
// answers "how high is the ground at (x, z)": the sampler FrostMod already guards (0x1F1720,
// see offsets.h). This asks it, a few thousand points a telemetry tick, into a grid of our own
// that the line then uses exactly as it would a `.ground`.
//
// What it does not do, by rule: write the ground anywhere. The grid lives in this process for
// the event and is dropped at its end; nothing of it is logged, saved, or sent to the app. It
// writes nothing into the game either: the sampler is a pure read (offsets.h), called on the
// game's own thread, from a plugin callback in the middle of a run - the same thread, between
// the same frames, as the physics that asks it the same question every step.
//
// And only on the build the addresses were read from: both the sampler and the accessor the
// slots are decoded out of must match their signatures at their RVAs exactly, and the decode
// must land where it did. Anything else and this stays off and the line works as it did.
//
// Pure C++ above the `game` namespace (tests/coachterrain_test.cpp); the in-process part is
// coachterrain.cpp.
#pragma once

#include "coachline.h"
#include "offsets.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coachterrain {

/// Bytes at `p` against a signature and its mask ('x' must match, anything else is a wildcard),
/// from byte `from` of the signature on.
inline bool MatchAt(const uint8_t* p, const char* sig, const char* mask, size_t from = 0) {
    for (size_t i = from; mask[i]; ++i)
        if (mask[i] == 'x' && p[i] != uint8_t(sig[i])) return false;
    return true;
}

enum class Sampler { NONE, PLAIN, BEHIND_HOOK };

/// The sampler's prologue as beta21e has it, or with its first five bytes a `jmp` (FrostMod's
/// guard, which leaves every finite query alone) and the rest as it was. The hook covers exactly
/// `mov [rsp+0x18], r8`, five bytes, so everything after it must still match.
inline Sampler CheckSampler(const uint8_t* fn) {
    if (MatchAt(fn, mxb::SIG_TERRAIN_SAMPLE, mxb::SIG_TERRAIN_SAMPLE_MASK)) return Sampler::PLAIN;
    if (fn[0] == 0xE9 && MatchAt(fn, mxb::SIG_TERRAIN_SAMPLE, mxb::SIG_TERRAIN_SAMPLE_MASK, 5))
        return Sampler::BEHIND_HOOK;
    return Sampler::NONE;
}

/// The track slots, out of the accessor's own `lea rax, [rip+disp]`.
inline const uint8_t* DecodeSlots(const uint8_t* accessor) {
    int32_t disp;
    std::memcpy(&disp, accessor + mxb::TRACK_SLOT_LEA_DISP_OFF, 4);
    return accessor + mxb::TRACK_SLOT_LEA_END_OFF + disp;
}

/// A slot's heightfield, as the sampler reads it.
struct Header {
    int32_t     cols = 0, rows = 0;
    const void* heights = nullptr;
    float       size_x = 0, size_z = 0, origin_x = 0, origin_z = 0;
    bool        operator==(const Header& o) const {
        return cols == o.cols && rows == o.rows && heights == o.heights && size_x == o.size_x &&
               size_z == o.size_z && origin_x == o.origin_x && origin_z == o.origin_z;
    }
    bool operator!=(const Header& o) const { return !(*this == o); }
};

inline Header ReadHeader(const uint8_t* slot) {
    Header h;
    std::memcpy(&h.cols, slot + mxb::OFF_TERRAIN_WIDTH, 4);
    std::memcpy(&h.rows, slot + mxb::OFF_TERRAIN_HEIGHT, 4);
    std::memcpy(&h.heights, slot + mxb::OFF_TERRAIN_GRID, sizeof(void*));
    std::memcpy(&h.size_x, slot + mxb::OFF_TERRAIN_SIZE_X, 4);
    std::memcpy(&h.size_z, slot + mxb::OFF_TERRAIN_SIZE_Z, 4);
    std::memcpy(&h.origin_x, slot + mxb::OFF_TERRAIN_ORIGIN_X, 4);
    std::memcpy(&h.origin_z, slot + mxb::OFF_TERRAIN_ORIGIN_Z, 4);
    return h;
}

/// A heightfield worth asking about: loaded, a sane size, somewhere sane. An empty slot is all
/// zeros; a stadium track is a few hundred metres, a national one a couple of kilometres.
inline bool Plausible(const Header& h) {
    const auto fin = [](float v) { return std::isfinite(v); };
    return h.heights && h.cols >= 2 && h.rows >= 2 && h.cols <= 65536 && h.rows <= 65536 && fin(h.size_x) &&
           fin(h.size_z) && h.size_x >= 10 && h.size_z >= 10 && h.size_x <= 20000 && h.size_z <= 20000 &&
           fin(h.origin_x) && fin(h.origin_z) && std::fabs(h.origin_x) < 1e6f && std::fabs(h.origin_z) < 1e6f;
}

/// How finely to sample. Half a metre: twice what a `.ground` has, fine enough for the tops of
/// bumps and lips the line's lift is there for, and a grid a 550 m track fills in a few seconds
/// of riding. Never finer than the game's own grid, and never more than GroundGrid holds.
constexpr float kStep = 0.5f;

struct Plan {
    uint32_t w = 0, h = 0;
    float    step = 0, x0 = 0, z0 = 0, x1 = 0, z1 = 0;  // x1, z1: the far edges, inclusive
};

inline Plan PlanFor(const Header& hd) {
    Plan p;
    if (!Plausible(hd)) return p;
    const float native = (std::max)(hd.size_x / float(hd.cols - 1), hd.size_z / float(hd.rows - 1));
    const float most   = (std::max)(hd.size_x, hd.size_z) / float(coachline::kGridMaxDim - 1);
    p.step             = (std::max)({kStep, native, most});
    p.w                = (std::min)(coachline::kGridMaxDim, uint32_t(hd.size_x / p.step) + 1);
    p.h                = (std::min)(coachline::kGridMaxDim, uint32_t(hd.size_z / p.step) + 1);
    p.x0 = hd.origin_x, p.z0 = hd.origin_z;
    p.x1 = hd.origin_x + hd.size_x, p.z1 = hd.origin_z + hd.size_z;
    return p;
}

/// Where sample (c, r) is asked for: on the lattice, but never past the far edge, which the
/// sampler's bounds check would refuse.
inline void SamplePoint(const Plan& p, uint32_t c, uint32_t r, float& x, float& z) {
    x = (std::min)(p.x1, p.x0 + float(c) * p.step);
    z = (std::min)(p.z1, p.z0 + float(r) * p.step);
}

/// Less than this share answered and the grid is not the ground under the track.
constexpr float kMinAnswered = 0.05f;

/// The grid, a slice at a time. `ask(x, z, y)` returns 1 with the height, 0 for no answer, and
/// anything else to stop for good (it faulted).
class Builder {
public:
    enum State { IDLE, RUNNING, DONE, FAILED };

    void reset() {
        state_ = IDLE;
        grid_.clear();
        next_ = answered_ = 0;
    }
    bool start(const Header& hd) {
        reset();
        hd_   = hd;
        plan_ = PlanFor(hd);
        if (!plan_.w || !grid_.make(plan_.w, plan_.h, plan_.step, plan_.x0, plan_.z0)) {
            state_ = FAILED;
            return false;
        }
        state_ = RUNNING;
        return true;
    }
    /// Up to `budget` samples. Returns how many it asked.
    template <class Ask>
    size_t run(Ask&& ask, size_t budget) {
        if (state_ != RUNNING) return 0;
        const uint64_t total = uint64_t(plan_.w) * plan_.h;
        size_t         n     = 0;
        for (; n < budget && next_ < total; ++n, ++next_) {
            const uint32_t c = uint32_t(next_ % plan_.w), r = uint32_t(next_ / plan_.w);
            float          x, z, y = NAN;
            SamplePoint(plan_, c, r, x, z);
            const int got = ask(x, z, y);
            if (got == 1 && std::isfinite(y)) {
                grid_.set(c, r, y);
                ++answered_;
            } else if (got != 0) {
                state_ = FAILED;
                grid_.clear();
                return n + 1;
            }
        }
        if (next_ >= total) {
            state_ = float(answered_) >= kMinAnswered * float(total) ? DONE : FAILED;
            if (state_ == FAILED) grid_.clear();
        }
        return n;
    }

    State             state() const { return state_; }
    const Header&     header() const { return hd_; }
    const Plan&       plan() const { return plan_; }
    uint64_t          total() const { return uint64_t(plan_.w) * plan_.h; }
    uint64_t          asked() const { return next_; }
    uint64_t          answered() const { return answered_; }
    /// The finished grid, handed over (the builder is left empty).
    coachline::GroundGrid take() {
        coachline::GroundGrid g = std::move(grid_);
        reset();
        return g;
    }

private:
    State                 state_ = IDLE;
    Header                hd_;
    Plan                  plan_;
    coachline::GroundGrid grid_;
    uint64_t              next_ = 0, answered_ = 0;
};

/// How long the rider must have been riding (on track, moving, not crashed) before the game is
/// asked anything: the track is loaded and the physics is asking it the same thing by then. And
/// the slot's heightfield must read the same on two looks a second apart - not one the loader is
/// still filling in.
constexpr double kSettleMs = 3000.0;

/// When the next slice may run, and the watchdog over it. A slice is at most kSliceMs of asking,
/// and slices are spread out to a kDuty share of wall time, so a burst of telemetry calls (the
/// game catching up after a hitch) cannot add up to a stall. A slice far over its cap (one query
/// that took far too long) or too much asking in all trips it: off for the event.
///
/// The refresh near the rider (Refresher, below) runs for as long as the rider rides, so it has no
/// total: its cost is held down by a smaller duty instead (kRefreshDuty), and one slice far over
/// its cap still trips it.
class Pacer {
public:
    static constexpr double kSliceMs = 0.3, kDuty = 0.1, kOverrunMs = 8.0, kTotalMs = 2000.0;
    static constexpr double kRefreshDuty = 0.03;  // at most 3% of wall time re-asking
    static constexpr double kNoTotal     = 1e300;

    explicit Pacer(double duty = kDuty, double total_ms = kTotalMs) : duty_(duty), total_ms_(total_ms) {}
    void reset() { next_ms_ = busy_ms_ = 0, tripped_ = false; }
    bool may_run(double now_ms) const { return !tripped_ && now_ms >= next_ms_; }
    /// A slice of `slice_ms` ended at `now_ms`. False once tripped.
    bool done(double now_ms, double slice_ms) {
        busy_ms_ += slice_ms;
        next_ms_ = now_ms + slice_ms / duty_;
        if (slice_ms > kOverrunMs || busy_ms_ > total_ms_) tripped_ = true;
        return !tripped_;
    }
    bool   tripped() const { return tripped_; }
    double busy_ms() const { return busy_ms_; }

private:
    double duty_, total_ms_;
    double next_ms_ = 0, busy_ms_ = 0;
    bool   tripped_ = false;
};

// ---------------------------------------------------------------------------------------
// Keeping the grid current: the ruts (after 0.49.7)
//
// The grid above is made once per event, but the ground does not stay put. The game's heights
// (track +0x750, what the sampler reads) are the .trh's base less the rut layer (+0x7A8, 16.16
// fixed, recomputed at 0x1F6030 as rut blocks arrive), and on a server that digs the track live
// (mxbserver) the ruts deepen all session. A grid made in the first minute keeps the ground as
// it was then, and the line rides over the top of every rut wall dug since (Sean, 10-06: the
// line should be down in the rut, not on top of the wall). So the square of ground the line is
// about to cross is asked again about once a second, and whatever moved is written over the old
// heights. The same sampler, the same 0.3 ms slices, at most kRefreshDuty of wall time, on the
// same thread.

/// Half the side of the square asked again, and how far ahead of the rider its centre is. The
/// line is drawn kAhead (60 m) on; the near part is what the rider is about to ride and where a
/// floating line shows, so the square runs from behind the rider to ~45 m ahead.
constexpr float  kRefreshHalfM   = 28.0f;
constexpr float  kRefreshLeadM   = 16.0f;
constexpr double kRefreshEveryMs = 1000.0;
/// A cell that moved less than this has not been dug: not counted as changed.
constexpr float  kRefreshMovedM  = 0.01f;

/// Cells (c0..c1, r0..r1 inclusive) of a plan's lattice.
struct Window {
    uint32_t c0 = 0, r0 = 0, c1 = 0, r1 = 0;
    bool     ok = false;
    uint64_t cells() const { return ok ? uint64_t(c1 - c0 + 1) * (r1 - r0 + 1) : 0; }
};

/// The lattice cells within `half` metres (a square) of (x, z), clipped to the grid; not ok when
/// none are on it.
inline Window WindowAround(const Plan& p, float x, float z, float half) {
    Window w;
    if (!p.w || !p.h || !(p.step > 0) || !std::isfinite(x) || !std::isfinite(z) || !(half > 0)) return w;
    const float lo_c = std::ceil((x - half - p.x0) / p.step), hi_c = std::floor((x + half - p.x0) / p.step);
    const float lo_r = std::ceil((z - half - p.z0) / p.step), hi_r = std::floor((z + half - p.z0) / p.step);
    if (hi_c < 0 || hi_r < 0 || lo_c > float(p.w - 1) || lo_r > float(p.h - 1) || hi_c < lo_c || hi_r < lo_r) return w;
    w.c0 = uint32_t((std::max)(0.0f, lo_c)), w.c1 = uint32_t((std::min)(float(p.w - 1), hi_c));
    w.r0 = uint32_t((std::max)(0.0f, lo_r)), w.r1 = uint32_t((std::min)(float(p.h - 1), hi_r));
    w.ok = true;
    return w;
}

/// Where the square is centred: ahead of the rider along their travel, or on them standing still.
inline void RefreshCentre(float x, float z, float vx, float vz, float& cx, float& cz) {
    cx = x, cz = z;
    const float v = std::sqrt(vx * vx + vz * vz);
    if (std::isfinite(v) && v > 1.0f) cx += vx / v * kRefreshLeadM, cz += vz / v * kRefreshLeadM;
}

struct Cell {
    uint32_t c = 0, r = 0;
    float    y = 0;
};

/// One pass over a window, a slice at a time, into a patch: the heights the game gives now.
/// Passes start no more often than kRefreshEveryMs apart (from start to start).
class Refresher {
public:
    void reset() {
        running_ = false;
        next_    = 0;
        last_ms_ = -1e300;
        patch_.clear();
        passes_ = 0;
    }
    bool running() const { return running_; }
    bool due(double now_ms) const { return !running_ && now_ms - last_ms_ >= kRefreshEveryMs; }
    /// A pass over the square ahead of (x, z), moving at (vx, vz). False, and nothing started,
    /// when that square is off the grid.
    bool start(const Plan& p, float x, float z, float vx, float vz, double now_ms) {
        float cx, cz;
        RefreshCentre(x, z, vx, vz, cx, cz);
        const Window w = WindowAround(p, cx, cz, kRefreshHalfM);
        last_ms_       = now_ms;  // a miss waits its second too
        if (!w.ok) return false;
        plan_ = p, win_ = w, next_ = 0, running_ = true;
        patch_.clear();
        patch_.reserve(size_t(w.cells()));
        return true;
    }
    /// Up to `budget` samples, as Builder::run: `ask` 1 answered, 0 no answer, else it faulted.
    /// Returns how many it asked; -1 when the sampler faulted (the pass is dropped).
    template <class Ask>
    long run(Ask&& ask, size_t budget) {
        if (!running_) return 0;
        const uint64_t total = win_.cells();
        const uint32_t ww    = win_.c1 - win_.c0 + 1;
        size_t         n     = 0;
        for (; n < budget && next_ < total; ++n, ++next_) {
            const uint32_t c = win_.c0 + uint32_t(next_ % ww), r = win_.r0 + uint32_t(next_ / ww);
            float          x, z, y = NAN;
            SamplePoint(plan_, c, r, x, z);
            const int got = ask(x, z, y);
            if (got == 1 && std::isfinite(y)) {
                patch_.push_back({c, r, y});
            } else if (got != 0) {
                running_ = false;
                patch_.clear();
                return -1;
            }
        }
        if (next_ >= total) running_ = false, ++passes_;
        return long(n);
    }
    /// The finished pass's heights, handed over (empty while a pass runs).
    std::vector<Cell> take() {
        std::vector<Cell> p;
        if (!running_) p.swap(patch_);
        return p;
    }
    const Window& window() const { return win_; }
    uint64_t      passes() const { return passes_; }

private:
    bool              running_ = false;
    Plan              plan_;
    Window            win_;
    uint64_t          next_    = 0;
    double            last_ms_ = -1e300;
    std::vector<Cell> patch_;
    uint64_t          passes_ = 0;
};

/// A patch written into the grid it was asked for. How many known cells moved by more than
/// kRefreshMovedM, and the most any moved (signed: negative is dug down).
struct Applied {
    uint64_t changed = 0;
    float    most    = 0;
};
inline Applied Apply(coachline::GroundGrid& g, const std::vector<Cell>& patch) {
    Applied a;
    for (const Cell& k : patch) {
        const float was = g.cell(k.c, k.r);
        if (std::isfinite(was)) {
            if (std::fabs(k.y - was) <= kRefreshMovedM) continue;
            ++a.changed;
            if (std::fabs(k.y - was) > std::fabs(a.most)) a.most = k.y - was;
        }
        g.set(k.c, k.r, k.y);
    }
    return a;
}

// ---------------------------------------------------------------------------------------
// In the game's process (coachterrain.cpp).
namespace game {

struct Terrain {
    bool           ok = false;
    bool           behind_hook = false;  // FrostMod's guard sits on the sampler
    const void*    sampler = nullptr;
    const uint8_t* slots = nullptr;
};

/// Finds the sampler and the slots in this process, on the verified build only. False with the
/// reason otherwise.
bool Locate(Terrain& t, std::string& why);

/// Slot `i` (0-based), or null.
inline const uint8_t* Slot(const Terrain& t, int i) {
    return t.ok && i >= 0 && i < mxb::TRACK_SLOT_COUNT ? t.slots + size_t(i) * mxb::TRACK_SLOT_STRIDE : nullptr;
}

/// The game's answer at (x, z) on `slot`: 1 with the height in `y`, 0 for none, -1 if the call
/// faulted (it is then not to be called again). Only finite positions inside the slot's
/// bounds are passed on; anything else is 0 without asking.
int Ask(const Terrain& t, const uint8_t* slot, float x, float z, float& y);

}  // namespace game
}  // namespace coachterrain
