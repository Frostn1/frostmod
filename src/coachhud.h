// coachhud.h - MXB Coach's in-game HUD in mxbcoach.dlo: the .hud sheet the app writes, hud.ini,
// the lap clock and the gap to Coach's lap, the track map, and where every part goes on screen.
//
// No Win32 here, so tests/coachhud_test.cpp runs anywhere. mxbcoach.cpp feeds it from the game's
// callbacks and copies the Frame it builds into the Draw buffers.
//
// Portions are adapted from MXBMRP3 (https://github.com/thomas4f/mxbmrp3), MIT licence,
//   Copyright (c) 2025-2026 thomas4f
// namely the centreline walk (hud/map_hud_internal.h advanceAlongArc, hud/map_hud.cpp
// updateTrackData / centerlinePositionAt / calculateTrackBounds, handlers/
// track_centerline_handler.cpp), the fit of the map into its box (hud/map_hud_geometry.cpp
// worldToScreen), the dot and line quads (hud/base_hud_primitives.cpp addDot / addLineSegment),
// the 1000-slot gap lookup (hud/gap_bar_hud.cpp calculateCurrentGap) and the lap restart on the
// wrap at the line (core/lap_timer.h). The full notice is in NOTICE.
//
// .hud file, MXHD v1, little-endian. MXB Coach writes it next to the .cue, named the same way:
//   0   char[4] "MXHD"
//   4   u32     version (1)
//   8   f32     track length m (a sheet for another length is ignored)
//   12  u32     N, reference-lap points (0..2000)
//   16  N x 16  f32 lap position 0..1, f32 elapsed s, f32 world x m, f32 world z m
//               (position and elapsed never decrease)
//   then u32    S, sections (0..64)
//       S x     f32 start m, f32 end m (0 <= start < end <= length, metres from the line),
//               u8 name length, name bytes, u8 tip length, tip bytes (ASCII; anything else
//               shows as '?')
//   then u32    flags: bit 0 = the setup card asks for a sag measurement
// Bytes after the flags are ignored.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "coachcue.h"
#include "stance.h"

namespace coachhud {

constexpr char     kMagic[4]    = {'M', 'X', 'H', 'D'};
constexpr uint32_t kVersion     = 1;
constexpr uint32_t kMaxPoints   = 2000;
constexpr uint32_t kMaxSections = 64;
constexpr uint32_t FLAG_SAG     = 1u << 0;

// The gap is looked up in one slot per 0.1% of the lap, as MXBMRP3's gap bar does.
constexpr int kSlots = 1000;

// Screen fractions are laid out for 16:9, like MXBMRP3's; horizontal sizes are divided by this
// so a square stays square.
constexpr float kAspect = 16.0f / 9.0f;

// The game has no text measuring. A character is about this many times the font size wide.
constexpr float kCharWidth = 0.275f;

struct RefPoint {
    float pos = 0, t = 0, x = 0, z = 0;
};

struct Section {
    float       start_m = 0, end_m = 0;
    std::string name, tip;
};

struct Sheet {
    float                 track_len = 0;
    std::vector<RefPoint> ref;
    std::vector<Section>  sections;  // by start
    uint32_t              flags = 0;
};

/// Text the game can draw: its strings are CP1252, so anything past ASCII becomes one '?' per
/// character (UTF-8 continuation bytes are dropped) and control characters a space.
inline std::string Ascii(const uint8_t* p, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = p[i];
        if (c >= 0x80 && c < 0xC0) continue;
        out += c >= 0xC0 || c == 0x7F ? '?' : c < 0x20 ? ' ' : char(c);
    }
    return out;
}

/// Reads a .hud file. Refuses anything that isn't what the app writes.
inline bool Parse(const uint8_t* b, size_t n, Sheet& out) {
    using coachcue::F32;
    using coachcue::U32;
    if (!b || n < 16 || std::memcmp(b, kMagic, 4) != 0 || U32(b + 4) != kVersion) return false;
    Sheet s;
    s.track_len = F32(b + 8);
    if (!std::isfinite(s.track_len) || s.track_len <= 0) return false;
    const uint32_t np = U32(b + 12);
    size_t at = 16;
    if (np > kMaxPoints || n - at < size_t(np) * 16) return false;
    for (uint32_t i = 0; i < np; ++i, at += 16) {
        RefPoint p{F32(b + at), F32(b + at + 4), F32(b + at + 8), F32(b + at + 12)};
        if (!std::isfinite(p.pos) || !std::isfinite(p.t) || !std::isfinite(p.x) || !std::isfinite(p.z)) return false;
        if (p.pos < 0 || p.pos > 1 || p.t < 0) return false;
        if (!s.ref.empty() && (p.pos < s.ref.back().pos || p.t < s.ref.back().t)) return false;
        s.ref.push_back(p);
    }
    if (n - at < 4) return false;
    const uint32_t ns = U32(b + at);
    at += 4;
    if (ns > kMaxSections) return false;
    for (uint32_t i = 0; i < ns; ++i) {
        if (n - at < 9) return false;
        Section sec;
        sec.start_m = F32(b + at);
        sec.end_m   = F32(b + at + 4);
        at += 8;
        const uint8_t nl = b[at++];
        if (n - at < size_t(nl) + 1) return false;
        sec.name = Ascii(b + at, nl);
        at += nl;
        const uint8_t tl = b[at++];
        if (n - at < tl) return false;
        sec.tip = Ascii(b + at, tl);
        at += tl;
        if (!std::isfinite(sec.start_m) || !std::isfinite(sec.end_m)) return false;
        if (sec.start_m < 0 || sec.end_m <= sec.start_m || sec.end_m > s.track_len + 1) return false;
        s.sections.push_back(std::move(sec));
    }
    if (n - at < 4) return false;
    s.flags = U32(b + at);
    std::stable_sort(s.sections.begin(), s.sections.end(),
                     [](const Section& x, const Section& y) { return x.start_m < y.start_m; });
    out = std::move(s);
    return true;
}

/// .hud files to try, best first: the .cue names with the other extension.
inline std::vector<std::string> HudNames(const coachcue::Event& e) {
    std::vector<std::string> out = coachcue::SheetNames(e);
    for (std::string& n : out) n = n.substr(0, n.size() - 4) + ".hud";
    return out;
}

/// A sheet belongs to a track only if it was made for one of the same length, as a .cue.
inline bool Fits(const Sheet& s, const coachcue::Event& e) {
    return e.track_len > 0 && std::fabs(s.track_len - e.track_len) <= 1.0f;
}

