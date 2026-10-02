// When FrostMod may rebuild the paint lists: the join-time paint sync rules.
//
// Paint sync used to land mid-ride. MXB App wrote a paint and asked for a refresh, and the
// refresh ran there and then: six paint-list loaders on the render thread, then every
// remote rider's paints re-applied in one frame. Network and render share that thread, so a
// long frame froze the riders on screen, and a longer one dropped them. The app could ask up
// to three times for one sync.
//
// The game itself never rescans paints after boot. The six paint loaders have exactly two
// callers each, both inside the boot content init, and joining a server does not run it.
// So a paint placed on disk after boot shows only after a refresh, and the refresh has to
// go somewhere a hitch costs nothing:
//
//   * Joining   the connection dialog is up (handshake, track load, "Caching n/m"). The
//               screen is a loading screen already. At most ONE refresh per join.
//   * Pits/Menu not riding. A short hitch there freezes nothing anyone is racing.
//   * Riding    never. Whatever arrives waits for the next of those.
//   * Unknown   the page stack could not be read: treated as riding.
//
// Requests coalesce: any number of asks while one is pending, or while a refresh runs, is
// one more refresh at most. A short settle lets a burst (the app's install, its folder
// watcher, a second rider arriving) land before the refresh starts.
//
// Pure - no Win32, no game - so tests/paintgate_test.cpp runs it in CI.
#pragma once

#include <cstdint>

#include "changefree.h"   // PageIs, PagesShowRiding

namespace frostmod {
namespace paintgate {

enum class Phase { Menu, Joining, Pits, Riding, Unknown };

inline const char* PhaseName(Phase p) {
    switch (p) {
        case Phase::Menu: return "menu";
        case Phase::Joining: return "joining";
        case Phase::Pits: return "pits";
        case Phase::Riding: return "riding";
        default: return "unknown";
    }
}

inline bool IsPitPage(const char* s) {
    return PageIs(s, "pit") || PageIs(s, "multi_pit") || PageIs(s, "testingday_pit") ||
           PageIs(s, "straightrhythm_pit");
}

// The phase from the game's page stack, plus the game's own "local rider is riding" flag
// (the one 0x5E570 tests before deferring a rider build). `names` may be null when the
// stack could not be read; that is Unknown.
inline Phase PhaseFromPages(const char* const* names, int n, bool vehicleRiding) {
    if (vehicleRiding) return Phase::Riding;   // the game says so; nothing overrides it
    if (!names || n < 0) return Phase::Unknown;
    bool pit = false;
    for (int i = 0; i < n; ++i) {
        if (PageIs(names[i], "connection_dialog")) return Phase::Joining;
        if (IsPitPage(names[i])) pit = true;
    }
    if (PagesShowRiding(names, n)) return Phase::Riding;
    return pit ? Phase::Pits : Phase::Menu;
}

// What a refresh is for. A paint sync refresh re-applies only riders whose paint was
// missing; the player's own look change re-applies everyone, as it always has.
enum Kind : unsigned { kNone = 0, kStaged = 1, kLook = 2 };

class Gate {
public:
    // The dialog gone for less than this and back again is the same join.
    static constexpr uint64_t kSameJoinMs = 3000;

    // Milliseconds a request must be quiet before the refresh starts.
    explicit Gate(uint64_t settleMs = 750) : settle_(settleMs) {}

    // MXB App (or anything else) asked for a paint refresh.
    void Request(unsigned kind, uint64_t nowMs) {
        pending_ |= kind;
        lastAsk_ = nowMs;
    }

    // Once a frame. `busy` = a reload or refresh is already running. Returns the kinds to
    // refresh for now (kNone = do nothing this frame); the caller starts exactly one
    // refresh when it is not kNone.
    unsigned Tick(Phase phase, bool busy, uint64_t nowMs) {
        // A new join - unless the dialog only flickered away for a moment (the page stack
        // changes under it as the load moves on), which is still the same join.
        if (phase == Phase::Joining && last_ != Phase::Joining &&
            (!seenJoin_ || nowMs - leftJoinAt_ >= kSameJoinMs)) {
            joinRefreshes_ = 0;
            seenJoin_ = true;
        }
        if (phase != Phase::Joining && last_ == Phase::Joining) leftJoinAt_ = nowMs;
        last_ = phase;
        if (!pending_ || busy) return kNone;
        if (phase == Phase::Riding || phase == Phase::Unknown) return kNone;
        if (nowMs - lastAsk_ < settle_) return kNone;
        if (phase == Phase::Joining) {
            if (joinRefreshes_ > 0) return kNone;   // one per join; the rest waits for the pits
            ++joinRefreshes_;
        }
        const unsigned k = pending_;
        pending_ = kNone;
        ++started_;
        return k;
    }

    unsigned Pending() const { return pending_; }
    int RefreshesThisJoin() const { return joinRefreshes_; }
    int Started() const { return started_; }

private:
    uint64_t settle_;
    unsigned pending_ = kNone;
    uint64_t lastAsk_ = 0;
    Phase last_ = Phase::Unknown;
    int joinRefreshes_ = 0;
    int started_ = 0;
    bool seenJoin_ = false;
    uint64_t leftJoinAt_ = 0;
};

// The full content reload MXB App pulses (a new track or bike installed) is not a paint
// refresh, but it is the same kind of hitch, and worse. It waits while riding too.
inline bool FullReloadMayStart(Phase phase) {
    return phase != Phase::Riding && phase != Phase::Unknown;
}

} // namespace paintgate
} // namespace frostmod
