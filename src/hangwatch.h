// hangwatch.h - a stack in frostmod.log when the game stops presenting frames.
//
// WHY. A freeze ("not responding", then Windows ends the game with 0xCFFFFFFF) leaves no crash
// dump and no crash report: nothing faulted. The log just stops. Lxntrip's two freezes on
// 2026-11-25 ended on "[session] rider #N removed by the game", which says when, not where.
//
// WHAT. The render thread stamps a heartbeat every frame (the swap hook). A watchdog thread
// wakes twice a second; when no frame has come for kStallMs it suspends the render thread just
// long enough to read its registers, lets it go, and writes that thread's unwound stack (and
// where every other thread of the game is) into frostmod.log. Once per stall. When frames come
// back it logs how long the stall was, so a long track load reads as a load and not a hang.
//
// The decisions are here, without Win32, so tests/hangwatch_test.cpp runs anywhere.
#pragma once
#include <cstdint>

namespace hangwatch {

constexpr uint64_t kStallMs  = 5000;   // no frame for this long: write the stacks
constexpr uint64_t kAgainMs  = 30000;  // still none this much later: write them once more
constexpr unsigned kPollMs   = 500;

enum class Action { None, Report, ReportAgain, Recovered };

struct State {
    uint64_t stallBeat = 0;   // the heartbeat the current stall started from (0: not stalled)
    int      reports   = 0;   // stacks written for this stall
};

/// One watchdog tick. `beat` is the last frame's time (0 before the first frame: nothing to
/// watch yet), `now` the clock. Report on the first tick past kStallMs, once more past
/// kAgainMs, and Recovered when a new frame shows up after a reported stall.
inline Action Step(State& s, uint64_t now, uint64_t beat) {
    if (beat == 0) { s = State{}; return Action::None; }
    if (s.stallBeat != 0 && beat != s.stallBeat) {
        const bool reported = s.reports > 0;
        s = State{};
        return reported ? Action::Recovered : Action::None;
    }
    if (now <= beat || now - beat < kStallMs) return Action::None;
    if (s.stallBeat == 0) { s.stallBeat = beat; s.reports = 1; return Action::Report; }
    if (s.reports == 1 && now - beat >= kAgainMs) { s.reports = 2; return Action::ReportAgain; }
    return Action::None;
}

/// A module, for naming addresses without the loader lock (a hung thread may hold it).
struct Module { uintptr_t base; uintptr_t size; char name[48]; };

/// "name+0xRVA" for `addr`, or false when no module covers it.
inline bool Describe(const Module* mods, int n, uintptr_t addr, const Module** out, uintptr_t* rva) {
    for (int i = 0; i < n; ++i)
        if (addr >= mods[i].base && addr - mods[i].base < mods[i].size) {
            *out = &mods[i]; *rva = addr - mods[i].base; return true;
        }
    return false;
}

}  // namespace hangwatch