/// The section `m` metres from the line lies in, or null.
inline const Section* SectionAt(const Sheet& s, float m) {
    for (const Section& sec : s.sections)
        if (m >= sec.start_m && m < sec.end_m) return &sec;
    return nullptr;
}

// ---------------------------------------------------------------------------------------
// hud.ini

// Where the cue block sits by default: centred across, and low enough to clear MXBMRP3's
// panels. The rider can move it, because on their screen the default lands in the middle of
// where they are looking.
constexpr float kCueDefaultX = 0.5f;
constexpr float kCueDefaultY = 0.285f;

// The map and the suspension bars, by their top-left corner and their size. Bottom left and
// bottom right by default — opposite corners, so the two don't land on each other — but the
// corner is only where they start now, not where they are stuck.
constexpr float kMapW = 0.12f, kMapH = 0.21f;
constexpr float kMapDefaultX = 0.01f, kMapDefaultY = 0.77f;
constexpr float kSuspW = 0.13f, kSuspH = 0.085f;
constexpr float kSuspDefaultX = 0.86f, kSuspDefaultY = 0.86f;
// The gap to Coach's lap and sit/stand, on one line. Centred under the cue block by default.
// It is sized to its text, so only where it starts is remembered; the width follows what it
// has to say.
constexpr float kRowH = 0.025f;
constexpr float kRowDefaultX = 0.5f, kRowDefaultY = 0.365f;
/// The width the row is taken hold of at. Its drawn width follows its text; this is near
/// enough to grab, and keeps picking it up from depending on what it happens to say.
constexpr float kRowHitW = 0.16f;

struct Settings {
    bool enabled = true, cue = true, section = true, gap = true, stance = true, map = true, setup = true;
    // Off by default: they are additions, and a HUD that grows parts on its own after an
    // update is a worse surprise than one that waits to be asked.
    bool  susp = false, trail = false;
    float cue_x = kCueDefaultX;  // centre of the cue box, a screen fraction
    float cue_y = kCueDefaultY;  // its top edge
    // The map and the suspension bars by their top-left corner. They used to be nailed to the
    // corners they were first drawn in, which is the one thing every rider asked to change.
    float map_x  = kMapDefaultX, map_y = kMapDefaultY;
    float susp_x = kSuspDefaultX, susp_y = kSuspDefaultY;
    // The gap-and-stance line: `row_x` is its centre across, `row_y` its top.
    float row_x  = kRowDefaultX, row_y = kRowDefaultY;
    // Right-drag a part to move it. On by default - it is how the rider finds out they can.
    bool  move = true;
};

/// `<save>\mxbcoach\hud.ini`, [hud] key=1|0. Anything missing is on, except the map when
/// MXBMRP3 is installed (it draws its own) unless map=1 says otherwise, and the suspension
/// bars and the reference trail, which are off until asked for.
///
/// `cue_x` and `cue_y` are screen fractions, 0..1: the centre and the top of the cue box. The
/// section line follows it down, since it is the cue's second line and would otherwise be left
/// behind in the middle of the screen. A value that isn't a number, or one that would push the
/// box off screen, keeps the default rather than hiding the cue somewhere it can't be found.
inline Settings ParseSettings(const std::string& ini, bool mxbmrp3) {
    auto flag = [&](const char* key, bool def) {
        const std::string v = stance::IniValue(ini, "hud", key);
        return v == "1" ? true : v == "0" ? false : def;
    };
    auto fraction = [&](const char* key, float def) {
        const std::string v = stance::IniValue(ini, "hud", key);
        if (v.empty()) return def;
        char*       end = nullptr;
        const float f   = std::strtof(v.c_str(), &end);
        if (end == v.c_str() || *end != '\0' || !std::isfinite(f) || f < 0.0f || f > 1.0f) return def;
        return f;
    };
    Settings s;
    s.enabled = flag("enabled", true);
    s.cue     = flag("cue", true);
    s.section = flag("section", true);
    s.gap     = flag("gap", true);
    s.stance  = flag("stance", true);
    s.map     = flag("map", !mxbmrp3);
    s.setup   = flag("setup", true);
    s.susp    = flag("susp", false);
    s.trail   = flag("trail", false);
    s.cue_x   = fraction("cue_x", kCueDefaultX);
    s.cue_y   = fraction("cue_y", kCueDefaultY);
    s.map_x   = fraction("map_x", kMapDefaultX);
    s.map_y   = fraction("map_y", kMapDefaultY);
    s.susp_x  = fraction("susp_x", kSuspDefaultX);
    s.susp_y  = fraction("susp_y", kSuspDefaultY);
    s.row_x   = fraction("row_x", kRowDefaultX);
    s.row_y   = fraction("row_y", kRowDefaultY);
    s.move    = flag("move", true);
    return s;
}

// ---------------------------------------------------------------------------------------
// The lap and Coach's lap

/// Coach's reference lap: time by lap position, in 1000 slots, and world x/z by time.
class RefLap {
public:
    void load(const std::vector<RefPoint>& pts) {
        slots_.assign(kSlots, -1.0f);
        pts_.clear();
        if (pts.size() < 2) return;
        pts_ = pts;
        size_t j = 0;
        for (int i = 0; i < kSlots; ++i) {
            const float p = float(i) / float(kSlots);
            if (p < pts.front().pos || p > pts.back().pos) continue;
            while (j + 1 < pts.size() && pts[j + 1].pos < p) ++j;
            const RefPoint& a = pts[j];
            const RefPoint& c = pts[(std::min)(j + 1, pts.size() - 1)];
            const float span  = c.pos - a.pos;
            const float f     = span > 0 ? (p - a.pos) / span : 0.0f;
            slots_[size_t(i)] = a.t + f * (c.t - a.t);
        }
    }
    bool ready() const { return !pts_.empty(); }

    /// Coach's points as the sheet gave them, for drawing the line to take. Empty without a
    /// sheet, which is what stops the trail being guessed from anything else.
    const std::vector<RefPoint>& points() const { return pts_; }

