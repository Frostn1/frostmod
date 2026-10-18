// coachpet.h - Pip, the Coach pet: a small pixel-art chick that runs Coach's line ahead of the
// rider, where the ground line would be, flies the jumps the way Coach's lap flew them and pulls
// a face in the air. Pure maths, no GL, so tests/coachpet_test.cpp checks exactly what is drawn.
//
// It is built out of coloured quads standing upright on the ribbon as built (coachmark.h), so it
// lies on the same ground as the line and the jump calls, takes the same ground correction and is
// drawn in the same pass. With `pet` on and `ground` off, the ribbon is still built (Pip needs it
// to stand on) but only Pip is drawn: Pip *is* the line then.
//
// Where it runs: as far ahead as Coach's lap is at the same lap time (the ghost), kept between
// kMinLead and kMaxLead so it is always in front of the rider and never past the ribbon's end,
// and eased so a jump in the ghost's place doesn't make it teleport. Without a ghost it holds
// kDefaultLead.
//
// What it does in the air: a jump call (coachjump.h) carries the flight's height above the ground
// a metre at a time, from Coach's own lap (AIRH) or predicted from the lip. Pip flies that arc.
// Over a triple or a quad it is scared; over anything smaller, amazed; for a moment after the
// landing, happy. On the ground it hops and blinks.
//
// READ-ONLY toward the game, like the ribbon: it only adds geometry to the plugin's own draw.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "coachjump.h"
#include "coachline.h"
#include "coachmark.h"

namespace coachpet {

constexpr float kHeight      = 1.2f;   // Pip stands this tall, metres: readable from 40 m
constexpr float kMinLead     = 7.0f;   // never closer than this ahead of the rider
constexpr float kMaxLead     = 45.0f;  // nor further than this, well inside the ribbon (kAhead)
constexpr float kDefaultLead = 14.0f;  // with no ghost to run with
constexpr float kLeadRate    = 12.0f;  // metres a second the lead may change by
constexpr float kMaxFlight   = 12.0f;  // a flight higher than this is a bad sample, capped
constexpr float kHappyM      = 12.0f;  // happy for this many metres after a landing
constexpr float kHopM        = 0.08f;  // the run's hop
constexpr float kBlinkEvery  = 3.7f;   // seconds between blinks
constexpr float kBlinkFor    = 0.15f;

enum Face { IDLE, BLINK, HAPPY, WOW, SCARED };

/// Metres along Coach's line where Coach's lap was `t` seconds after the line (its own lap times,
/// `ref[i].t`), between the two points either side. NaN with no points or no arc for them.
inline float MetreAtTime(const std::vector<coachhud::RefPoint>& ref, const coachline::Arc& arc, float t) {
    const size_t n = ref.size();
    if (n < 2 || arc.m.size() != n || !std::isfinite(t)) return NAN;
    if (t <= ref[0].t) return arc.m[0];
    if (t >= ref[n - 1].t) return arc.m[n - 1];
    size_t lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        (ref[mid].t <= t ? lo : hi) = mid;
    }
    const float span = ref[hi].t - ref[lo].t;
    const float k    = span > 1e-6f ? (t - ref[lo].t) / span : 0.0f;
    return arc.m[lo] + (arc.m[hi] - arc.m[lo]) * k;
}

/// How far ahead Pip wants to be: where the ghost is, kept in [kMinLead, kMaxLead].
inline float LeadTarget(float ghost_m, float rider_m, float total) {
    if (!std::isfinite(ghost_m) || !std::isfinite(rider_m)) return kDefaultLead;
    const float d = coachline::WrapAhead(ghost_m, rider_m, total);
    return (std::max)(kMinLead, (std::min)(kMaxLead, d));
}

/// The lead as drawn: eased toward the target at kLeadRate, so it runs rather than jumps.
struct Follow {
    float lead = NAN;
    float step(float target, float dt) {
        if (!std::isfinite(target)) target = kDefaultLead;
        if (!std::isfinite(lead) || !(dt > 0.0f) || dt > 1.0f) return lead = target;
        const float most = kLeadRate * dt;
        lead += (std::max)(-most, (std::min)(most, target - lead));
        return lead;
    }
    void reset() { lead = NAN; }
};

