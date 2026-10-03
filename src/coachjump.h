// coachjump.h - jump calls on the Coach ground line: where Coach's lap took off, where it landed,
// and what the jump is - SINGLE, DOUBLE, TRIPLE, QUAD, JUMP ON / JUMP OFF for a table, or ROLL
// where the lap rolled a face it could have jumped. Pure maths, no Win32 and no GL, so
// tests/coachjump_test.cpp runs it on synthetic terrain and on real sheets.
//
// What it reads, all from MXB Coach's .hud sheet (coachhud.h):
//   * the ground under the line: the centre column of "TRRN" (the track's own .trh terrain);
//   * where the lap was in the air: "AIRH" (the share of each stretch airborne, and the bike's
//     height above the ground), from Coach v-next on;
//   * the lap's speed: "DRIV", or its own times and positions.
// An older sheet with TRRN but no AIRH still marks its lips: the flight is predicted from each lip's
// angle and the lap's speed there (ballistic, no drag), and marked as predicted. A prediction says
// where a jump is, not which: it is called JUMP, never SINGLE or the like, and its landing is not
// drawn. ROLL needs AIRH: only the lap's own record can say it stayed on the ground.
//
// How a jump is counted. A face is a rise of at least kFaceRise metres, steep enough
// (kFaceSlope), short enough (kFaceMaxLen) to be built rather than a hill; its crest is the lip.
// A flight clears every crest from just before its takeoff to its landing: one is a SINGLE, two a
// DOUBLE, three a TRIPLE, four or more a QUAD. Landing on a plateau well above the lowest ground
// it flew over is a JUMP ON (onto a table, or onto the top of the next jump); taking off from a
// plateau and landing well below it with nothing in between is a JUMP OFF.
//
// READ-ONLY toward the game: the calls are drawn as extra geometry on the ribbon (coachmark.h).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "coachhud.h"
#include "coachline.h"
#include "coachmark.h"