    /// Coach's elapsed time at lap position `pos`, as MXBMRP3's calculateCurrentGap reads it.
    bool time_at(float pos, float& out) const {
        if (!ready()) return false;
        const float exact = pos * float(kSlots);
        int lo            = int(exact);
        int hi            = lo + 1;
        const float f     = exact - float(lo);
        lo = (std::max)(0, (std::min)(lo, kSlots - 1));
        hi = (std::max)(0, (std::min)(hi, kSlots - 1));
        const float a = slots_[size_t(lo)], c = slots_[size_t(hi)];
        if (a >= 0 && c >= 0) out = a + f * (c - a);
        else if (a >= 0) out = a;
        else if (c >= 0) out = c;
        else {
            for (int k = 1; k < 10; ++k)
                if (lo - k >= 0 && slots_[size_t(lo - k)] >= 0) {
                    out = slots_[size_t(lo - k)];
                    return true;
                }
            return false;
        }
        return true;
    }

    /// Where Coach was `t` seconds into the lap. False before its first point or after its last.
    bool world_at(float t, float& x, float& z) const {
        if (!ready() || t < pts_.front().t || t > pts_.back().t) return false;
        const auto it = std::upper_bound(pts_.begin(), pts_.end(), t,
                                         [](float v, const RefPoint& p) { return v < p.t; });
        const RefPoint& c = it == pts_.end() ? pts_.back() : *it;
        const RefPoint& a = it == pts_.begin() ? c : *(it - 1);
        const float span  = c.t - a.t;
        const float f     = span > 0 ? (t - a.t) / span : 0.0f;
        x = a.x + f * (c.x - a.x);
        z = a.z + f * (c.z - a.z);
        return true;
    }

private:
    std::vector<float>    slots_;
    std::vector<RefPoint> pts_;
};

/// The rider's lap, on track time (RunTelemetry's _fTime, which stops while paused). It starts
/// when the lap position wraps forwards at the line, as MXBMRP3's lap timer does, at the moment
/// the line was crossed between the two samples. Joining mid-lap, riding back over the line, or
/// time going backwards leaves it unset until the next crossing.
class LapClock {
public:
    void reset() {
        last_pos_ = -1;
        valid_    = false;
    }
    void on_sample(float t, float pos) {
        if (last_pos_ >= 0) {
            if (t < last_t_) valid_ = false;
            if (pos + 0.5f < last_pos_) {
                const float before = 1.0f - last_pos_, span = before + pos;
                start_ = last_t_ + (span > 0 ? (t - last_t_) * before / span : 0.0f);
                valid_ = true;
            } else if (last_pos_ + 0.5f < pos) {
                valid_ = false;
            }
        }
        last_pos_ = pos;
        last_t_   = t;
    }
    bool  valid() const { return valid_; }
    float elapsed(float t) const { return t - start_; }

private:
    float last_pos_ = -1, last_t_ = 0, start_ = 0;
    bool  valid_    = false;
};

/// Seconds behind Coach's lap at this point (negative: ahead).
inline bool Gap(const RefLap& ref, const LapClock& clock, float t, float pos, float& gap) {
    float rt;
    if (!clock.valid() || !ref.time_at(pos, rt)) return false;
    gap = clock.elapsed(t) - rt;
    return std::isfinite(gap);
}

/// Stopped: under 0.5 m/s for over a second of track time.
class StopWatch {
public:
    void reset() { since_ = -1; }
    void on_sample(float t, float speed) {
        now_ = t;
        if (speed >= 0.5f) since_ = -1;
        else if (since_ < 0 || t < since_) since_ = t;
    }
    bool stopped() const { return since_ >= 0 && now_ - since_ > 1.0f; }

private:
    float since_ = -1, now_ = 0;
};