/// What Pip is doing at lap metre `m`: its face and how high above the ground it is flying.
struct Pose {
    Face  face   = IDLE;
    float flight = 0;      // metres above the ground, from the jump's arc
    bool  air    = false;  // in a jump's flight
};

/// `m - from`, forward round the lap, in [0, total).
inline float Forward(float m, float from, float total) {
    float d = m - from;
    if (total > 0.0f) {
        d = std::fmod(d, total);
        if (d < 0) d += total;
    }
    return d;
}

inline Pose PoseAt(const std::vector<coachjump::Call>& calls, float m, float total, float time_s) {
    Pose p;
    if (std::isfinite(m)) {
        for (const coachjump::Call& c : calls) {
            if (c.kind == coachjump::ROLL) continue;
            const float len = c.landing_s - c.takeoff_s;
            if (!(len > 0.0f)) continue;
            const float in = Forward(m, c.takeoff_s, total);
            if (in <= len) {
                p.air  = true;
                p.face = c.kind == coachjump::TRIPLE || c.kind == coachjump::QUAD ? SCARED : WOW;
                if (!c.arc.empty()) {
                    const float  at = (std::min)(in, float(c.arc.size() - 1));
                    const size_t i  = size_t(at);
                    const size_t j  = (std::min)(i + 1, c.arc.size() - 1);
                    const float  h  = c.arc[i] + (c.arc[j] - c.arc[i]) * (at - float(i));
                    p.flight        = std::isfinite(h) ? (std::max)(0.0f, (std::min)(kMaxFlight, h)) : 0.0f;
                }
                return p;
            }
            if (in - len <= kHappyM) p.face = HAPPY;
        }
    }
    if (p.face == IDLE && std::isfinite(time_s) && std::fmod(time_s, kBlinkEvery) < kBlinkFor) p.face = BLINK;
    return p;
}

// ---------------------------------------------------------------------------------------
// The sprite: 12 x 12 pixels, row 0 at the top, facing the rider (it looks back over the line it
// is leading you down). One character per pixel, '.' for none.

constexpr int kW = 12, kH = 12;

inline const char* const* Body() {
    static const char* const rows[kH] = {
        ".....Y.Y....",  // the tuft
        "...YYYYYY...",
        "..YOOYYOOY..",  // the goggles, pushed up
        ".KKOOKKOOKK.",
        ".YYYYYYYYYY.",
        "YYEEYYYYEEYY",  // eyes and mouth: rows 5..8, replaced by the face
        "YYEEYYYYEEYY",
        "YYYYYMMYYYYY",
        "YYYLLLLLLYYY",
        ".YLLLLLLLLY.",
        "..YYYYYYYY..",
        "...F....F...",  // the feet: replaced while running
    };
    return rows;
}

/// Rows 5..8 for a face.
inline const char* const* FaceRows(Face f) {
    static const char* const idle[4]   = {"YYEEYYYYEEYY", "YYEEYYYYEEYY", "YYYYYMMYYYYY", "YYYLLLLLLYYY"};
    static const char* const blink[4]  = {"YYYYYYYYYYYY", "YEEEYYYYEEEY", "YYYYYMMYYYYY", "YYYLLLLLLYYY"};
    static const char* const happy[4]  = {"YYEYEYYEYEYY", "YPYYYYYYYYPY", "YYYYMMMMYYYY", "YYYLLMMLLYYY"};
    static const char* const wow[4]    = {"YEEEYYYYEEEY", "YEWEYYYYEWEY", "YYYYYMMYYYYY", "YYYLLMMLLYYY"};
    static const char* const scared[4] = {"YWWWYYYYWWWY", "YWEWYYYYWEWY", "YYYYYYYYYYYY", "YYYMLMLMLYYY"};
    switch (f) {
        case BLINK: return blink;
        case HAPPY: return happy;
        case WOW: return wow;
        case SCARED: return scared;
        default: return idle;
    }
}