namespace coachjump {

enum Kind { JUMP, SINGLE, DOUBLE, TRIPLE, QUAD, JUMP_ON, JUMP_OFF, ROLL, STEP_UP, STEP_DOWN, TABLE, HOP };
constexpr int kKinds = 12;

/// What the rider is told over the lip.
inline const char* KindName(Kind k) {
    switch (k) {
        case SINGLE: return "SINGLE";
        case DOUBLE: return "DOUBLE";
        case TRIPLE: return "TRIPLE";
        case QUAD: return "QUAD";
        case JUMP_ON: return "JUMP ON";
        case JUMP_OFF: return "JUMP OFF";
        case ROLL: return "ROLL";
        case STEP_UP: return "STEP UP";
        case STEP_DOWN: return "STEP DOWN";
        case TABLE: return "TABLE";
        case HOP: return "HOP";
        default: return "JUMP";
    }
}

// ---------------------------------------------------------------------------------------
// The lap along its points

constexpr float kFaceRise   = 0.6f;   // m: a face rises at least this much
constexpr float kFaceSlope  = 0.15f;  // and on average this steeply (8.5 degrees)
constexpr float kFaceMaxLen = 25.0f;  // m: and over no longer than this, or it's a hill
constexpr float kZigM       = 0.3f;   // m: a turn in the ground smaller than this is noise
constexpr float kMinJumpM   = 5.0f;   // m: a flight shorter than this is a hop, not a jump
constexpr float kMinAirS    = 0.35f;  // s: and one shorter than this too
constexpr float kShortAirS  = 0.6f;   // s: under this, a SINGLE or STEP UP must come off a built face
constexpr float kMinLipM    = 0.5f;   // m: a lip lower than this over the ground before it is a
                                      //    roller or a crest skipped off, not a jump
constexpr float kLipBackM   = 15.0f;  // m: how far back the ground is looked at for that
constexpr float kLipBend    = 0.12f;  // the slope change over 3 m either side of a natural lip
constexpr float kSharpBend  = 0.25f;  // and of an edge sharp enough to be one on its own
constexpr float kSureAirS   = 1.0f;   // s: this long in the air is a jump, lip or no lip
constexpr float kStepUpM    = 1.0f;   // m: landing this much higher is a STEP UP
constexpr float kStepDownM  = 1.5f;   // m: and this much lower, off flat ground, a STEP DOWN
constexpr float kTableTopM  = 4.0f;   // m: a flat top this long after the lip is a table
constexpr float kTableEndM  = 25.0f;  // m: a plateau that ends within this is a table, not a step
constexpr float kRhythmGapM = 12.0f;  // m: crests this close, three or more running, are a rhythm
constexpr float kBounceM    = 2.0f;   // m: back in the air this soon is one jump, bounced
constexpr float kLipSlackM  = 3.0f;   // m: a crest this far before the takeoff is its lip
constexpr float kPlateauM   = 3.0f;   // m: a plateau holds its height this long
constexpr float kPlateauTol = 0.3f;   // m: within this
constexpr float kStepM      = 0.8f;   // m: and stands this far above (or below) the other end
constexpr float kRollRise   = 0.9f;   // m: a rolled face this big is called ROLL
constexpr float kRollGroupM = 15.0f;  // m: rolled faces this close are one rolled section
constexpr float kAirLift    = 0.5f;   // m: without the airborne share, this far over its usual
                                      //    ride height the bike is in the air
constexpr float kGravity    = 9.81f;

/// Coach's lap, one entry a sheet point. Unknown values are NaN; channels the sheet lacks are
/// empty.
struct Line {
    std::vector<float> s;       // metres along the lap
    std::vector<float> t;       // seconds since the line
    std::vector<float> ground;  // the ground under the line's centre (TRRN)
    std::vector<float> air;     // airborne share 0..1 (AIRH)
    std::vector<float> above;   // the bike's height above the ground (AIRH)
    std::vector<float> speed;   // m/s (DRIV, else from the times)
    size_t size() const { return s.size(); }
};

/// The lap as coachjump reads it, from a sheet.
inline Line MakeLine(const coachhud::Sheet& sh) {
    Line         l;
    const size_t n = sh.ref.size();
    if (n < 2) return l;
    l.s.assign(n, 0.0f);
    l.t.assign(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        l.t[i] = sh.ref[i].t;
        if (i) {
            const float d = std::hypot(sh.ref[i].x - sh.ref[i - 1].x, sh.ref[i].z - sh.ref[i - 1].z);
            l.s[i]        = l.s[i - 1] + (d < coachline::kMaxGapM ? d : 0.0f);
        }
    }
    l.ground.assign(n, NAN);
    const uint32_t k = sh.terrain_k;
    if (k >= 2 && sh.terrain.size() == n * k)
        for (size_t i = 0; i < n; ++i) {
            // The middle sample, or the mean of the middle two.
            const float a = sh.terrain[i * k + (k - 1) / 2], b = sh.terrain[i * k + k / 2];
            l.ground[i]   = (std::isfinite(a) && std::isfinite(b)) ? 0.5f * (a + b) : NAN;
        }
    if (sh.air.size() == n * 3) {
        l.air.resize(n), l.above.resize(n);
        for (size_t i = 0; i < n; ++i) l.air[i] = sh.air[i * 3 + 2], l.above[i] = sh.air[i * 3 + 1];
    }
    l.speed.assign(n, NAN);
    if (sh.drive.size() == n * 3) {
        for (size_t i = 0; i < n; ++i) l.speed[i] = sh.drive[i * 3];
    } else {
        for (size_t i = 0; i < n; ++i) {
            size_t lo = i, hi = i;
            while (lo > 0 && l.s[i] - l.s[lo] < 3.0f) --lo;
            while (hi + 1 < n && l.s[hi] - l.s[i] < 3.0f) ++hi;
            const float dt = l.t[hi] - l.t[lo];
            if (dt > 1e-3f) l.speed[i] = (l.s[hi] - l.s[lo]) / dt;
        }
    }
    return l;
}

/// The ground at `s` metres along the lap, between the points either side, or NaN.
inline float GroundAt(const Line& l, float s) {
    const size_t n = l.size();
    if (n < 2 || l.ground.size() != n || !(s >= l.s.front()) || !(s <= l.s.back())) return NAN;
    const size_t hi = size_t(std::lower_bound(l.s.begin(), l.s.end(), s) - l.s.begin());
    if (hi == 0) return l.ground[0];
    const size_t lo   = hi - 1;
    const float  span = l.s[hi] - l.s[lo];
    const float  f    = span > 1e-4f ? (s - l.s[lo]) / span : 0.0f;
    return l.ground[lo] + (l.ground[hi] - l.ground[lo]) * f;
}

/// A face: the ground rising from its foot to its crest (point indices).
struct Face {
    size_t foot = 0, crest = 0;
    float  rise = 0;
};

/// The faces in the ground along the line. The ground is walked as a zigzag of turns bigger
/// than kZigM, so washboard and ruts don't split a face; each trough-to-crest leg that rises
/// kFaceRise, steeply enough, over no more than kFaceMaxLen, is a face.
inline std::vector<Face> Faces(const Line& l) {
    std::vector<Face> out;
    std::vector<size_t> ix;
    for (size_t i = 0; i < l.ground.size(); ++i)
        if (std::isfinite(l.ground[i])) ix.push_back(i);
    if (ix.size() < 3) return out;
    // The turns: alternate troughs and crests.
    std::vector<size_t> turns;
    size_t cand = ix[0], lo = ix[0], hi = ix[0];
    int    dir  = 0;  // +1 rising toward a crest, -1 falling toward a trough, 0 not yet known
    for (size_t k = 1; k < ix.size(); ++k) {
        const size_t i = ix[k];
        const float  g = l.ground[i], c = l.ground[cand];
        if (dir == 0) {
            // Until the ground has moved kZigM one way, the lowest and highest so far are both
            // where it might start.
            if (g <= l.ground[lo]) lo = i;
            if (g > l.ground[hi]) hi = i;
            if (g - l.ground[lo] >= kZigM) turns.push_back(lo), dir = +1, cand = i;
            else if (l.ground[hi] - g >= kZigM) turns.push_back(hi), dir = -1, cand = i;
            continue;
        }
        if (dir > 0) {
            if (g >= c) cand = i;
            else if (c - g >= kZigM) turns.push_back(cand), dir = -1, cand = i;
        } else {
            if (g <= c) cand = i;
            else if (g - c >= kZigM) turns.push_back(cand), dir = +1, cand = i;
        }
    }
    turns.push_back(cand);
    for (size_t k = 0; k + 1 < turns.size(); ++k) {
        const size_t a = turns[k], b = turns[k + 1];
        const float  rise = l.ground[b] - l.ground[a];
        if (rise < kFaceRise) continue;
        // The face runs from where the ground leaves the bottom to where it stops climbing: the
        // last point within a few centimetres of the trough, and the first within a few of the
        // top, so flat ground before it doesn't count as face and a table's crest is its lip.
        size_t foot = a, crest = b;
        for (size_t i = a; i <= b; ++i)
            if (std::isfinite(l.ground[i]) && l.ground[i] - l.ground[a] < 0.05f) foot = i;
        for (size_t i = foot; i <= b; ++i)
            if (std::isfinite(l.ground[i]) && l.ground[b] - l.ground[i] < 0.05f) {
                crest = i;
                break;
            }
        const float len = l.s[crest] - l.s[foot];
        if (len > kFaceMaxLen || !(len > 0) || rise / len < kFaceSlope) continue;
        out.push_back({foot, crest, rise});
    }
    return out;
}

/// One call: a jump (or a rolled section), by point index and metres along the lap.
struct Call {
    Kind   kind      = JUMP;
    size_t takeoff   = 0, landing = 0;
    float  takeoff_s = 0, landing_s = 0;
    float  airtime   = 0;    // s
    float  speed     = NAN;  // m/s at the takeoff
    int    faces     = 0;    // crests cleared (or rolled)
    bool   predicted = false;  // flown from the lip's angle, not the lap's own record
    // The flight's height above the ground, a sample a metre from the takeoff on.
    std::vector<float> arc;
};

/// Where the lap was in the air, as [first airborne, first grounded again) index runs. From the
/// airborne share when the sheet has it, else from the height above the ground against the bike's
/// usual ride height. Empty without either.
inline std::vector<std::pair<size_t, size_t>> AirRuns(const Line& l) {
    std::vector<std::pair<size_t, size_t>> out;
    const size_t n = l.size();
    std::vector<bool> up(n, false);
    if (l.air.size() == n) {
        for (size_t i = 0; i < n; ++i) up[i] = l.air[i] >= 0.5f;
    } else if (l.above.size() == n) {
        std::vector<float> h;
        for (float v : l.above)
            if (std::isfinite(v)) h.push_back(v);
        if (h.size() < n / 2) return out;
        std::nth_element(h.begin(), h.begin() + long(h.size() / 2), h.end());
        const float usual = h[h.size() / 2];
        for (size_t i = 0; i < n; ++i) up[i] = std::isfinite(l.above[i]) && l.above[i] > usual + kAirLift;
    } else {
        return out;
    }
    for (size_t i = 0; i < n;) {
        if (!up[i]) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < n && up[j]) ++j;
        // A touch and straight back up is one flight, bounced.
        if (!out.empty() && l.s[i] - l.s[out.back().second] < kBounceM) out.back().second = j;
        else out.push_back({i, j});
        i = j;
    }
    return out;
}