/// The setup file name from RunInit's SPluginsBikeSession_t (char[100] at 12), without a folder.
inline std::string SetupName(const void* data, int size) {
    if (!data || size <= 12) return {};
    std::string s = coachcue::Field(static_cast<const uint8_t*>(data), size_t(size), 12, 100);
    const size_t slash = s.find_last_of("\\/");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    return Ascii(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// ---------------------------------------------------------------------------------------
// The track map

/// Moves (x, y, heading degrees, 0 = north) `dist` metres along an arc of signed `radius`, 0 for
/// a straight. Exact, so however finely it is sampled it stays on the line. From MXBMRP3.
inline void AdvanceAlongArc(float& x, float& y, float& heading, float radius, float dist) {
    const float h0 = heading * 0.017453292f;
    if (std::fabs(radius) < 0.01f) {
        x += std::sin(h0) * dist;
        y += std::cos(h0) * dist;
        return;
    }
    const float theta = dist / radius;
    x += radius * (std::cos(h0) - std::cos(h0 + theta));
    y += radius * (std::sin(h0 + theta) - std::sin(h0));
    heading += theta * 57.29578f;
}

struct Pt {
    float x = 0, y = 0;
};

/// The centreline from TrackCenterline, as world x/z (the centreline's y is the world's z, north).
class Track {
public:
    static constexpr int kLineSegments = 300;
    static constexpr int kMaxSegments  = 20000;

    /// `segs`: SPluginsTrackSegment_t records `stride` bytes apart (i32 type, f32 length,
    /// f32 radius, f32 start angle, f32 start[2], f32 height). `race`: the race-data floats,
    /// the line's distance along the centreline first, or null.
    bool build(int count, const void* segs, int stride, const float* race) {
        clear();
        if (!segs || count <= 0 || count > kMaxSegments || stride < 24) return false;
        const auto* b = static_cast<const uint8_t*>(segs);
        float x = coachcue::F32(b + 16), y = coachcue::F32(b + 20), h = coachcue::F32(b + 12);
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(h)) return false;
        float total = 0;
        for (int i = 0; i < count; ++i) {
            const uint8_t* r = b + size_t(i) * size_t(stride);
            Seg s;
            s.len    = coachcue::F32(r + 4);
            s.radius = coachcue::U32(r) == 0 ? 0.0f : coachcue::F32(r + 8);
            if (!std::isfinite(s.len) || s.len < 0 || !std::isfinite(s.radius)) return clear(), false;
            s.x = x, s.y = y, s.heading = h, s.from = total;
            AdvanceAlongArc(x, y, h, s.radius, s.len);
            total += s.len;
            segs_.push_back(s);
        }
        if (!(total > 0)) return clear(), false;
        total_ = total;
        sf_    = race && std::isfinite(race[0]) && race[0] > 0 && race[0] <= total ? race[0] : 0.0f;
        line_.reserve(kLineSegments + 1);
        for (int k = 0; k <= kLineSegments; ++k) {
            Pt p;
            centreline_at(total * float(k) / float(kLineSegments), p.x, p.y);
            line_.push_back(p);
        }
        min_ = max_ = line_[0];
        for (const Pt& p : line_) {
            min_.x = (std::min)(min_.x, p.x), min_.y = (std::min)(min_.y, p.y);
            max_.x = (std::max)(max_.x, p.x), max_.y = (std::max)(max_.y, p.y);
        }
        const float px = (max_.x - min_.x) * 0.05f, py = (max_.y - min_.y) * 0.05f;
        min_.x -= px, max_.x += px, min_.y -= py, max_.y += py;
        return true;
    }
    void clear() {
        segs_.clear();
        line_.clear();
        total_ = sf_ = 0;
    }
    bool  ready() const { return !line_.empty(); }
    float length() const { return total_; }
    float start_line() const { return sf_; }
    const std::vector<Pt>& line() const { return line_; }
    Pt lo() const { return min_; }
    Pt hi() const { return max_; }

    /// World x/z `m` metres past the line (a lap position times the length).
    bool at(float m, float& x, float& z) const {
        if (!ready() || !std::isfinite(m)) return false;
        float c = std::fmod(sf_ + m, total_);
        if (c < 0) c += total_;
        centreline_at(c, x, z);
        return true;
    }

private:
    struct Seg {
        float len = 0, radius = 0, x = 0, y = 0, heading = 0, from = 0;
    };
    void centreline_at(float c, float& x, float& y) const {
        auto it = std::upper_bound(segs_.begin(), segs_.end(), c, [](float v, const Seg& s) { return v < s.from; });
        const Seg& s = *(it == segs_.begin() ? it : it - 1);
        float h      = s.heading;
        x = s.x, y = s.y;
        AdvanceAlongArc(x, y, h, s.radius, (std::min)(c - s.from, s.len));
    }

    std::vector<Seg> segs_;
    std::vector<Pt>  line_;
    float            total_ = 0, sf_ = 0;
    Pt               min_, max_;
};

struct Box {
    float x0, y0, x1, y1;
};

/// World x/z to the screen, the whole track fitted into `box`, square after aspect correction,
/// north up. After MXBMRP3's worldToScreen.
inline Pt Project(const Track& t, const Box& box, float wx, float wz) {
    const float w = (std::max)(t.hi().x - t.lo().x, 1e-3f), h = (std::max)(t.hi().y - t.lo().y, 1e-3f);
    const float s = (std::min)((box.x1 - box.x0) * kAspect / w, (box.y1 - box.y0) / h);
    Pt p;
    p.x = (box.x0 + box.x1) * 0.5f + (wx - (t.lo().x + t.hi().x) * 0.5f) * s / kAspect;
    p.y = (box.y0 + box.y1) * 0.5f - (wz - (t.lo().y + t.hi().y) * 0.5f) * s;
    return p;
}

/// The next `max` cue spots ahead of `m`, wrapping past the line, nearest first.
inline std::vector<float> Upcoming(const coachcue::Sheet& s, float m, size_t max) {
    std::vector<float> out;
    const size_t n = s.cues.size();
    size_t first   = 0;
    while (first < n && s.cues[first].at_m <= m) ++first;
    for (size_t k = 0; k < n && out.size() < max; ++k) out.push_back(s.cues[(first + k) % n].at_m);
    return out;
}

// ---------------------------------------------------------------------------------------
// The frame

struct Quad {
    float    p[4][2] = {};  // counter-clockwise from the top left
    int      sprite  = 0;   // 0 = solid; sprites are 1-based
    uint32_t color   = 0;   // ABGR
};
struct Text {
    std::string s;
    float       x = 0, y = 0, size = 0;  // y is the top of the line
    int         justify = 0;             // 0 left, 1 centre, 2 right
    uint32_t    color   = 0;
};
// Room for the map's centreline segments plus the reference trail, the suspension bars and
// every marker on top of them.
//
// The cap used to be reachable: the centreline alone spent 301, a dense trail up to 240 more,
// and the rider's arrow and the ghost are drawn last — so with the trail on, the first thing
// to be dropped was the rider's own marker. Two things stop that now. The line and the trail
// are thinned to what a map this size can show (`kLineOnMap`, `kTrailSegs`), and the last
// slots are held back: `Frame::room` refuses ordinary drawing once the reserve is all that is
// left, and `Frame::reserved` spends it for the markers that must never go missing.
constexpr size_t kMaxQuads = 512;
constexpr size_t kReserve  = 12;
constexpr size_t kMaxTexts = 16;
constexpr size_t kMaxChars = 99;  // SPluginString_t holds 100 bytes

struct Frame {
    std::vector<Quad> quads;
    std::vector<Text> texts;
    /// True while the reserve is being spent, for the handful of markers that must draw.
    bool reserved = false;
    /// Whether one more quad may be drawn.
    bool room() const { return quads.size() < (reserved ? kMaxQuads : kMaxQuads - kReserve); }
    void clear() {
        quads.clear();
        texts.clear();
        reserved = false;
    }
};

/// Spends the reserve for as long as it is in scope.
struct Reserved {
    Frame& f;
    explicit Reserved(Frame& frame) : f(frame) { f.reserved = true; }
    ~Reserved() { f.reserved = false; }
};

// Where everything goes, as screen fractions (16:9). Clear of MXBMRP3's Notices (y 0.099-0.165)
// and Timing (y 0.187-0.253) panels.
// The cue block. kCueBox and kSectionBox are where it sits with the default cue_x / cue_y, and
// what the rest of the layout was measured against; CueBoxAt and SectionBoxAt move it to
// wherever hud.ini puts it.
constexpr float kCueWidth      = 0.30f;
constexpr float kCueHeight     = 0.05f;
constexpr float kCueGap        = 0.003f;  // between the cue and the section line under it
constexpr float kSectionHeight = 0.024f;
constexpr Box   kCueBox     = {0.35f, 0.285f, 0.65f, 0.335f};
constexpr float kCueSize    = 0.040f;
constexpr Box   kSectionBox = {0.35f, 0.338f, 0.65f, 0.362f};
constexpr float kSmallSize  = 0.022f;
constexpr Box   kMapBox  = {kMapDefaultX, kMapDefaultY, kMapDefaultX + kMapW, kMapDefaultY + kMapH};
constexpr Box   kCardBox = {0.35f, 0.60f, 0.65f, 0.72f};
constexpr Box   kSuspBox = {kSuspDefaultX, kSuspDefaultY, kSuspDefaultX + kSuspW, kSuspDefaultY + kSuspH};
constexpr float kSuspLabelW = 0.022f;  // room for the F / R label at the left of each bar
constexpr float kSuspPad    = 0.006f;

/// How far ahead of the rider Coach's line is drawn, as a fraction of the lap.
constexpr float kTrailLap = 0.12f;
/// How many segments the centreline and the trail are worth drawing on a map this small.
constexpr size_t kLineOnMap = 120;
constexpr size_t kTrailSegs = 40;

/// The cue box for a `cue_x` (its centre) and `cue_y` (its top), kept wholly on screen along
/// with the section line beneath it. A rider who drags it to the edge gets it at the edge, not
/// half off the screen where the text can't be read.
inline Box CueBoxAt(float cx, float cy) {
    const float half = kCueWidth * 0.5f;
    const float x    = (std::max)(half, (std::min)(cx, 1.0f - half));
    const float tall = kCueHeight + kCueGap + kSectionHeight;
    const float y    = (std::max)(0.0f, (std::min)(cy, 1.0f - tall));
    return {x - half, y, x + half, y + kCueHeight};
}

/// A part of fixed size at the top-left the rider put it, kept wholly on screen. A part
/// dragged past the edge sits against the edge rather than half out of sight.
inline Box BoxAt(float x, float y, float w, float h) {
    const float px = (std::max)(0.0f, (std::min)(x, 1.0f - w));
    const float py = (std::max)(0.0f, (std::min)(y, 1.0f - h));
    return {px, py, px + w, py + h};
}

inline Box MapBoxAt(const Settings& s) { return BoxAt(s.map_x, s.map_y, kMapW, kMapH); }

/// The gap-and-stance line, `text_w` wide, centred on `row_x` and kept on screen. Its width
/// depends on what it says, so unlike the rest it is measured each frame rather than fixed.
inline Box RowBoxAt(const Settings& s, float text_w) {
    const float w = text_w + 0.02f;
    return BoxAt(s.row_x - w * 0.5f, s.row_y, w, kRowH);
}
inline Box SuspBoxAt(const Settings& s) { return BoxAt(s.susp_x, s.susp_y, kSuspW, kSuspH); }

// ---------------------------------------------------------------------------------------
// Moving a part with the mouse
//
// The plugin API has no input callback and no cursor, so the drag itself is read from Win32 in
// mxbcoach.cpp. Everything that can be decided without Windows lives here, where it is tested:
// which part is under a point, where a part lands when it is dragged, and how hud.ini is
// rewritten so Coach's own keys survive.

enum Part { PART_NONE = 0, PART_CUE, PART_MAP, PART_SUSP, PART_ROW };

/// A part's name, for the log: a rider who can't move something wants to know whether the
/// plugin ever saw the grab.
inline const char* PartName(Part p) {
    switch (p) {
        case PART_CUE: return "the cue";
        case PART_MAP: return "the map";
        case PART_SUSP: return "the suspension bars";
        case PART_ROW: return "the gap line";
        default: return "nothing";
    }
}

/// The box a part occupies now. `PART_NONE`, or a part that isn't drawn, has none.
inline bool PartBox(const Settings& s, Part part, Box& out) {
    switch (part) {
        case PART_CUE:
            if (!s.cue) return false;
            out = CueBoxAt(s.cue_x, s.cue_y);
            return true;
        case PART_MAP:
            if (!s.map) return false;
            out = MapBoxAt(s);
            return true;
        case PART_SUSP:
            if (!s.susp) return false;
            out = SuspBoxAt(s);
            return true;
        case PART_ROW:
            // Grabbable whenever either half of it is on. Its real width follows its text, so
            // a nominal one is used for the hit test — near enough to take hold of.
            if (!s.gap && !s.stance) return false;
            out = RowBoxAt(s, kRowHitW);
            return true;
        default: return false;
    }
}

/// Which drawn part is under (x, y), or `PART_NONE`. The cue is tested first: it is the
/// smallest, and the one most likely to be sitting over something else.
inline Part PartAt(const Settings& s, float x, float y) {
    for (Part p : {PART_CUE, PART_ROW, PART_SUSP, PART_MAP}) {
        Box b;
        if (!PartBox(s, p, b)) continue;
        if (x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1) return p;
    }
    return PART_NONE;
}

/// Put a part's top-left corner at (x, y). The cue is stored by the centre of its box, so it
/// is converted; the clamping that keeps a part on screen belongs to the readers, so a value
/// written here is the one the rider dragged to.
inline void SetPartOrigin(Settings& s, Part part, float x, float y) {
    switch (part) {
        case PART_CUE: s.cue_x = x + kCueWidth * 0.5f, s.cue_y = y; break;
        case PART_MAP: s.map_x = x, s.map_y = y; break;
        case PART_SUSP: s.susp_x = x, s.susp_y = y; break;
        // Stored by its centre, like the cue box, and grabbed at the nominal width the hit
        // test uses — its real width changes with the text.
        case PART_ROW: s.row_x = x + (kRowHitW + 0.02f) * 0.5f, s.row_y = y; break;
        default: break;
    }
}

/// The `[hud]` keys a part's position is written under, and their values, to three decimals.
inline std::vector<std::pair<std::string, std::string>> PartKeys(const Settings& s, Part part) {
    char buf[32];
    auto num = [&buf](float v) {
        std::snprintf(buf, sizeof(buf), "%.3f", double(v));
        return std::string(buf);
    };
    switch (part) {
        case PART_CUE: return {{"cue_x", num(s.cue_x)}, {"cue_y", num(s.cue_y)}};
        case PART_MAP: return {{"map_x", num(s.map_x)}, {"map_y", num(s.map_y)}};
        case PART_SUSP: return {{"susp_x", num(s.susp_x)}, {"susp_y", num(s.susp_y)}};
        case PART_ROW: return {{"row_x", num(s.row_x)}, {"row_y", num(s.row_y)}};
        default: return {};
    }
}

/// `ini` with each key set in `[hud]`, every other line kept as it was. Coach writes this file
/// too, so a key it owns must survive the rider dragging something across the screen.
inline std::string WithHudKeys(const std::string& ini, const std::vector<std::pair<std::string, std::string>>& keys) {
    if (keys.empty()) return ini;
    std::vector<std::string> lines;
    std::string              cur;
    for (char c : ini) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    lines.push_back(cur);
    auto trimmed = [](const std::string& l) {
        size_t a = l.find_first_not_of(" \t");
        if (a == std::string::npos) return std::string();
        size_t b = l.find_last_not_of(" \t");
        return l.substr(a, b - a + 1);
    };
    // Rewrite in place wherever the key is already there, inside [hud].
    std::vector<bool> done(keys.size(), false);
    bool              in_hud = false;
    size_t            hud_end = std::string::npos;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trimmed(lines[i]);
        if (!t.empty() && t[0] == '[') {
            if (in_hud) hud_end = i;
            in_hud = t == "[hud]";
            continue;
        }
        if (!in_hud) continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string name = trimmed(t.substr(0, eq));
        for (size_t k = 0; k < keys.size(); ++k) {
            if (done[k] || name != keys[k].first) continue;
            lines[i] = keys[k].first + "=" + keys[k].second;
            done[k]  = true;
        }
    }
    if (in_hud) hud_end = lines.size();
    // Anything that wasn't there goes in at the end of [hud], or in a new section.
    std::vector<std::string> add;
    for (size_t k = 0; k < keys.size(); ++k)
        if (!done[k]) add.push_back(keys[k].first + "=" + keys[k].second);
    if (!add.empty()) {
        if (hud_end == std::string::npos) {
            if (!lines.empty() && !trimmed(lines.back()).empty()) lines.push_back("");
            lines.push_back("[hud]");
            hud_end = lines.size();
        }
        lines.insert(lines.begin() + long(hud_end), add.begin(), add.end());
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size()) out += "\n";
    }
    return out;
}

