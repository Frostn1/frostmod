// petride.h - two small things about what FrostMod draws on track, kept pure so CI tests them.
//
// 1. When the panel may show. The corner pill ("Game Integration vX - F8"), the F8 menu and
//    every panel opened from it are for the pause screen, never for riding. Offline the game
//    calls RunStop when it pauses and RunTelemetry only while the simulation runs. Online it
//    keeps both going behind the Esc menu, so the game's page stack (track page on top or not)
//    and, for the Esc menu itself, two globals beside it decide there. Any one of them saying "not riding" is enough.
//
// 2. Pip, MXB App's pet, riding along. A 12 x 12 pixel chick (the same one as the app's corner
//    and Coach's line) drawn as solid quads through the plugin Draw() call, low in a corner of
//    the screen. It runs at the bike's speed, gasps in the air, is scared on a long flight or a
//    crash, cheers a landing and falls asleep when the bike sits still. The app turns it on or
//    off (`pet=` in frostmod_radar.cfg) and F8 > P moves it round the corners or hides it.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace petride {

// ---------------------------------------------------------------------------------------
// Riding or paused

/// RunTelemetry comes ten times a second while riding (Startup asks for rate 3); three missed
/// samples in a row and the simulation has stopped.
constexpr uint64_t kTelemetryFreshMs = 300;

/// Riding: the simulation was started and not stopped since, and a telemetry sample came in
/// the last kTelemetryFreshMs. `last_sample_ms` 0 means none yet.
inline bool Riding(bool running, uint64_t now_ms, uint64_t last_sample_ms) {
    if (!running || last_sample_ms == 0) return false;
    if (last_sample_ms >= now_ms) return true;   // stamped on another thread a moment later
    return now_ms - last_sample_ms <= kTelemetryFreshMs;
}

/// The game's own page stack (MX Bikes, offsets.h RVA_PAGE_*), top last. Riding shows the track
/// page ("mtrackm", or its other variant "mtrack") with nothing over it; the Esc menu, the pits,
/// chat or settings each put a page on top. Online the simulation keeps running behind the Esc
/// menu (no RunStop, telemetry keeps coming), so this is what tells the pause screen there.
inline bool TrackOnTop(const char* const* names, int n) {
    if (n <= 0 || !names[n - 1]) return false;
    const char* s = names[n - 1];
    return std::strcmp(s, "mtrackm") == 0 || std::strcmp(s, "mtrack") == 0;
}

/// The Esc menu over the track page (offsets.h RVA_UI_DEPTH / RVA_GAME_INPUT): the UI one level
/// deeper and the game no longer taking the rider's input. Both, so one odd value on its own
/// never puts the panel on screen while riding.
inline bool EscMenuOver(int ui_depth, int game_input) { return ui_depth >= 2 && game_input == 0; }

/// Riding, all signals together: `page` 1 / 0 from TrackOnTop, -1 when the stack can't be read
/// (another title, or before the game set it up), and then the telemetry alone decides.
inline bool Riding(int page, bool running, uint64_t now_ms, uint64_t last_sample_ms) {
    if (page == 0) return false;
    return Riding(running, now_ms, last_sample_ms);
}

// ---------------------------------------------------------------------------------------
// What the bike is doing, from the MX Bikes telemetry (SPluginsBikeData_t, data version 8)

constexpr int kOffSpeed    = 20;    // m_fSpeedometer, m/s
constexpr int kOffCrashed  = 136;   // m_iCrashed, 1 = rider off the bike
constexpr int kOffWheelMat = 168;   // m_aiWheelMaterial[2], 0 = wheel not touching anything
constexpr int kMinSize     = kOffWheelMat + 8;

struct Sample {
    float speed   = 0;      // m/s
    bool  air     = false;  // both wheels off the ground
    bool  crashed = false;
};

/// False when the payload is too short to hold the fields (another title, a cut struct).
inline bool Read(const void* data, int size, Sample* out) {
    if (!data || size < kMinSize || !out) return false;
    const char* p = (const char*)data;
    float sp; int cr, w0, w1;
    std::memcpy(&sp, p + kOffSpeed, 4);
    std::memcpy(&cr, p + kOffCrashed, 4);
    std::memcpy(&w0, p + kOffWheelMat, 4);
    std::memcpy(&w1, p + kOffWheelMat + 4, 4);
    out->speed   = std::isfinite(sp) ? std::fabs(sp) : 0.0f;
    out->crashed = cr != 0;
    out->air     = w0 == 0 && w1 == 0 && !out->crashed;
    return true;
}

// ---------------------------------------------------------------------------------------
// Pip's mood

enum Face { IDLE, BLINK, HAPPY, WOW, SCARED, SLEEP };