/// The lap's speed at point `i`, or NaN.
inline float SpeedAt(const Line& l, size_t i) { return i < l.speed.size() ? l.speed[i] : NAN; }

/// Whether the crest at `cs` is one of three or more crests running, each within kRhythmGapM
/// of the next: a supercross rhythm lane, where jumps are counted by the crests they clear.
inline bool InRhythm(const Line& l, const std::vector<Face>& faces, float cs) {
    for (size_t k = 0; k < faces.size(); ++k) {
        size_t j = k;
        while (j + 1 < faces.size() && l.s[faces[j + 1].crest] - l.s[faces[j].crest] <= kRhythmGapM) ++j;
        if (j - k + 1 >= 3 && cs >= l.s[faces[k].crest] - kLipSlackM && cs <= l.s[faces[j].crest] + 1.0f) return true;
        k = j;
    }
    return false;
}

/// What a flight from `take` to `land` was, by the ground under it, the way a rider names it:
/// - in a rhythm lane, by the crests it cleared: SINGLE, DOUBLE, TRIPLE, QUAD;
/// - onto a table's top: JUMP ON; off its far end: JUMP OFF; clean over a flat top: TABLE;
/// - over a gap onto a separate landing face: DOUBLE;
/// - landing on higher ground: STEP UP; off flat ground onto much lower: STEP DOWN;
/// - off a lip at least kMinLipM high: SINGLE;
/// - anything else (a skip off a roller, a hop over a crest) is a HOP, which isn't called.
/// `count` is the crests cleared, the lip included. JUMP when the ground isn't known.
inline Kind Classify(const Line& l, const std::vector<Face>& faces, size_t take, size_t land, int& count) {
    count = 0;
    const float st = l.s[take], sl = l.s[land];
    int after = 0;
    for (const Face& f : faces) {
        const float cs = l.s[f.crest];
        if (cs >= st - kLipSlackM && cs <= sl) ++count;
        if (cs > st + 1.0f && cs <= sl) ++after;
    }
    const float gt = GroundAt(l, st), gl = GroundAt(l, sl);
    if (!std::isfinite(gt) || !std::isfinite(gl)) return JUMP;
    auto flat = [&](float from, float to, float h) {
        for (float s = from; s <= to + 1e-3f; s += 0.5f) {
            const float g = GroundAt(l, s);
            if (!std::isfinite(g) || std::fabs(g - h) > kPlateauTol) return false;
        }
        return true;
    };
    // Where flat ground at height `h` from `s` runs out going `dir`, within `within` metres, by
    // dropping kStepM below it; NaN when it doesn't.
    auto drops = [&](float s, float dir, float h, float within) {
        for (float d = 0.5f; d <= within; d += 0.5f) {
            const float g = GroundAt(l, s + dir * d);
            if (!std::isfinite(g)) return false;
            if (h - g >= kStepM) return true;
            if (g - h > kPlateauTol) return false;  // it climbs on: a step, not a table
        }
        return false;
    };
    // Is the takeoff a lip at all? A built face's crest is; otherwise the ground has to bend
    // over there - climbing into it and falling (or flattening) away after - rather than run on
    // up or down a hill, which is a skip, not a jump.
    const float before = GroundAt(l, st - 3.0f), ahead = GroundAt(l, st + 3.0f);
    const float bend   = std::isfinite(before) && std::isfinite(ahead) ? ((gt - before) - (ahead - gt)) / 3.0f : 0.0f;
    const bool  lip    = count >= 1 || bend >= kSharpBend || (bend >= kLipBend && gt - before >= kMinLipM * 0.6f);
    float low = INFINITY;
    for (size_t i = take; i <= land; ++i)
        if (std::isfinite(l.ground[i])) low = (std::min)(low, l.ground[i]);
    // A rhythm lane: counted by the crests cleared.
    if (InRhythm(l, faces, st)) {
        if (gl - low >= kStepM && flat(sl, sl + kPlateauM, gl)) return JUMP_ON;
        count = (std::max)(count, 1);
        return count == 1 ? SINGLE : count == 2 ? DOUBLE : count == 3 ? TRIPLE : QUAD;
    }
    // Onto a table's top (a plateau over the ground flown across, ending ahead), or off its end.
    if (gl - low >= kStepM && flat(sl, sl + kPlateauM, gl) && drops(sl, +1.0f, gl, kTableEndM)) return JUMP_ON;
    if (after == 0 && gt - gl >= kStepM && flat(st - kPlateauM, st, gt) && drops(st, -1.0f, gt, kTableEndM))
        return JUMP_OFF;
    // Anything else needs a lip; a long flight is a jump whatever the ground says about it.
    if (!lip && l.t[land] - l.t[take] < kSureAirS) return HOP;
    // Onto higher ground, whatever was crossed on the way up.
    if (gl - gt >= kStepUpM) return STEP_UP;
    // Over a gap onto a separate landing face.
    if (after >= 1) return DOUBLE;
    // The ground behind the lip, at its lowest.
    float behind = gt;
    for (float d = 0.5f; d <= kLipBackM; d += 0.5f) {
        const float g = GroundAt(l, st - d);
        if (std::isfinite(g)) behind = (std::min)(behind, g);
    }
    // Clean over a flat top that stands over the approach: a table, not flat ground.
    if (gt - behind >= kStepM && flat(st + 1.0f, st + kTableTopM, gt) && gl < gt - kPlateauTol) return TABLE;
    if (gt - gl >= kStepDownM && flat(st - kPlateauM, st, gt)) return STEP_DOWN;
    // A lip standing over the ground behind it.
    if (gt - behind >= kMinLipM || count >= 1) return SINGLE;
    return HOP;
}