/// The section line, directly under the cue box: it is the cue's second line, so it travels
/// with it rather than staying behind in the middle of the screen.
inline Box SectionBoxAt(const Box& cue) {
    return {cue.x0, cue.y1 + kCueGap, cue.x1, cue.y1 + kCueGap + kSectionHeight};
}

constexpr uint32_t kBacking = 0xA0000000u;
constexpr uint32_t kWhite   = 0xFFFFFFFFu;
constexpr uint32_t kRed     = 0xFF4B4BFFu;
constexpr uint32_t kGreen   = 0xFF5CD65Cu;
constexpr uint32_t kAmber   = 0xFF5CD6FFu;
constexpr uint32_t kGrey    = 0xFFB4B4B4u;
constexpr uint32_t kBlue    = 0xFFFFC04Bu;

inline uint32_t CueColour(uint8_t kind) {
    switch (kind) {
        case coachcue::BRAKE: return kRed;
        case coachcue::THROTTLE: return kGreen;
        case coachcue::OFF_BRAKES: return kAmber;
        default: return kWhite;
    }
}

inline float TextWidth(size_t chars, float size) { return float(chars) * kCharWidth * size; }

/// `s` cut to what fits `width` at `size` (and the game's 99 bytes), with "..." when cut.
inline std::string Fit(const std::string& s, float size, float width) {
    const size_t max = (std::min)(kMaxChars, size_t(width / (kCharWidth * size)));
    if (s.size() <= max) return s;
    return max > 3 ? s.substr(0, max - 3) + "..." : s.substr(0, max);
}

