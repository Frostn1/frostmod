// coachmark.h - extra marks drawn on and over the Coach ground line (coachline.h): flat bars and
// boxes lying on the ribbon, and short labels standing over it, as plain coloured quads in the
// telemetry world. Pure maths, no GL, so the tests and the offline previews draw exactly what the
// plugin draws. coachjump.h builds its jump calls out of these; mxbcoach.cpp draws them after the
// ribbon, through the same camera.
//
// Everything is placed relative to the ribbon as built (coachline::Ribbon::verts()): a distance
// ahead along it, across it and up from it. The ribbon already lies on the ground the best way the
// plugin knows (the track's own terrain, or the rider's ground plus the centreline's rise), so a
// mark placed on it lies on the same ground, and takes the same depth-snap correction by `s`.
//
// READ-ONLY toward the game, like the ribbon: it only adds geometry to the plugin's own draw.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "coachline.h"

namespace coachmark {

/// One flat quad, corners in order around it, telemetry world (x east, y up, z north).
struct Quad {
    float p[4][3];
    float rgba[4];
    float s;  // metres ahead along the ribbon it belongs to: the depth snap is looked up by it
};

/// Where the ribbon is `s` metres ahead: its centre, the ground under each edge, and its frame
/// in x/z (`l` points to the line's left, `f` along it).
struct Spot {
    bool  ok = false;
    float x = 0, y = 0, z = 0;  // the centre
    float yl = 0, yr = 0;       // the left and right edges' heights
    float lx = 0, lz = 0;       // unit left, horizontal
    float fx = 0, fz = 0;       // unit forward, horizontal
    float hw = coachline::kHalfWidth;  // half the ribbon's width there
};

/// The ribbon at `s` metres ahead, between the two rows either side of it. Not ok before the
/// first row or past the last, or on a row with no width.
inline Spot At(const std::vector<coachline::Vert>& v, float s) {
    Spot out;
    const size_t rows = v.size() / 2;
    if (rows < 2 || !std::isfinite(s) || s < v[0].s || s > v[(rows - 1) * 2].s) return out;
    size_t a = 0;
    while (a + 2 < rows && v[(a + 1) * 2].s < s) ++a;
    const coachline::Vert &al = v[a * 2], &ar = v[a * 2 + 1], &bl = v[a * 2 + 2], &br = v[a * 2 + 3];
    const float span = bl.s - al.s;
    const float k    = span > 1e-4f ? (std::max)(0.0f, (std::min)(1.0f, (s - al.s) / span)) : 0.0f;
    const float lx = al.x + (bl.x - al.x) * k, lz = al.z + (bl.z - al.z) * k;
    const float rx = ar.x + (br.x - ar.x) * k, rz = ar.z + (br.z - ar.z) * k;
    out.yl         = al.y + (bl.y - al.y) * k;
    out.yr         = ar.y + (br.y - ar.y) * k;
    out.x = (lx + rx) * 0.5f, out.z = (lz + rz) * 0.5f, out.y = (out.yl + out.yr) * 0.5f;
    const float w = std::hypot(lx - rx, lz - rz);
    if (!(w > 1e-4f) || !std::isfinite(out.y)) return out;
    out.hw = 0.5f * w;
    out.lx = (lx - rx) / w, out.lz = (lz - rz) / w;
    // Left is forward turned a quarter anticlockwise seen from above (coachline: n = (-tz, tx)).
    out.fx = out.lz, out.fz = -out.lx;
    out.ok = true;
    return out;
}

/// A rectangle lying on the ribbon: centred `across` metres left of its centre line at `s`,
/// `half_w` either side across it and `half_d` either side along it, `lift` above the ground. Each
/// side follows its own edge's height, so it lies on a cambered ribbon as the ribbon does.
inline bool Flat(std::vector<Quad>& out, const Spot& p, float s, float across, float half_w, float half_d, float lift,
                 const float rgba[4]) {
    if (!p.ok) return false;
    Quad q;
    const float cx = p.x + p.lx * across, cz = p.z + p.lz * across;
    // The height across, extended past the ribbon's edges along the same camber.
    auto y_at = [&](float off) {
        const float half = (std::max)(1e-3f, p.hw);
        return p.y + (p.yl - p.yr) * 0.5f * (off / half) + lift;
    };
    const float sx[4] = {+1, +1, -1, -1}, sf[4] = {-1, +1, +1, -1};
    for (int c = 0; c < 4; ++c) {
        const float off = across + sx[c] * half_w;
        q.p[c][0] = cx + p.lx * sx[c] * half_w + p.fx * sf[c] * half_d;
        q.p[c][1] = y_at(off);
        q.p[c][2] = cz + p.lz * sx[c] * half_w + p.fz * sf[c] * half_d;
    }
    for (int c = 0; c < 4; ++c) q.rgba[c] = rgba[c];
    q.s = s;
    out.push_back(q);
    return true;
}

// ---------------------------------------------------------------------------------------
// Text: a 5 x 7 block font, each lit run of a row one quad, standing upright over the ribbon.
// Big enough to read from 30-50 m: at 1 m a letter is ~30 px tall at 40 m on a 1440p screen.

/// Seven rows of five bits, the leftmost column the highest bit. Null for a character it lacks.
inline const uint8_t* Glyph(char c) {
    static const uint8_t az[26][7] = {
        {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},  // A B C
        {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},  // D E F
        {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},      // G H I
        {7, 2, 2, 2, 2, 18, 12},      {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},  // J K L
        {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17}, {14, 17, 17, 17, 17, 17, 14},  // M N O
        {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},  // P Q R
        {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},       {17, 17, 17, 17, 17, 17, 14},  // S T U
        {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},   // V W X
        {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31}};                                   // Y Z
    static const uint8_t digits[10][7] = {
        {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},   {14, 17, 1, 2, 4, 8, 31},  {31, 2, 4, 2, 1, 17, 14},
        {2, 6, 10, 18, 31, 2, 2},     {31, 16, 30, 1, 1, 17, 14}, {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
        {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12}};
    static const uint8_t dash[7] = {0, 0, 0, 31, 0, 0, 0}, slash[7] = {1, 1, 2, 4, 8, 16, 16},
                         dot[7] = {0, 0, 0, 0, 0, 12, 12}, bang[7] = {4, 4, 4, 4, 4, 0, 4}, tick[7] = {4, 4, 8, 0, 0, 0, 0},
                         space[7] = {0, 0, 0, 0, 0, 0, 0};
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return az[c - 'A'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    switch (c) {
        case '-': return dash;
        case '/': return slash;
        case '.': return dot;
        case '!': return bang;
        case '\'': return tick;
        case ' ': return space;
        default: return nullptr;
    }
}

/// Columns a character advances by: five lit, one gap.
constexpr int kAdvance = 6;

/// How wide `t` is at cap height `h`: the last character's gap left off.
inline float TextWidth(const std::string& t, float h) {
    if (t.empty()) return 0;
    return (float(t.size()) * kAdvance - 1) * (h / 7.0f);
}

/// `t` standing upright, centred on `base` (the middle of its bottom edge), reading along
/// `right` (a horizontal unit vector) with `up` straight up, cap height `h`. Each lit run of a
/// glyph row is one quad. Characters the font lacks are left as a gap.
///
/// Drawn in the rider's style (coachline::Look().text_style): bold widens every lit run by a
/// fifth of a pixel each side, still inside the gap between letters; italic leans each row
/// forward by a fifth of a pixel per row up. Sizes are the caller's, scaled there.
inline void Text(std::vector<Quad>& out, const std::string& t, const float base[3], const float right[3], float h,
                 const float rgba[4], float s) {
    const float px    = h / 7.0f;
    const float x0    = -0.5f * TextWidth(t, h);
    const int   style = coachline::Look().text_style;
    const float fat   = style == coachhud::TEXT_BOLD ? 0.2f * px : 0.0f;
    const float lean  = style == coachhud::TEXT_ITALIC ? 0.2f : 0.0f;  // x per unit of height
    for (size_t i = 0; i < t.size(); ++i) {
        const uint8_t* g = Glyph(t[i]);
        if (!g) continue;
        for (int row = 0; row < 7; ++row) {
            const float y_lo = float(6 - row) * px, y_hi = y_lo + px;
            for (int col = 0; col < 5;) {
                if (!(g[row] & (16 >> col))) {
                    ++col;
                    continue;
                }
                int end = col;
                while (end < 5 && (g[row] & (16 >> end))) ++end;
                const float a = x0 + (float(i) * kAdvance + float(col)) * px - fat,
                            b = x0 + (float(i) * kAdvance + float(end)) * px + fat;
                Quad q;
                const float sl = lean * (y_lo - 3.5f * px), sh = lean * (y_hi - 3.5f * px);  // about the middle row
                const float xs[4] = {a + sl, b + sl, b + sh, a + sh}, ys[4] = {y_lo, y_lo, y_hi, y_hi};
                for (int c = 0; c < 4; ++c) {
                    q.p[c][0] = base[0] + right[0] * xs[c];
                    q.p[c][1] = base[1] + right[1] * xs[c] + ys[c];
                    q.p[c][2] = base[2] + right[2] * xs[c];
                }
                for (int c = 0; c < 4; ++c) q.rgba[c] = rgba[c];
                q.s = s;
                out.push_back(q);
                col = end;
            }
        }
    }
}

}  // namespace coachmark