/// The flight predicted from a lip: launched along the face's last two metres at the lap's speed
/// there, landing where it meets the ground again. False when it doesn't leave the ground for
/// kMinJumpM (a face rolled at that speed), or the ground runs out first.
inline bool Predict(const Line& l, size_t lip, float& land_s, float& airtime, std::vector<float>& arc) {
    const float v  = SpeedAt(l, lip);
    const float s0 = l.s[lip], g0 = GroundAt(l, s0), gb = GroundAt(l, s0 - 2.0f);
    if (!(v > 5.0f) || !std::isfinite(g0) || !std::isfinite(gb)) return false;
    const float th = std::atan2(g0 - gb, 2.0f), vx = v * std::cos(th), vy = v * std::sin(th);
    arc.clear();
    for (float x = 0.5f; x < 80.0f; x += 0.5f) {
        const float t = x / vx, y = g0 + vy * t - 0.5f * kGravity * t * t;
        const float g = GroundAt(l, s0 + x);
        if (!std::isfinite(g)) return false;
        if (std::fmod(x, 1.0f) < 0.25f) arc.push_back((std::max)(0.0f, y - g));
        if (y <= g) {
            if (x < kMinJumpM) return false;
            land_s  = s0 + x;
            airtime = t;
            return true;
        }
    }
    return false;
}