inline void Rect(Frame& f, float x0, float y0, float x1, float y1, uint32_t color) {
    if (!f.room()) return;
    Quad q;
    q.p[0][0] = x0, q.p[0][1] = y0;
    q.p[1][0] = x0, q.p[1][1] = y1;
    q.p[2][0] = x1, q.p[2][1] = y1;
    q.p[3][0] = x1, q.p[3][1] = y0;
    q.color = color;
    f.quads.push_back(q);
}

/// A square `size` tall centred on (x, y). MXBMRP3's addDot.
inline void Dot(Frame& f, float x, float y, uint32_t color, float size) {
    const float hx = size * 0.5f / kAspect, hy = size * 0.5f;
    Rect(f, x - hx, y - hy, x + hx, y + hy, color);
}

/// An arrow `size` long centred on (x, y), pointing along the screen direction (dx, dy).
///
/// The rider was a square, which says where they are and nothing else — on a map whose whole
/// job is which way the next corner goes, that is half the information missing. A quad takes
/// four free corners, so the head is one quad with its two back corners brought together into
/// a triangle and the tail is a second: no sprite, no new API, two quads.
///
/// (dx, dy) is already in screen space, so a caller projecting a world heading must invert z
/// the way `Project` does. A direction too short to normalise falls back to a dot, which is
/// what a stationary bike should look like anyway.
inline void Arrow(Frame& f, float x, float y, float dx, float dy, uint32_t color, float size) {
    const float len = std::sqrt(dx * dx + dy * dy);
    if (!(len > 1e-6f)) return Dot(f, x, y, color, size * 0.6f);
    // Forward, and the perpendicular to it, both in screen units with the aspect taken out of
    // x so the arrow stays the same shape rather than stretching with the screen.
    const float fx = dx / len, fy = dy / len;
    const float half = size * 0.5f;
    const float tipx = x + fx * half / kAspect, tipy = y + fy * half;
    const float bakx = x - fx * half / kAspect, baky = y - fy * half;
    const float wx = -fy * size * 0.34f / kAspect, wy = fx * size * 0.34f;
    if (f.room()) {
        Quad q;
        q.p[0][0] = tipx, q.p[0][1] = tipy;          // the point
        q.p[1][0] = bakx + wx, q.p[1][1] = baky + wy;  // one back corner
        q.p[2][0] = bakx - wx, q.p[2][1] = baky - wy;  // the other
        q.p[3][0] = tipx, q.p[3][1] = tipy;            // folded onto the point: a triangle
        q.color = color;
        f.quads.push_back(q);
    }
}

/// The rider's own pointer, as a triangle with a dark edge under it.
///
/// Drawn rather than relied upon: on track the game hides the system cursor, so a rider
/// dragging a part would be aiming something they cannot see. MXBMRP3 draws its own pointer
/// widget for the same reason. Two quads, no sprite.
inline void Pointer(Frame& f, float x, float y) {
    const float w = 0.011f / kAspect, h = 0.030f;
    auto tri = [&f](float px, float py, float pw, float ph, uint32_t color) {
        if (!f.room()) return;
        Quad q;
        q.p[0][0] = px, q.p[0][1] = py;                        // the point
        q.p[1][0] = px, q.p[1][1] = py + ph;                   // straight down
        q.p[2][0] = px + pw, q.p[2][1] = py + ph * 0.72f;      // and back up to the right
        q.p[3][0] = px, q.p[3][1] = py;                        // folded onto the point
        q.color = color;
        f.quads.push_back(q);
    };
    // The shadow first and a shade larger, so the pointer reads over pale ground as well as
    // dark. Over a track map it would otherwise vanish into the grey.
    tri(x - 0.0015f / kAspect, y - 0.0015f, w * 1.28f, h * 1.18f, 0xC0000000u);
    tri(x, y, w, h, kWhite);
}

