// When to clear a wedged server browser, and when to leave it alone.
//
// FrostMod watches the game's master login for the wedge — leave a server, open Browse, and
// the game times out forever against a master that is answering everyone else, because its
// own login was left half-open and its opener will not start a second one. Clearing it is a
// forged state write and a LOGOUT through the game's command bus; that half lives in
// frostmod.cpp, because it needs the process.
//
// This is the other half: the counters that decide whether a timeout is the wedge worth
// clearing or a master that is simply down. From inside the game the two are the same event —
// a 2 -> 1 login-state transition carrying reason 0 — so the whole judgement is made of what
// came before it, and none of it needs the game to be running. Kept here so it can be proved
// on any host, which is the only way it ever gets proved at all: nothing else in this file's
// subject matter can be reproduced without a wedged browser and a licensed copy of the game.
//
// No Win32, no I/O, no globals. Pure arithmetic over a State the caller owns.

#pragma once

#include <cstdint>

namespace worldwatch {

/// Resets one wedge gets before we stop believing it is a wedge.
///
/// A master that is genuinely down produces exactly the same timeout, so an unbroken run of
/// them has to stop somewhere — rebinding a socket every five seconds at a server that isn't
/// there helps nobody. Three is what one episode gets: close the session, the same again
/// taking the source port, and one spare.
///
/// The budget is per episode, not per run: see `OnLogin`.
inline constexpr int kMaxAutoResets = 3;

/// The quiet time after a reset before another one is allowed. The game retries its login
/// every five seconds, and a reset needs more than one retry to show whether it worked.
inline constexpr uint64_t kResetGapMs = 30000;

/// What the caller should do about the timeout it just saw.
enum class Action {
    /// Nothing has logged in yet this run, so this may simply be the master. Wait for a
    /// second timeout in a row before touching anything.
    HoldForOutage,
    /// Out of resets for this episode. Whatever this is, it isn't the half-open session.
    OverBudget,
    /// Too soon after the last reset to learn anything from another one.
    TooSoon,
    /// Clear it.
    Reset,
};

struct Decision {
    Action action = Action::HoldForOutage;
    /// Take the source port as well, not just the session. The cheap reset is tried first so
    /// that the login which follows says which half was actually to blame.
    bool alsoPort = false;
    /// For `TooSoon`: how much of the gap is left.
    uint64_t waitMs = 0;
};

/// Everything the decision is made of. One per run of the game.
struct State {
    /// A login reached the logged-in state at some point since the last reset. "It worked,
    /// then it stopped" is the wedge and nothing else, so this is what separates it from an
    /// outage.
    bool everLogged = false;
    /// Consecutive timeouts since the last success.
    int timeouts = 0;
    /// Resets since the last successful login — the budget `kMaxAutoResets` caps.
    int resets = 0;
    /// Resets this run, all episodes. Reported in the log and nothing else; it gates nothing,
    /// because a rider who wedges ten times in an evening should be fixed ten times.
    int totalResets = 0;
    /// Earliest tick another reset is allowed at.
    uint64_t nextResetOkMs = 0;
};

/// A login got through.
///
/// This refunds the episode's budget, and the escalation with it. The cap exists to stop us
/// rebinding sockets at a master that is down; a login getting through is proof it isn't, and
/// proof that whatever we last did worked. So the next wedge is a fresh episode and starts
/// from the cheap reset again.
///
/// Counting per run instead meant an evening of joining and leaving servers spent all three
/// on its first three sessions and left the rider with the app's manual button for the rest of
/// it — having just proved, three times over, that the automatic fix works on that machine.
inline void OnLogin(State& s) {
    s.everLogged = true;
    s.timeouts = 0;
    s.resets = 0;
}

/// The login timed out with nothing coming back at all.
///
/// The order of the tests is the order they have to be in. The outage hold comes first because
/// it is the only one that can be answered without spending anything; the budget before the
/// gap, so a run of timeouts against a dead master goes quiet rather than reporting a wait
/// that will never end in a reset.
inline Decision OnTimeout(State& s, uint64_t nowMs) {
    ++s.timeouts;

    if (!s.everLogged && s.timeouts < 2) return {Action::HoldForOutage, false, 0};
    if (s.resets >= kMaxAutoResets) return {Action::OverBudget, false, 0};
    if (nowMs < s.nextResetOkMs) return {Action::TooSoon, false, s.nextResetOkMs - nowMs};

    // The first reset of an episode closes the session and keeps the port; anything after
    // that takes the port too.
    const bool alsoPort = s.resets > 0;
    ++s.resets;
    ++s.totalResets;
    s.nextResetOkMs = nowMs + kResetGapMs;
    // Re-armed only by a fresh successful login: until then we are back to not knowing
    // whether this machine is wedged or the master is out.
    s.everLogged = false;
    s.timeouts = 0;
    return {Action::Reset, alsoPort, 0};
}

}  // namespace worldwatch
