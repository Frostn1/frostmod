// coachlink.h - whether MXB Coach is open, for mxbcoach.dlo.
//
// The recorder draws the line, the hints and the cue box from sheets MXB Coach leaves on disk.
// Up to 0.49.12 it kept drawing them after Coach was closed, so nothing the rider switched off
// in a Coach that was no longer running could reach it (GAMING PROFI 2, 0.49.7). Coach 0.1.25
// rewrites <save path>\mxbcoach\coach.ini every 2 s while it runs ([coach] on=1) and writes on=0
// on the way out. The recorder clears what it draws and stops drawing when the file says on=0,
// or hasn't been written for kTimeoutMs (Coach closed, crashed or hung). With no file at all
// there is nothing to say Coach is running, so it draws nothing either (it used to draw as
// before, for a Coach from before the file, and a rider whose Coach was off got the old line,
// minimap and banner back, GAMING PROFI 2, 2026-10-14). Only `standalone=1` in hud.ini draws
// without Coach.
//
// Pure, so tests/coachlink_test.cpp runs it anywhere.
#pragma once

#include <cstdint>
#include <string>

#include "stance.h"  // IniValue

namespace coachlink {

/// Three missed beats: long enough for a busy disk, short enough that the rider isn't left with
/// a line from a Coach that has gone.
constexpr int64_t kTimeoutMs = 6000;

enum State {
    NONE,  // no coach.ini: Coach has not run, or is from before the file. Nothing is drawn.
    ON,    // Coach is open and says so
    OFF,   // Coach said on=0: it was closed
    GONE,  // Coach stopped writing: crashed, hung or killed
};

/// What the file says. `exists` and `text` are coach.ini; `age_ms` how long since it was written.
inline State Decide(bool exists, const std::string& text, int64_t age_ms) {
    if (!exists) return NONE;
    if (stance::IniValue(text, "coach", "on") == "0") return OFF;
    if (age_ms > kTimeoutMs) return GONE;
    return ON;
}

/// Whether the recorder may draw Coach's line, hints and cues in this state. Only an open Coach
/// does, unless the rider turned on `standalone` (hud.ini): then whatever sheets are on disk.
inline bool Draws(State s, bool standalone = false) { return s == ON || standalone; }

inline const char* Name(State s) {
    switch (s) {
        case NONE: return "no coach.ini (Coach has not run): nothing is drawn";
        case ON: return "Coach is open";
        case OFF: return "Coach was closed: the line, hints and cues are cleared";
        case GONE: return "Coach stopped answering: the line, hints and cues are cleared";
    }
    return "?";
}

/// Milliseconds from `then` to `now`, both FILETIME ticks (100 ns). A file written "in the
/// future" (a clock set back) counts as just written.
inline int64_t AgeMs(uint64_t then, uint64_t now) { return now > then ? int64_t((now - then) / 10000) : 0; }

}  // namespace coachlink
