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
#include <cstring>
#include <string>
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

struct Settings {
    bool enabled = true, cue = true, section = true, gap = true, stance = true, map = true, setup = true;
};

/// `<save>\mxbcoach\hud.ini`, [hud] key=1|0. Anything missing is on, except the map when
/// MXBMRP3 is installed (it draws its own) unless map=1 says otherwise.
inline Settings ParseSettings(const std::string& ini, bool mxbmrp3) {
    auto flag = [&](const char* key, bool def) {
        const std::string v = stance::IniValue(ini, "hud", key);
        return v == "1" ? true : v == "0" ? false : def;
    };
    Settings s;
    s.enabled = flag("enabled", true);
    s.cue     = flag("cue", true);
    s.section = flag("section", true);
    s.gap     = flag("gap", true);
    s.stance  = flag("stance", true);
    s.map     = flag("map", !mxbmrp3);
    s.setup   = flag("setup", true);
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
constexpr size_t kMaxQuads = 400;
constexpr size_t kMaxTexts = 16;
constexpr size_t kMaxChars = 99;  // SPluginString_t holds 100 bytes

struct Frame {
    std::vector<Quad> quads;
    std::vector<Text> texts;
    void clear() {
        quads.clear();
        texts.clear();
    }
};

// Where everything goes, as screen fractions (16:9). Clear of MXBMRP3's Notices (y 0.099-0.165)
// and Timing (y 0.187-0.253) panels.
constexpr Box   kCueBox     = {0.35f, 0.285f, 0.65f, 0.335f};
constexpr float kCueSize    = 0.040f;
constexpr Box   kSectionBox = {0.35f, 0.338f, 0.65f, 0.362f};
constexpr float kSmallSize  = 0.022f;
constexpr float kRowY0 = 0.365f, kRowY1 = 0.39f;  // gap and stance
constexpr Box   kMapBox  = {0.01f, 0.77f, 0.13f, 0.98f};
constexpr Box   kCardBox = {0.35f, 0.60f, 0.65f, 0.72f};

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
    if (f.quads.size() >= kMaxQuads) return;
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

/// A line `thickness` thick. MXBMRP3's addLineSegment.
inline void Line(Frame& f, float x1, float y1, float x2, float y2, uint32_t color, float thickness) {
    const float dx = x2 - x1, dy = y2 - y1, len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.0001f || f.quads.size() >= kMaxQuads) return;
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
    std::vector<float>       upcoming;      // cue spots ahead, metres from the line
    bool                     stopped = false;
    std::string              setup;
    bool                     sag = false;
};

/// "vs Coach +0.34"
inline std::string GapText(float gap) {
    char s[32];
    std::snprintf(s, sizeof(s), "vs Coach %+.2f", double(gap));
    return s;
}

inline void Build(const View& v, Frame& f) {
    f.clear();
    if (!v.set.enabled) return;

    // 1. The cue, centred.
    if (v.set.cue && v.cue) {
        const Box& b = kCueBox;
        Rect(f, b.x0, b.y0, b.x1, b.y1, kBacking);
        Say(f, Fit(v.cue->text, kCueSize, b.x1 - b.x0), 0.5f, b.y0 + (b.y1 - b.y0 - kCueSize) * 0.5f, kCueSize, 1,
            CueColour(v.cue->kind));
    }

    // 2. The section and its tip, below it.
    if (v.set.section && v.section) {
        const Box& b = kSectionBox;
        std::string s = v.section->name;
        if (!v.section->tip.empty()) s += s.empty() ? v.section->tip : ": " + v.section->tip;
        Rect(f, b.x0, b.y0, b.x1, b.y1, kBacking);
        Say(f, Fit(s, kSmallSize, b.x1 - b.x0 - 0.01f), 0.5f, b.y0 + (b.y1 - b.y0 - kSmallSize) * 0.5f, kSmallSize, 1,
            kWhite);
    }

    // 3 and 4. The gap and the stance, side by side, centred together. Widths are estimates.
    const bool gap = v.set.gap && v.has_gap && std::fabs(v.gap) < 100.0f;
    const bool st  = v.set.stance && v.conf != stance::CONF_NONE && v.stance != stance::STANCE_UNKNOWN;
    if (gap || st) {
        const std::string g  = gap ? GapText(v.gap) : std::string();
        const std::string s  = st ? (v.stance == stance::SIT ? "SIT" : "STAND") : "";
        const float       wg = TextWidth(g.size(), kSmallSize), ws = TextWidth(s.size(), kSmallSize);
        const float       sep = gap && st ? 0.015f : 0.0f;
        const float       x0  = 0.5f - (wg + sep + ws) * 0.5f;
        const float       y   = kRowY0 + (kRowY1 - kRowY0 - kSmallSize) * 0.5f;
        Rect(f, x0 - 0.01f, kRowY0, x0 + wg + sep + ws + 0.01f, kRowY1, kBacking);
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

    // 5. The map, bottom left: the line, the cue spots ahead, Coach's ghost, then the rider.
    if (v.set.map && v.track && v.track->ready()) {
        const Track& t = *v.track;
        const Box&   b = kMapBox;
        Rect(f, b.x0, b.y0, b.x1, b.y1, 0x80000000u);
        const std::vector<Pt>& line = t.line();
        Pt prev = Project(t, b, line[0].x, line[0].y);
        for (size_t i = 1; i < line.size(); ++i) {
            const Pt p = Project(t, b, line[i].x, line[i].y);
            Line(f, prev.x, prev.y, p.x, p.y, kGrey, 0.003f);
            prev = p;
        }
        for (float m : v.upcoming) {
            float x, z;
            if (!t.at(m, x, z)) continue;
            const Pt p = Project(t, b, x, z);
            Dot(f, p.x, p.y, kAmber, 0.008f);
        }
        if (v.has_ghost) {
            const Pt p = Project(t, b, v.ghost.x, v.ghost.y);
            Dot(f, p.x, p.y, kBlue, 0.011f);
        }
        if (v.has_rider) {
            const Pt p = Project(t, b, v.rider.x, v.rider.y);
            Dot(f, p.x, p.y, kWhite, 0.013f);
        }
    }
}

}  // namespace coachhud