constexpr float kBigAirS     = 0.9f;   // longer than this in the air: scared, not amazed
constexpr float kLandedAirS  = 0.25f;  // a flight at least this long gets a cheer on landing
constexpr float kHappyS      = 1.2f;
constexpr float kSleepAfterS = 20.0f;  // sitting still this long: asleep
constexpr float kStillMs     = 0.6f;   // below this speed (m/s) the bike counts as still
constexpr float kBlinkEvery  = 3.7f;
constexpr float kBlinkFor    = 0.15f;

struct Mood {
    float t = 0;          // seconds Pip has been drawn (drives blink and run)
    float air_for = 0;
    float happy_for = 0;
    float still_for = 0;
    float run = 0;        // the run's phase, in strides
    Face  face = IDLE;
    int   stride = 0;     // -1 / +1 the feet, 0 standing
    float hop = 0;        // 0..1 of a pixel row's worth of bounce, x kHopRows
    float shake = 0;      // -1..1, sideways while crashed

    /// One drawn frame, `dt` seconds after the last. `riding` false (paused): Pip stands and
    /// blinks, and nothing about the bike changes its mood.
    void step(const Sample& s, bool riding, float dt) {
        if (!(dt > 0.0f) || dt > 0.5f) dt = 0.0f;   // first frame, or back from a long pause
        t += dt;
        stride = 0; hop = 0; shake = 0;
        if (!riding) {
            face = std::fmod(t, kBlinkEvery) < kBlinkFor ? BLINK : (still_for >= kSleepAfterS ? SLEEP : IDLE);
            return;
        }
        if (s.crashed) {
            air_for = 0; happy_for = 0; still_for = 0;
            face  = SCARED;
            shake = std::sin(t * 40.0f) >= 0.0f ? 1.0f : -1.0f;
            return;
        }
        if (s.air) {
            air_for += dt;
            still_for = 0;
            face = air_for > kBigAirS ? SCARED : WOW;
            hop  = 1.0f;                              // up with the bike
            return;
        }
        if (air_for >= kLandedAirS) happy_for = kHappyS;
        air_for = 0;
        if (happy_for > 0) happy_for -= dt;

        if (s.speed < kStillMs) still_for += dt; else still_for = 0;
        if (s.speed >= kStillMs) {
            // Faster bike, faster feet: one to eight strides a second.
            const float rate = std::fmax(1.0f, std::fmin(8.0f, s.speed / 3.0f));
            run += rate * dt;
            stride = std::fmod(run, 2.0f) < 1.0f ? 1 : -1;
            hop    = std::fabs(std::sin(run * 3.14159265f)) * std::fmin(1.0f, s.speed / 15.0f) * 0.5f;
        }
        if (happy_for > 0)                face = HAPPY;
        else if (still_for >= kSleepAfterS) face = SLEEP;
        else face = std::fmod(t, kBlinkEvery) < kBlinkFor ? BLINK : IDLE;
    }
};

// ---------------------------------------------------------------------------------------
// Where it sits: a corner of the screen, kept clear of the game's own HUD (standings and lap
// top-left, chat bottom-left). 0 hides it. F8 > P walks through them.

enum Corner { OFF = 0, BOTTOM_RIGHT = 1, BOTTOM_LEFT = 2, TOP_RIGHT = 3, kCorners = 4 };

inline Corner NextCorner(Corner c) { return Corner((int(c) + 1) % kCorners); }

inline const char* CornerName(Corner c) {
    switch (c) {
        case BOTTOM_RIGHT: return "bottom right";
        case BOTTOM_LEFT:  return "bottom left";
        case TOP_RIGHT:    return "top right";
        default:           return "off";
    }
}

// ---------------------------------------------------------------------------------------
// The sprite: 12 x 12 pixels, row 0 at the top. One character per pixel, '.' for none.

constexpr int   kW = 12, kH = 12;
constexpr float kHeight   = 0.075f;  // of the screen's height
constexpr float kMarginX  = 0.012f;  // from the screen's side, of its height (kept square)
constexpr float kMarginY  = 0.030f;
constexpr float kHopRows  = 2.0f;    // the highest bounce, in sprite pixels
constexpr float kShakeCols = 0.6f;

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