/// The jump calls along Coach's lap, in lap order.
inline std::vector<Call> Calls(const Line& l) {
    std::vector<Call> out;
    const size_t n = l.size();
    if (n < 8) return out;
    const std::vector<Face> faces = Faces(l);
    const auto runs = AirRuns(l);
    const bool recorded = l.air.size() == n || l.above.size() == n;
    if (recorded) {
        // The usual ride height, for the flight's height above the ground.
        float usual = NAN;
        if (l.above.size() == n) {
            std::vector<float> h;
            for (float v : l.above)
                if (std::isfinite(v)) h.push_back(v);
            if (!h.empty()) {
                std::nth_element(h.begin(), h.begin() + long(h.size() / 2), h.end());
                usual = h[h.size() / 2];
            }
        }
        for (const auto& r : runs) {
            Call c;
            c.takeoff   = r.first > 0 ? r.first - 1 : 0;
            c.landing   = (std::min)(r.second, n - 1);
            c.takeoff_s = l.s[c.takeoff], c.landing_s = l.s[c.landing];
            c.airtime   = l.t[c.landing] - l.t[c.takeoff];
            if (c.landing_s - c.takeoff_s < kMinJumpM || c.airtime < kMinAirS) continue;
            c.speed = SpeedAt(l, c.takeoff);
            c.kind  = Classify(l, faces, c.takeoff, c.landing, c.faces);
            // A skip, not a jump: nothing to call. A short flight counts only off a built face.
            if (c.kind == HOP || (c.airtime < kShortAirS && c.faces == 0 && (c.kind == SINGLE || c.kind == STEP_UP))) continue;
            if (std::isfinite(usual))
                for (float s = c.takeoff_s; s <= c.landing_s; s += 1.0f) {
                    size_t i = size_t(std::lower_bound(l.s.begin(), l.s.end(), s) - l.s.begin());
                    i        = (std::min)(i, n - 1);
                    const float h = l.above[i] - usual;
                    c.arc.push_back(std::isfinite(h) ? (std::max)(0.0f, h) : 0.0f);
                }
            out.push_back(c);
        }
        // The faces the lap stayed on the ground over, big ones only, grouped by section.
        for (size_t k = 0; k < faces.size();) {
            const Face& f    = faces[k];
            auto        flown = [&](const Face& x) {
                const float cs = l.s[x.crest];
                for (const Call& c : out)
                    if (c.kind != ROLL && cs >= c.takeoff_s - kLipSlackM && cs <= c.landing_s + 1.0f) return true;
                return false;
            };
            if (f.rise < kRollRise || flown(f)) {
                ++k;
                continue;
            }
            Call c;
            c.kind      = ROLL;
            c.takeoff   = f.crest;
            c.landing   = f.crest;
            c.faces     = 1;
            size_t j    = k + 1;
            while (j < faces.size() && l.s[faces[j].crest] - l.s[c.landing] <= kRollGroupM && !flown(faces[j])) {
                if (faces[j].rise >= kFaceRise) c.landing = faces[j].crest, ++c.faces;
                ++j;
            }
            c.takeoff_s = l.s[c.takeoff], c.landing_s = l.s[c.landing];
            c.speed     = SpeedAt(l, c.takeoff);
            out.push_back(c);
            k = j;
        }
        std::sort(out.begin(), out.end(), [](const Call& a, const Call& b) { return a.takeoff_s < b.takeoff_s; });
        return out;
    }
    // No record of the air: fly each lip at the lap's speed.
    float done_s = -INFINITY;
    for (const Face& f : faces) {
        if (l.s[f.crest] < done_s) continue;  // under a flight already called
        Call c;
        if (!Predict(l, f.crest, c.landing_s, c.airtime, c.arc)) continue;
        c.predicted = true;
        c.takeoff   = f.crest;
        c.takeoff_s = l.s[f.crest];
        c.landing   = size_t(std::lower_bound(l.s.begin(), l.s.end(), c.landing_s) - l.s.begin());
        c.landing   = (std::min)(c.landing, n - 1);
        c.speed     = SpeedAt(l, c.takeoff);
        c.kind      = Classify(l, faces, c.takeoff, c.landing, c.faces);
        if (c.kind == HOP) continue;
        // Where the lip is, yes; what it is, no. A flight flown from the lip's angle lands short of
        // the lap's real one (on SavageMX, Maryland, Walnut and Carson, whose sheets have both, the
        // prediction named the right kind for 15 of 41 and said SINGLE for most of the rest, where
        // the lap had cleared a double, a table or a step), so it is not named: a plain JUMP.
        c.kind      = JUMP;
        done_s      = c.landing_s;
        out.push_back(c);
    }
    return out;
}