/// The colour of a pixel character, or false for none.
inline bool Colour(char c, float rgba[4]) {
    uint32_t hex = 0;
    switch (c) {
        case 'Y': hex = 0xffc93c; break;  // body
        case 'L': hex = 0xffe48a; break;  // belly
        case 'K': hex = 0x2b2b2b; break;  // goggle strap
        case 'O': hex = 0xff7a1a; break;  // goggle lens
        case 'E': hex = 0x1b1b1b; break;  // eye
        case 'W': hex = 0xffffff; break;  // eye white
        case 'M': hex = 0x7a2230; break;  // mouth
        case 'P': hex = 0xff8a9a; break;  // cheek
        case 'F': hex = 0xe8761a; break;  // feet
        default: return false;
    }
    rgba[0] = float((hex >> 16) & 0xff) / 255.0f;
    rgba[1] = float((hex >> 8) & 0xff) / 255.0f;
    rgba[2] = float(hex & 0xff) / 255.0f;
    rgba[3] = 1.0f;
    return true;
}

/// The row of pixels at `r` for this face and stride.
inline const char* Row(int r, Face f, int stride) {
    if (r >= 5 && r <= 8) return FaceRows(f)[r - 5];
    if (r == kH - 1 && stride != 0) return stride > 0 ? "..F......F.." : "....F..F....";
    return Body()[r];
}

/// Pip standing on `base` (the middle of its feet), `right` the rider's right (a horizontal unit
/// vector), `h` tall. Each run of one colour in a row is one quad. `s` and `m` as coachmark::Quad.
inline void Sprite(std::vector<coachmark::Quad>& out, const float base[3], const float right[3], float h, Face f,
                   int stride, float s, float m) {
    const float px = h / float(kH), x0 = -0.5f * float(kW) * px;
    for (int r = 0; r < kH; ++r) {
        const char* row = Row(r, f, stride);
        const float y_lo = float(kH - 1 - r) * px, y_hi = y_lo + px;
        for (int c = 0; c < kW;) {
            float rgba[4];
            if (!Colour(row[c], rgba)) {
                ++c;
                continue;
            }
            int end = c;
            while (end < kW && row[end] == row[c]) ++end;
            const float a = x0 + float(c) * px, b = x0 + float(end) * px;
            coachmark::Quad q;
            const float xs[4] = {a, b, b, a}, ys[4] = {y_lo, y_lo, y_hi, y_hi};
            for (int k = 0; k < 4; ++k) {
                q.p[k][0] = base[0] + right[0] * xs[k];
                q.p[k][1] = base[1] + ys[k];
                q.p[k][2] = base[2] + right[2] * xs[k];
            }
            for (int k = 0; k < 4; ++k) q.rgba[k] = rgba[k];
            q.s = s;
            q.m = m;
            out.push_back(q);
            c = end;
        }
    }
}

/// Pip on the ribbon `lead` metres ahead of the rider: standing on it, or flying the jump it is
/// over. `rider_m` and `total` place it on Coach's line for the jump calls; `time_s` drives the
/// run and the blink. False (and nothing added) where the ribbon doesn't reach.
inline bool Build(std::vector<coachmark::Quad>& out, const std::vector<coachline::Vert>& ribbon,
                  const std::vector<coachjump::Call>& calls, float lead, float rider_m, float total, float time_s,
                  Face* face = nullptr) {
    const coachmark::Spot sp = coachmark::At(ribbon, lead);
    if (!sp.ok) return false;
    const float m = std::isfinite(rider_m) ? rider_m + lead : NAN;
    const float lap_m = std::isfinite(m) && total > 0.0f ? std::fmod(m, total) : m;
    const Pose  p = PoseAt(calls, lap_m, total, time_s);
    const float run = std::isfinite(time_s) ? time_s * 7.0f : 0.0f;  // seven strides a second
    const float hop = p.air ? 0.0f : kHopM * std::fabs(std::sin(run * 3.14159265f));
    const int   stride = p.air ? 0 : (std::fmod(run, 2.0f) < 1.0f ? 1 : -1);
    const float base[3]  = {sp.x, sp.y + p.flight + hop, sp.z};
    const float right[3] = {-sp.lx, 0.0f, -sp.lz};
    Sprite(out, base, right, kHeight, p.face, stride, lead, lap_m);
    if (face) *face = p.face;
    return true;
}

}  // namespace coachpet