inline const char* const* FaceRows(Face f) {
    static const char* const idle[4]   = {"YYEEYYYYEEYY", "YYEEYYYYEEYY", "YYYYYMMYYYYY", "YYYLLLLLLYYY"};
    static const char* const blink[4]  = {"YYYYYYYYYYYY", "YEEEYYYYEEEY", "YYYYYMMYYYYY", "YYYLLLLLLYYY"};
    static const char* const happy[4]  = {"YYEYEYYEYEYY", "YPYYYYYYYYPY", "YYYYMMMMYYYY", "YYYLLMMLLYYY"};
    static const char* const wow[4]    = {"YEEEYYYYEEEY", "YEWEYYYYEWEY", "YYYYYMMYYYYY", "YYYLLMMLLYYY"};
    static const char* const scared[4] = {"YWWWYYYYWWWY", "YWEWYYYYWEWY", "YYYYYYYYYYYY", "YYYMLMLMLYYY"};
    static const char* const sleep[4]  = {"YYYYYYYYYYYY", "YEEEYYYYEEEY", "YYYYYYYYYYYY", "YYYLLMMLLYYY"};
    switch (f) {
        case BLINK:  return blink;
        case HAPPY:  return happy;
        case WOW:    return wow;
        case SCARED: return scared;
        case SLEEP:  return sleep;
        default:     return idle;
    }
}

/// 0xRRGGBB of a pixel character; false for none.
inline bool Colour(char c, uint32_t* rgb) {
    switch (c) {
        case 'Y': *rgb = 0xffc93c; return true;  // body
        case 'L': *rgb = 0xffe48a; return true;  // belly
        case 'K': *rgb = 0x2b2b2b; return true;  // goggle strap
        case 'O': *rgb = 0xff7a1a; return true;  // goggle lens
        case 'E': *rgb = 0x1b1b1b; return true;  // eye
        case 'W': *rgb = 0xffffff; return true;  // eye white
        case 'M': *rgb = 0x7a2230; return true;  // mouth
        case 'P': *rgb = 0xff8a9a; return true;  // cheek
        case 'F': *rgb = 0xe8761a; return true;  // feet
        default:  return false;
    }
}

inline const char* Row(int r, Face f, int stride) {
    if (r >= 5 && r <= 8) return FaceRows(f)[r - 5];
    if (r == kH - 1 && stride != 0) return stride > 0 ? "..F......F.." : "....F..F....";
    return Body()[r];
}

/// A screen rectangle in the plugin Draw() space: 0..1 both ways, top-left origin.
struct Quad {
    float x0, y0, x1, y1;
    uint32_t rgb;   // 0xRRGGBB, opaque
    float alpha;
};

/// Where the sprite's box goes: `aspect` is the screen's width over its height.
struct Box { float x0, y0, w, h; };

inline Box Place(Corner c, float aspect) {
    if (!(aspect > 0.2f && aspect < 10.0f)) aspect = 16.0f / 9.0f;
    Box b;
    b.h = kHeight;
    b.w = kHeight * float(kW) / float(kH) / aspect;
    const float mx = kMarginX / aspect;
    switch (c) {
        case BOTTOM_LEFT:  b.x0 = mx;              b.y0 = 1.0f - kMarginY - b.h; break;
        case TOP_RIGHT:    b.x0 = 1.0f - mx - b.w; b.y0 = kMarginY;              break;
        default:           b.x0 = 1.0f - mx - b.w; b.y0 = 1.0f - kMarginY - b.h; break;
    }
    return b;
}

/// Pip as quads, one per run of one colour in a row, plus a soft shadow under it. Nothing for
/// Corner OFF. The shadow stays on the ground while Pip hops.
inline void Build(std::vector<Quad>& out, Corner c, float aspect, const Mood& m) {
    if (c == OFF) return;
    const Box   b  = Place(c, aspect);
    const float pw = b.w / float(kW), ph = b.h / float(kH);
    const float lift = m.hop * kHopRows * ph;
    const float dx   = m.shake * kShakeCols * pw;

    const float sy = b.y0 + b.h - ph * 0.35f;
    out.push_back({b.x0 + 2 * pw, sy, b.x0 + 10 * pw, sy + ph * 0.7f, 0x000000, m.hop > 0.5f ? 0.18f : 0.30f});

    for (int r = 0; r < kH; ++r) {
        const char* row = Row(r, m.face, m.stride);
        const float y0 = b.y0 + float(r) * ph - lift, y1 = y0 + ph;
        for (int col = 0; col < kW;) {
            uint32_t rgb;
            if (!Colour(row[col], &rgb)) { ++col; continue; }
            int end = col;
            while (end < kW && row[end] == row[col]) ++end;
            out.push_back({b.x0 + float(col) * pw + dx, y0, b.x0 + float(end) * pw + dx, y1, rgb, 1.0f});
            col = end;
        }
    }
}

/// Where the sleeping "z z" goes, above Pip's head (top-left of the text).
inline void ZPos(Corner c, float aspect, float* x, float* y) {
    const Box b = Place(c, aspect);
    *x = b.x0 + b.w * 0.55f;
    *y = b.y0 - b.h * 0.35f;
}

}  // namespace petride