/// The label's first line: what the jump is.
inline std::string Label(const Call& c) { return KindName(c.kind); }

/// The label's second line: the takeoff speed Coach's lap carried, km/h, or nothing.
inline std::string SpeedHint(const Call& c) {
    if (c.kind == ROLL || !(c.speed > 1.0f) || !(c.speed < 120.0f)) return "";
    char b[16];
    std::snprintf(b, sizeof(b), "%d KMH", int(std::lround(c.speed * 3.6f)));
    return b;
}

/// One line for the log: how many calls of each kind, and where they came from.
inline std::string Summary(const Line& l, const std::vector<Call>& calls) {
    const char* from = l.air.size() == l.size() && !l.air.empty() ? "the lap's air"
                       : l.above.size() == l.size() && !l.above.empty() ? "the lap's height"
                       : !Faces(l).empty() ? "predicted from the lips"
                                           : "none (no terrain under the line)";
    int k[kKinds] = {};
    for (const Call& c : calls) ++k[c.kind];
    std::string out = std::to_string(calls.size()) + " calls from " + from + ":";
    for (int i = 0; i < kKinds; ++i)
        if (k[i]) out += std::string(" ") + KindName(Kind(i)) + "=" + std::to_string(k[i]);
    return out;
}

// ---------------------------------------------------------------------------------------
// On the ribbon

constexpr float kBarHalfW   = 0.65f;  // the takeoff bar: wider than the ribbon (0.35 a side)
constexpr float kBarHalfD   = 0.16f;
constexpr float kLipStripH  = 0.22f;  // the strip standing across the lip
constexpr float kMarkLift   = 0.012f; // above the ribbon, so it draws over it
constexpr float kLandHalfW  = 0.6f;   // the landing box
constexpr float kLandHalfD  = 0.6f;
constexpr float kLandEdge   = 0.07f;
constexpr float kLabelLift  = 2.0f;   // m over the lip, the label's bottom
constexpr float kLabelH     = 1.0f;   // cap height of the first line
constexpr float kHintH      = 0.55f;  // and of the speed hint under it
constexpr float kLabelNear  = 5.0f;   // closer than this the label is gone (it's under the rider)
constexpr float kLabelFull  = 10.0f;  // and from here it is solid
constexpr float kLabelFarFull = 50.0f;
constexpr float kLabelFar   = 60.0f;

/// Each sheet point's distance ahead of the rider along the ribbon, walked the way
/// coachline::Ribbon walks it (from `anchor`, `frac` of the way along its segment), out to
/// `ahead` metres; NaN for points not reached.
inline std::vector<float> AheadOf(const std::vector<coachhud::RefPoint>& ref, int anchor, float frac, float ahead) {
    const size_t       n = ref.size();
    std::vector<float> out(n, NAN);
    if (anchor < 0 || size_t(anchor) >= n || n < 2) return out;
    size_t i = size_t(anchor);
    float  d = -frac * std::hypot(ref[(i + 1) % n].x - ref[i].x, ref[(i + 1) % n].z - ref[i].z);
    out[i]   = d;
    for (size_t k = 0; k + 1 < n && d <= ahead; ++k) {
        const size_t j   = (i + 1) % n;
        const float  len = std::hypot(ref[j].x - ref[i].x, ref[j].z - ref[i].z);
        if (len >= coachline::kMaxGapM) break;
        d += len;
        if (std::isfinite(out[j])) break;  // round the whole lap
        out[j] = d;
        i      = j;
    }
    return out;
}

/// How solid the label is `s` metres ahead.
inline float LabelAlpha(float s) {
    if (!(s > kLabelNear) || !(s < kLabelFar)) return 0.0f;
    if (s < kLabelFull) return (s - kLabelNear) / (kLabelFull - kLabelNear);
    if (s > kLabelFarFull) return 1.0f - (s - kLabelFarFull) / (kLabelFar - kLabelFarFull);
    return 1.0f;
}

/// Where the rider is on Coach's line, so a mark can be placed by its metre on the line rather than
/// by its distance from the rider. `verts` (the ribbon) is rebuilt only every couple of metres and
/// its `s` is the distance from the rider *at that rebuild*; the `m` of its rows is fixed to the
/// line. Placing by `s` made every mark creep up to two metres and snap back each rebuild, which
/// is the jitter Sean saw. Without a Frame the marks are placed by `s` as they were.
struct Frame {
    float rider_m = NAN;  // the rider's metre on the line now
    float total   = 0;    // metres round the whole lap
};