/// A line `thickness` thick. MXBMRP3's addLineSegment.
inline void Line(Frame& f, float x1, float y1, float x2, float y2, uint32_t color, float thickness) {
    const float dx = x2 - x1, dy = y2 - y1, len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.0001f || !f.room()) return;
    const float hx = dy / len * thickness * 0.5f / kAspect, hy = -dx / len * thickness * 0.5f;
    Quad q;
    q.p[0][0] = x1 + hx, q.p[0][1] = y1 + hy;
    q.p[1][0] = x1 - hx, q.p[1][1] = y1 - hy;
    q.p[2][0] = x2 - hx, q.p[2][1] = y2 - hy;
    q.p[3][0] = x2 + hx, q.p[3][1] = y2 + hy;
    q.color = color;
    f.quads.push_back(q);
}

inline void Say(Frame& f, std::string s, float x, float y, float size, int justify, uint32_t color) {
    if (f.texts.size() >= kMaxTexts || s.empty()) return;
    Text t;
    t.s = s.size() > kMaxChars ? s.substr(0, kMaxChars) : std::move(s);
    t.x = x, t.y = y, t.size = size, t.justify = justify, t.color = color;
    f.texts.push_back(std::move(t));
}

/// Everything the frame shows, gathered under the plugin's lock.
struct View {
    Settings                 set;
    const coachcue::Cue*     cue     = nullptr;
    const Section*           section = nullptr;
    bool                     has_gap = false;
    float                    gap     = 0;
    stance::State            stance  = stance::STANCE_UNKNOWN;
    stance::Confidence       conf    = stance::CONF_NONE;
    const Track*             track   = nullptr;
    bool                     has_rider = false, has_ghost = false;
    Pt                       rider, ghost;  // world x/z
    // Which way the rider is pointing, as a world x/z direction. Zero until they have moved
    // far enough to say, and then the arrow on the map points along it.
    Pt                       rider_dir{};
    // Where the rider's mouse is, while it is worth showing: they have moved it recently, or
    // they are holding a part. Off, and nothing is drawn.
    bool                     has_pointer = false;
    Pt                       pointer{};
    float                    pos = 0;       // the rider's lap position 0..1, for the trail
    std::vector<float>       upcoming;      // cue spots ahead, metres from the line
    bool                     stopped = false;
    std::string              setup;
    bool                     sag = false;
    // Coach's lap, for the blue line ahead of the rider. Null, or empty, without a sheet - and
    // then no trail is drawn at all, rather than one guessed from the centreline.
    const std::vector<RefPoint>* ref = nullptr;
    // How much suspension travel each end is using now, 0..1, and the deepest it has reached
    // this stint. False when the bike hasn't said (the event carried no max travel).
    bool                     has_susp    = false;
    float                    susp[2]     = {0, 0};  // 0 = front, 1 = rear
    float                    susp_max[2] = {0, 0};
};

/// How much of an end's travel is in use, given the shock's current length and its maximum
/// travel: 0 fully extended, 1 fully compressed. From MXBMRP3's updateSuspensionLength
/// (core/plugin_data_telemetry.cpp), which is where the direction of m_afSuspLength is
/// settled - the published header only says "shocks length", which does not say which way.
inline float SuspUsed(float length, float max_travel) {
    if (!(max_travel > 0) || !std::isfinite(length) || !std::isfinite(max_travel)) return 0.0f;
    return (std::max)(0.0f, (std::min)(1.0f, (max_travel - length) / max_travel));
}

/// "vs Coach +0.34"
inline std::string GapText(float gap) {
    char s[32];
    std::snprintf(s, sizeof(s), "vs Coach %+.2f", double(gap));
    return s;
}