/// The nearest call's label, when the rider has it fixed on screen rather than over the lip
/// (LineLook::jump.screen): Marks leaves it off the line and hands it over for the HUD to draw.
struct ScreenLabel {
    bool        ok = false;
    std::string top, hint;
    float       rgb[3] = {1, 1, 1};
    float       alpha  = 0;
    float       ahead  = 0;  // metres to the lip
};

/// The marks for the calls ahead of the rider: a bar across the line at each lip, a box where
/// Coach's lap landed with a faint dotted arc of its flight between, and the label standing over
/// the lip. `ahead` from AheadOf; `verts` the ribbon as built. Appends to `out`.
inline void Marks(std::vector<coachmark::Quad>& out, const std::vector<Call>& calls, const std::vector<float>& ahead,
                  const std::vector<coachline::Vert>& verts, bool speed_hint = true, const Frame* fr = nullptr,
                  ScreenLabel* screen = nullptr) {
    using coachmark::Flat;
    using coachmark::Spot;
    const size_t first = out.size();
    const bool   fixed = fr && std::isfinite(fr->rider_m) && fr->total > 0 && verts.size() >= 4;
    // A distance ahead of the rider now -> the same place in the ribbon's own `s`.
    auto in_ribbon = [&](float x) {
        if (!fixed) return x;
        const coachline::Vert& v0 = verts[0];
        return v0.s + coachline::WrapAhead(fr->rider_m + x, v0.m, fr->total);
    };
    auto At = [&](const std::vector<coachline::Vert>& v, float x) { return coachmark::At(v, in_ribbon(x)); };
    for (const Call& c : calls) {
        if (c.takeoff >= ahead.size() || c.landing >= ahead.size()) continue;
        const float st = ahead[c.takeoff];
        // The landing ahead too when the walk reached it; else as far past the lip as it flew.
        const float sl = std::isfinite(ahead[c.landing]) ? ahead[c.landing] : st + (c.landing_s - c.takeoff_s);
        if (!std::isfinite(st) && !std::isfinite(ahead[c.landing])) continue;
        if (std::isfinite(st) ? st > coachline::kAhead : sl < -1.0f) continue;
        const bool  roll = c.kind == ROLL;
        const float fade_t = std::isfinite(st) ? coachline::Fade((std::max)(0.0f, st)) : 0.0f;
        // The takeoff bar, a dark edge under a bright one so it reads on any colour of line.
        if (std::isfinite(st) && st > -1.0f) {
            const Spot p = At(verts, st);
            const float dark[4] = {0.05f, 0.05f, 0.06f, 0.55f * fade_t};
            const float lit[4]  = {1.0f, 1.0f, 1.0f, (roll ? 0.55f : 0.95f) * fade_t};
            Flat(out, p, st, 0, kBarHalfW + 0.06f, kBarHalfD + 0.06f, kMarkLift, dark);
            Flat(out, p, st, 0, kBarHalfW, kBarHalfD, kMarkLift * 2, lit);
            // And a low strip standing across the lip: a bar lying flat is a sliver from 40 m
            // back, up the face, where the rider most needs to see where it is.
            if (p.ok) {
                coachmark::Quad q;
                const float     xs[4] = {-kBarHalfW, kBarHalfW, kBarHalfW, -kBarHalfW};
                const float     ys[4] = {0.0f, 0.0f, kLipStripH, kLipStripH};
                for (int i = 0; i < 4; ++i) {
                    q.p[i][0] = p.x - p.lx * xs[i];
                    q.p[i][1] = (xs[i] < 0 ? p.yl : p.yr) + ys[i];
                    q.p[i][2] = p.z - p.lz * xs[i];
                }
                q.rgba[0] = q.rgba[1] = q.rgba[2] = 1.0f;
                q.rgba[3] = (roll ? 0.35f : 0.8f) * fade_t;
                q.s       = st;
                out.push_back(q);
            }
        }
        // (A predicted flight's landing is a guess that runs short, so it isn't drawn.)
        if (!roll && !c.predicted && sl > 0 && sl <= coachline::kAhead) {
            // The landing: a hollow box, faint, so it marks a spot without shouting.
            const Spot  p     = At(verts, sl);
            const float fade  = coachline::Fade(sl);
            const float col[4] = {0.80f, 0.93f, 1.0f, 0.75f * fade};
            Flat(out, p, sl, 0, kLandHalfW, kLandEdge, kMarkLift, col);  // middle cross bar
            const float fs[2] = {-kLandHalfD, kLandHalfD};
            for (float f : fs) Flat(out, At(verts, sl + f), sl + f, 0, kLandHalfW, kLandEdge, kMarkLift, col);
            for (float side : {-kLandHalfW, kLandHalfW})
                for (float f = -kLandHalfD; f < kLandHalfD - 0.01f; f += 0.3f)
                    Flat(out, At(verts, sl + f + 0.15f), sl + f + 0.15f, side, kLandEdge, 0.15f, kMarkLift, col);
            // The flight, a dash a metre, at the height Coach's lap flew.
            const float from = std::isfinite(st) ? st : sl - (c.landing_s - c.takeoff_s);
            for (size_t k = 1; k < c.arc.size(); ++k) {
                const float s = from + float(k);
                if (s < 1.0f || s > sl - 0.5f) continue;
                const Spot q = At(verts, s);
                if (!q.ok) continue;
                const float a = 0.45f * coachline::Fade(s), h = c.arc[k];
                if (h < 0.3f) continue;
                coachmark::Quad d;
                const float y0 = q.y + h, y1 = y0 + 0.07f;
                const float xs[4] = {-0.25f, 0.25f, 0.25f, -0.25f}, ys[4] = {y0, y0, y1, y1};
                for (int i = 0; i < 4; ++i) {
                    d.p[i][0] = q.x + q.fx * xs[i];
                    d.p[i][1] = ys[i];
                    d.p[i][2] = q.z + q.fz * xs[i];
                }
                d.rgba[0] = 1, d.rgba[1] = 1, d.rgba[2] = 1, d.rgba[3] = a;
                d.s = s;
                out.push_back(d);
            }
        }
        // The label over the lip, facing back down the line at the rider coming to it.
        // Not at all with the line's text off (line_text=0), and at the rider's size.
        const coachhud::LineLook& look = coachline::Look();
        // The call has its own style and size (hud.ini jump_style, jump_size), the line's text
        // style and size when it has none, and can be switched off or fixed on screen.
        const coachhud::TextItem& item = look.jump;
        const float la = std::isfinite(st) && look.text && item.on ? LabelAlpha(st) : 0.0f;
        if (la > 0 && item.screen) {
            if (screen && (!screen->ok || st < screen->ahead)) {
                screen->ok    = true;
                screen->ahead = st;
                screen->alpha = la;
                screen->top   = Label(c);
                screen->hint  = speed_hint ? SpeedHint(c) : "";
                screen->rgb[0] = roll ? 0.72f : 1.0f, screen->rgb[1] = roll ? 0.86f : 1.0f, screen->rgb[2] = roll ? 1.0f : 1.0f;
                if (c.kind == JUMP_ON || c.kind == JUMP_OFF || c.kind == TABLE || c.kind == STEP_UP || c.kind == STEP_DOWN)
                    screen->rgb[0] = 1.0f, screen->rgb[1] = 0.88f, screen->rgb[2] = 0.35f;
            }
        } else if (la > 0) {
            const float size = item.size > 0.0f ? item.size : look.text_size;
            const int   sty  = item.style != coachhud::kStyleAuto ? item.style : look.text_style;
            const float lh = kLabelH * size, hh = kHintH * size;
            const Spot p = At(verts, st);
            if (!p.ok) continue;
            const float right[3] = {-p.lx, 0, -p.lz};  // the rider's right, facing along the line
            const std::string top = Label(c), hint = speed_hint ? SpeedHint(c) : "";
            const float lift = kLabelLift + (hint.empty() ? 0.0f : hh * 1.6f);
            float base[3] = {p.x, p.y + lift, p.z};
            float shadow[3] = {base[0] + p.fx * 0.06f + right[0] * 0.05f, base[1] - 0.05f, base[2] + p.fz * 0.06f + right[2] * 0.05f};
            const float dark[4] = {0.0f, 0.0f, 0.0f, 0.6f * la};
            float col[4] = {1.0f, 1.0f, 1.0f, la};
            if (roll) col[0] = 0.72f, col[1] = 0.86f, col[2] = 1.0f;
            if (c.kind == JUMP_ON || c.kind == JUMP_OFF || c.kind == TABLE || c.kind == STEP_UP || c.kind == STEP_DOWN)
                col[0] = 1.0f, col[1] = 0.88f, col[2] = 0.35f;
            coachmark::Text(out, top, shadow, right, lh, dark, st, sty);
            coachmark::Text(out, top, base, right, lh, col, st, sty);
            if (!hint.empty()) {
                base[1] -= hh * 1.6f, shadow[1] -= hh * 1.6f;
                const float hc[4] = {1.0f, 1.0f, 1.0f, 0.85f * la};
                coachmark::Text(out, hint, shadow, right, hh, dark, st, sty);
                coachmark::Text(out, hint, base, right, hh, hc, st, sty);
            }
        }
    }
    // Each quad's place on the line, for the ground correction held for that place.
    if (fixed)
        for (size_t i = first; i < out.size(); ++i) {
            float m = std::fmod(fr->rider_m + out[i].s, fr->total);
            if (m < 0) m += fr->total;
            out[i].m = m;
        }
}

}  // namespace coachjump