inline void Build(const View& v, Frame& f) {
    f.clear();
    if (!v.set.enabled) return;

    // 1. The cue, wherever the rider has put the block.
    const Box cue_box = CueBoxAt(v.set.cue_x, v.set.cue_y);
    const float cue_mid = (cue_box.x0 + cue_box.x1) * 0.5f;
    if (v.set.cue && v.cue) {
        const Box& b = cue_box;
        Rect(f, b.x0, b.y0, b.x1, b.y1, kBacking);
        Say(f, Fit(v.cue->text, kCueSize, b.x1 - b.x0), cue_mid, b.y0 + (b.y1 - b.y0 - kCueSize) * 0.5f, kCueSize, 1,
            CueColour(v.cue->kind));
    }

    // 2. The section and its tip, below it.
    if (v.set.section && v.section) {
        const Box b = SectionBoxAt(cue_box);
        std::string s = v.section->name;
        if (!v.section->tip.empty()) s += s.empty() ? v.section->tip : ": " + v.section->tip;
        Rect(f, b.x0, b.y0, b.x1, b.y1, kBacking);
        Say(f, Fit(s, kSmallSize, b.x1 - b.x0 - 0.01f), cue_mid, b.y0 + (b.y1 - b.y0 - kSmallSize) * 0.5f, kSmallSize,
            1, kWhite);
    }

    // 3 and 4. The gap and the stance, side by side, centred together. Widths are estimates.
    const bool gap = v.set.gap && v.has_gap && std::fabs(v.gap) < 100.0f;
    const bool st  = v.set.stance && v.conf != stance::CONF_NONE && v.stance != stance::STANCE_UNKNOWN;
    if (gap || st) {
        const std::string g  = gap ? GapText(v.gap) : std::string();
        const std::string s  = st ? (v.stance == stance::SIT ? "SIT" : "STAND") : "";
        const float       wg = TextWidth(g.size(), kSmallSize), ws = TextWidth(s.size(), kSmallSize);
        const float       sep = gap && st ? 0.015f : 0.0f;
        const Box         b   = RowBoxAt(v.set, wg + sep + ws);
        const float       x0  = b.x0 + 0.01f;
        const float       y   = b.y0 + (kRowH - kSmallSize) * 0.5f;
        Rect(f, b.x0, b.y0, b.x1, b.y1, kBacking);
        if (gap) Say(f, g, x0, y, kSmallSize, 0, v.gap > 0 ? kRed : kGreen);
        if (st) {
            uint32_t c = v.stance == stance::SIT ? kBlue : kWhite;
            if (v.conf == stance::CONF_GUESS) c = (c & 0x00FFFFFFu) | 0xA0000000u;
            Say(f, s, x0 + wg + sep, y, kSmallSize, 0, c);
        }
    }

    // 6. The setup card, while stopped.
    if (v.set.setup && v.stopped) {
        const Box& b = kCardBox;
        Rect(f, b.x0, b.y0, b.x1, b.y1, kBacking);
        const std::string name = "Setup: " + (v.setup.empty() ? std::string("default") : v.setup);
        Say(f, Fit(name, 0.026f, b.x1 - b.x0 - 0.02f), 0.5f, b.y0 + 0.02f, 0.026f, 1, kWhite);
        if (v.sag) Say(f, "Stop 2 seconds in neutral to measure sag", 0.5f, b.y0 + 0.065f, kSmallSize, 1, kAmber);
    }

    // 7. The suspension, bottom right: how much travel each end is using now, and a mark where
    //    it bottomed. Off unless hud.ini asks for it.
    if (v.set.susp && v.has_susp) {
        const Box b = SuspBoxAt(v.set);
        Rect(f, b.x0, b.y0, b.x1, b.y1, kBacking);
        const float h  = (b.y1 - b.y0 - kSuspPad * 3) * 0.5f;
        const float x0 = b.x0 + kSuspPad + kSuspLabelW, x1 = b.x1 - kSuspPad;
        for (int i = 0; i < 2; ++i) {
            const float y0 = b.y0 + kSuspPad + float(i) * (h + kSuspPad), y1 = y0 + h;
            Say(f, i == 0 ? "F" : "R", b.x0 + kSuspPad, y0 + (h - kSmallSize) * 0.5f, kSmallSize, 0, kGrey);
            Rect(f, x0, y0, x1, y1, 0x50FFFFFFu);  // the travel there is
            const float used = (std::max)(0.0f, (std::min)(1.0f, v.susp[i]));
            if (used > 0) Rect(f, x0, y0, x0 + (x1 - x0) * used, y1, kBlue);
            // Where it bottomed: a tick standing slightly proud of the bar, so it reads as a
            // mark on the scale rather than as more fill.
            const float deep = (std::max)(0.0f, (std::min)(1.0f, v.susp_max[i]));
            if (deep > 0) {
                const float mx = x0 + (x1 - x0) * deep;
                Rect(f, mx - 0.0012f, y0 - 0.002f, mx + 0.0012f, y1 + 0.002f, kRed);
            }
        }
    }

    // 5. The map, bottom left: the line, the cue spots ahead, Coach's line to take, then the
    //    ghost and the rider on top of it.
    if (v.set.map && v.track && v.track->ready()) {
        const Track& t = *v.track;
        const Box    b = MapBoxAt(v.set);
        Rect(f, b.x0, b.y0, b.x1, b.y1, 0x80000000u);
        // Every other segment, and every other one again if the line is long: a map this size
        // cannot show 300 segments apart, and each one spent a quad the rider's own marker
        // might have needed.
        const std::vector<Pt>& line = t.line();
        const size_t step = (std::max)(size_t(1), (line.size() + kLineOnMap - 1) / kLineOnMap);
        Pt prev = Project(t, b, line[0].x, line[0].y);
        for (size_t i = step; i < line.size(); i += step) {
            const Pt p = Project(t, b, line[i].x, line[i].y);
            Line(f, prev.x, prev.y, p.x, p.y, kGrey, 0.003f);
            prev = p;
        }
        // Back to the start, so the lap reads as a loop rather than stopping short.
        const Pt last = Project(t, b, line[0].x, line[0].y);
        Line(f, prev.x, prev.y, last.x, last.y, kGrey, 0.003f);
        for (float m : v.upcoming) {
            float x, z;
            if (!t.at(m, x, z)) continue;
            const Pt p = Project(t, b, x, z);
            Dot(f, p.x, p.y, kAmber, 0.008f);
        }
        // Coach's line for the stretch coming up, in blue: where to take it. Walked forward
        // from the rider's position in lap order, so the piece that wraps past the line joins
        // up properly instead of being drawn straight across the track. Drawn only from the
        // sheet's own points - with no sheet there is nothing here, which is the whole rule:
        // a line guessed from the centreline would be a wrong line confidently drawn.
        if (v.set.trail && v.ref && v.ref->size() >= 2) {
            const std::vector<RefPoint>& r = *v.ref;
            size_t first = 0;
            while (first < r.size() && r[first].pos < v.pos) ++first;
            // The sheet carries up to 2000 points, so a twelfth of a lap of them is far more
            // than this map can draw. Thinned to a fixed budget: the same line, a fraction of
            // the quads, and no chance of crowding out what is drawn after it.
            const size_t want = size_t(float(r.size()) * kTrailLap);
            const size_t step = (std::max)(size_t(1), (want + kTrailSegs - 1) / kTrailSegs);
            bool started = false;
            Pt   prev_pt{};
            for (size_t k = 0; k < r.size(); k += step) {
                const RefPoint& p = r[(first + k) % r.size()];
                float ahead = p.pos - v.pos;
                if (ahead < 0) ahead += 1.0f;
                if (ahead > kTrailLap) break;
                const Pt sp = Project(t, b, p.x, p.z);
                if (started) Line(f, prev_pt.x, prev_pt.y, sp.x, sp.y, kBlue, 0.004f);
                prev_pt = sp;
                started = true;
            }
        }
        // On the reserve: whatever else ran out of room, the rider has to be able to see
        // themselves and the lap they are chasing.
        Reserved hold(f);
        if (v.has_ghost) {
            const Pt p = Project(t, b, v.ghost.x, v.ghost.y);
            Dot(f, p.x, p.y, kBlue, 0.011f);
        }
        if (v.has_rider) {
            const Pt p = Project(t, b, v.rider.x, v.rider.y);
            // The world heading through the same flip `Project` applies to z, so the arrow
            // points where the bike is pointing on the map rather than mirrored.
            Arrow(f, p.x, p.y, v.rider_dir.x, -v.rider_dir.y, kWhite, 0.019f);
        }
    }

    // 8. The pointer, over everything, and on the reserve: a rider dragging a part must be able
    //    to see where they are pointing even on a busy frame.
    if (v.has_pointer) {
        Reserved hold(f);
        Pointer(f, v.pointer.x, v.pointer.y);
    }
}

}  // namespace coachhud
