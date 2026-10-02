// coachcolor_render - draws the colours of Coach's ground line (src/coachcolor.h) top-down for a
// .hud sheet, as a PNG: the whole lap, a legend, a zoomed crop of the strongest braking onset
// with the ribbon's vertices marked, and (when the .cue sits beside the .hud) the same lap
// coloured from the cue sheet's braking zones alone, the fallback for a sheet with no channels.
//
//   coachcolor_render <sheet.hud> <out.png> [--cue <sheet.cue>]
//
// A sheet from before the channels existed has none, so they are synthesised from the line's
// curvature (corner speed from lateral grip, braked down to it, accelerated out of it) and the
// picture says so: "SYNTHETIC CHANNELS". Offline only; nothing here ships in a plugin.

#include "../src/coachcolor.h"
#include "../src/coachhud.h"
#include "../src/coachline.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace coachcolor;

// --- a canvas and the little it needs ---------------------------------------------------

struct Canvas {
    int                  w, h;
    std::vector<uint8_t> rgb;
    Canvas(int w_, int h_) : w(w_), h(h_), rgb(size_t(w_) * size_t(h_) * 3, 0) {}
    void put(int x, int y, float r, float g, float b, float a = 1.0f) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint8_t* p = &rgb[(size_t(y) * size_t(w) + size_t(x)) * 3];
        const float c[3] = {r, g, b};
        for (int i = 0; i < 3; ++i) p[i] = uint8_t(std::lround(255.0f * Clamp01(float(p[i]) / 255.0f * (1 - a) + c[i] * a)));
    }
    void fill(int x0, int y0, int x1, int y1, float r, float g, float b, float a = 1.0f) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) put(x, y, r, g, b, a);
    }
};

// 5x7 glyphs, one byte per column, low bit at the top. Upper case, digits and a few marks.
static const char* kGlyphChars = " -.:/()+0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const uint8_t kGlyphs[][5] = {
    {0, 0, 0, 0, 0},          {8, 8, 8, 8, 8},          {0, 0x60, 0x60, 0, 0},    {0, 0x36, 0x36, 0, 0},
    {0x20, 0x10, 8, 4, 2},    {0, 0x1C, 0x22, 0x41, 0}, {0, 0x41, 0x22, 0x1C, 0}, {8, 8, 0x3E, 8, 8},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0, 0x42, 0x7F, 0x40, 0}, {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {1, 0x71, 9, 5, 3}, {0x36, 0x49, 0x49, 0x49, 0x36}, {6, 0x49, 0x49, 0x29, 0x1E},
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 9, 9, 9, 1},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 8, 8, 8, 0x7F}, {0, 0x41, 0x7F, 0x41, 0}, {0x20, 0x40, 0x41, 0x3F, 1},
    {0x7F, 8, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 2, 0x0C, 2, 0x7F}, {0x7F, 4, 8, 0x10, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 9, 9, 9, 6}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 9, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {1, 1, 0x7F, 1, 1}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 8, 0x14, 0x63}, {7, 8, 0x70, 8, 7}, {0x61, 0x51, 0x49, 0x45, 0x43}};

static int TextWidth(const std::string& t, int sc) { return int(t.size()) * 6 * sc; }

static void Text(Canvas& c, int x, int y, int sc, const std::string& t, float r = 1, float g = 1, float b = 1) {
    for (char ch : t) {
        if (ch >= 'a' && ch <= 'z') ch = char(ch - 'a' + 'A');
        const char* at = std::strchr(kGlyphChars, ch);
        const int   gi = (at && ch) ? int(at - kGlyphChars) : 0;
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row)
                if (kGlyphs[gi][col] >> row & 1) c.fill(x + col * sc, y + row * sc, x + col * sc + sc, y + row * sc + sc, r, g, b);
        x += 6 * sc;
    }
}

// --- PNG (stored deflate blocks: no compressor needed) -----------------------------------

static uint32_t Crc(const uint8_t* d, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool     init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        init = true;
    }
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static void Be32(std::vector<uint8_t>& o, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) o.push_back(uint8_t(v >> s));
}

static void Chunk(std::vector<uint8_t>& o, const char* type, const std::vector<uint8_t>& data) {
    Be32(o, uint32_t(data.size()));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    o.insert(o.end(), td.begin(), td.end());
    Be32(o, ~Crc(td.data(), td.size()));
}

static bool WritePng(const Canvas& c, const char* path) {
    std::vector<uint8_t> raw;
    for (int y = 0; y < c.h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), c.rgb.begin() + size_t(y) * size_t(c.w) * 3, c.rgb.begin() + size_t(y + 1) * size_t(c.w) * 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t             a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t at = 0; at < raw.size(); at += 65535) {
        const size_t n = (std::min)(size_t(65535), raw.size() - at);
        z.push_back(at + n >= raw.size() ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)); z.push_back(uint8_t((~n) >> 8));
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    }
    Be32(z, (b << 16) | a);
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    Be32(ihdr, uint32_t(c.w)); Be32(ihdr, uint32_t(c.h));
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    Chunk(out, "IHDR", ihdr);
    Chunk(out, "IDAT", z);
    Chunk(out, "IEND", {});
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    return bool(f);
}

// --- the lap, resampled; synthetic channels -----------------------------------------------

struct Pt {
    float x, z, s;
};

// Coach's reference points at an even spacing along the lap (position runs 0..1 over track_len).
static std::vector<Pt> Resample(const coachhud::Sheet& sh, float step) {
    std::vector<Pt> out;
    const size_t    n = sh.ref.size();
    const int       m = int(sh.track_len / step);
    size_t          i = 0;
    for (int k = 0; k < m; ++k) {
        const float pos = float(k) * step / sh.track_len;
        while (i + 1 < n && sh.ref[i + 1].pos <= pos) ++i;
        const coachhud::RefPoint& a = sh.ref[i];
        const coachhud::RefPoint& b = sh.ref[(i + 1) % n];
        float                     span = b.pos - a.pos;
        if (span <= 0) span += 1.0f;
        const float f = (std::min)(1.0f, (std::max)(0.0f, (pos - a.pos) / span));
        out.push_back({a.x + (b.x - a.x) * f, a.z + (b.z - a.z) * f, float(k) * step});
    }
    return out;
}

// What a rider would plausibly do: slowest in the tightest corners, braking down to each, on the
// gas out of it, coasting where the speed holds.
static std::vector<Sample> Synthesize(const std::vector<Pt>& p, float step) {
    const int          n = int(p.size());
    std::vector<float> curv(size_t(n), 0.0f);
    const int          h = int(std::lround(4.0f / step));
    for (int i = 0; i < n; ++i) {
        const Pt &a = p[size_t((i - h + n) % n)], &b = p[size_t(i)], &c = p[size_t((i + h) % n)];
        const float h0 = std::atan2(b.x - a.x, b.z - a.z), h1 = std::atan2(c.x - b.x, c.z - b.z);
        float       d = h1 - h0;
        while (d > 3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        curv[size_t(i)] = std::fabs(d) / (2.0f * float(h) * step);
    }
    curv = detail::Smooth(curv, step, 5.0f, true);
    std::vector<float> v(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i) v[size_t(i)] = (std::min)(21.0f, (std::max)(7.0f, std::sqrt(5.5f / (std::max)(curv[size_t(i)], 1e-4f))));
    for (int pass = 0; pass < 2; ++pass) {  // twice round, so the lap closes
        for (int i = 0; i < n; ++i) {       // accelerate out of corners
            const float prev = v[size_t((i + n - 1) % n)];
            v[size_t(i)]     = (std::min)(v[size_t(i)], std::sqrt(prev * prev + 2.0f * 4.5f * step));
        }
        for (int i = n - 1; i >= 0; --i) {  // brake down to them
            const float next = v[size_t((i + 1) % n)];
            v[size_t(i)]     = (std::min)(v[size_t(i)], std::sqrt(next * next + 2.0f * 8.0f * step));
        }
    }
    std::vector<Sample> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float vp = v[size_t((i + n - 1) % n)], vn = v[size_t((i + 1) % n)];
        const float a  = (vn * vn - vp * vp) / (4.0f * step);  // m/s^2
        Sample&     s  = out[size_t(i)];
        s.s            = p[size_t(i)].s;
        s.speed        = v[size_t(i)];
        const bool cruise = v[size_t(i)] >= 20.5f;
        s.throttle     = cruise ? 1.0f : Clamp01(a / 1.5f);
        s.brake        = Clamp01((-a - 0.8f) / 5.0f);
    }
    return out;
}

// --- drawing the ribbon --------------------------------------------------------------------

struct View {
    int   x0, y0, x1, y1;  // the panel on the canvas
    float cx, cz, scale;   // world centre and px per metre
    int   px(float x) const { return int(std::lround(float(x0 + x1) * 0.5f + (x - cx) * scale)); }
    int   py(float z) const { return int(std::lround(float(y0 + y1) * 0.5f - (z - cz) * scale)); }
};

static View Fit(int x0, int y0, int x1, int y1, float minx, float maxx, float minz, float maxz, float pad) {
    View v{x0, y0, x1, y1, (minx + maxx) * 0.5f, (minz + maxz) * 0.5f, 1};
    v.scale = (std::min)((float(x1 - x0) - 2 * pad) / (std::max)(maxx - minx, 1.0f),
                         (float(y1 - y0) - 2 * pad) / (std::max)(maxz - minz, 1.0f));
    return v;
}

static void Panel(Canvas& c, const View& v) {
    c.fill(v.x0, v.y0, v.x1, v.y1, 0.33f, 0.25f, 0.17f);  // dirt
    for (int x = v.x0; x < v.x1; ++x) { c.put(x, v.y0, 1, 1, 1, 0.35f); c.put(x, v.y1 - 1, 1, 1, 1, 0.35f); }
    for (int y = v.y0; y < v.y1; ++y) { c.put(v.x0, y, 1, 1, 1, 0.35f); c.put(v.x1 - 1, y, 1, 1, 1, 0.35f); }
}

struct Layer {  // opaque colour stamps, composited once at the line's alpha
    int                w, h;
    std::vector<float> rgba;
    Layer(int w_, int h_) : w(w_), h(h_), rgba(size_t(w_) * size_t(h_) * 4, 0.0f) {}
    void disc(const View& v, float x, float y, float rad, const Rgba& c) {
        for (int j = int(std::floor(y - rad)); j <= int(std::ceil(y + rad)); ++j)
            for (int i = int(std::floor(x - rad)); i <= int(std::ceil(x + rad)); ++i) {
                if (i < v.x0 || j < v.y0 || i >= v.x1 || j >= v.y1) continue;
                const float dx = float(i) + 0.5f - x, dy = float(j) + 0.5f - y;
                if (dx * dx + dy * dy > rad * rad) continue;
                float* q = &rgba[(size_t(j) * size_t(w) + size_t(i)) * 4];
                q[0] = c.r; q[1] = c.g; q[2] = c.b; q[3] = c.a;
            }
    }
};

// The ribbon the way GL would draw it: vertices where Subdivide puts them, colour interpolated
// linearly between neighbours.
static std::vector<float> DrawLine(Canvas& c, const View& v, const std::vector<Pt>& p, const Line& line, float s_from, float s_to,
                                   float width_m, float base_step, bool ticks) {
    const float lap = p.back().s + (p.size() > 1 ? p[1].s - p[0].s : 1.0f);
    auto        at  = [&](float s) {
        const float step = p[1].s - p[0].s;
        float       u    = std::fmod(s, lap);
        if (u < 0) u += lap;
        const size_t i = size_t(u / step) % p.size(), j = (i + 1) % p.size();
        const float  f = u / step - std::floor(u / step);
        return Pt{p[i].x + (p[j].x - p[i].x) * f, p[i].z + (p[j].z - p[i].z) * f, s};
    };
    const std::vector<float> verts = Subdivide(line, s_from, s_to, base_step);
    Layer                    L(c.w, c.h);
    const float              rad = (std::max)(1.0f, width_m * v.scale * 0.5f);
    for (size_t i = 1; i < verts.size(); ++i) {
        const Pt   a = at(verts[i - 1]), b = at(verts[i]);
        const Rgba ca = line.at(verts[i - 1]), cb = line.at(verts[i]);
        const float ax = float(v.px(a.x)), ay = float(v.py(a.z)), bx = float(v.px(b.x)), by = float(v.py(b.z));
        const float len = (std::max)(1.0f, std::hypot(bx - ax, by - ay));
        for (float t = 0; t <= len; t += 0.5f) {
            const float f = t / len;
            L.disc(v, ax + (bx - ax) * f, ay + (by - ay) * f, rad,
                   {ca.r + (cb.r - ca.r) * f, ca.g + (cb.g - ca.g) * f, ca.b + (cb.b - ca.b) * f, ca.a + (cb.a - ca.a) * f});
        }
    }
    for (int y = v.y0; y < v.y1; ++y)
        for (int x = v.x0; x < v.x1; ++x) {
            const float* q = &L.rgba[(size_t(y) * size_t(L.w) + size_t(x)) * 4];
            if (q[3] > 0) c.put(x, y, q[0], q[1], q[2], q[3]);
        }
    if (ticks)
        for (float s : verts) {
            const Pt a = at(s);
            c.fill(v.px(a.x) - 1, v.py(a.z) - 1, v.px(a.x) + 2, v.py(a.z) + 2, 0.05f, 0.05f, 0.05f);
        }
    return verts;
}

static void Legend(Canvas& c, int x, int y, int w) {
    Text(c, x, y, 2, "LEGEND", 1, 1, 1);
    const int bar_y = y + 24, bar_h = 22;
    c.fill(x, bar_y, x + w, bar_y + bar_h + 1, 0.33f, 0.25f, 0.17f);
    for (int i = 0; i < w; ++i) {
        const Rgba col = Gradient(3.0f * float(i) / float(w - 1), 0.7f);
        c.fill(x + i, bar_y, x + i + 1, bar_y + bar_h, col.r, col.g, col.b, col.a);
    }
    const char* names[4] = {"GAS", "COAST", "LIGHT BRAKE", "HEAVY BRAKE"};
    for (int k = 0; k < 4; ++k) {
        const int cx = x + (w - 1) * k / 3;
        c.fill(cx, bar_y + bar_h, cx + 1, bar_y + bar_h + 6, 1, 1, 1);
        int tx = cx - TextWidth(names[k], 1) / 2;
        tx     = (std::max)(x, (std::min)(tx, x + w - TextWidth(names[k], 1)));
        Text(c, tx, bar_y + bar_h + 9, 1, names[k]);
    }
}

static std::vector<unsigned char> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: coachcolor_render <sheet.hud> <out.png> [--cue <sheet.cue>]\n");
        return 2;
    }
    std::string hud = argv[1], cue;
    for (int i = 3; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--cue")) cue = argv[i + 1];
    if (cue.empty() && hud.size() > 4) cue = hud.substr(0, hud.size() - 4) + ".cue";

    const std::vector<unsigned char> file = ReadFile(hud);
    coachhud::Sheet                  sheet;
    if (file.empty() || !coachhud::Parse(file.data(), file.size(), sheet) || sheet.ref.size() < 3 || sheet.track_len <= 0) {
        std::fprintf(stderr, "cannot read %s\n", hud.c_str());
        return 1;
    }
    // No channels in an MXHD v1 sheet: synthesise them from the line, and say so.
    const float            step    = 2.0f;
    const std::vector<Pt>  pts     = Resample(sheet, step);
    const std::vector<Sample> smp  = Synthesize(pts, step);
    const Line             line    = Build(smp, sheet.track_len);

    float minx = 1e9f, maxx = -1e9f, minz = 1e9f, maxz = -1e9f;
    for (const Pt& q : pts) {
        minx = (std::min)(minx, q.x); maxx = (std::max)(maxx, q.x);
        minz = (std::min)(minz, q.z); maxz = (std::max)(maxz, q.z);
    }

    const int W = 1700, H = 1000;
    Canvas    c(W, H);
    c.fill(0, 0, W, H, 0.09f, 0.09f, 0.10f);

    // The title, then the whole lap.
    std::string name = hud;
    const size_t sl  = name.find_last_of("/\\");
    if (sl != std::string::npos) name = name.substr(sl + 1);
    if (name.size() > 4) name.resize(name.size() - 4);
    for (char& ch : name) if (ch == '_' || ch == '.') ch = ' ';
    Text(c, 20, 14, 3, name);
    Text(c, 20, 46, 2, "SYNTHETIC CHANNELS (FROM CURVATURE) - GAS GREEN, COAST WHITE, LIGHT BRAKE YELLOW, HEAVY BRAKE RED", 1, 0.8f, 0.4f);
    const View lap = Fit(20, 80, 1060, 980, minx, maxx, minz, maxz, 40);
    Panel(c, lap);
    DrawLine(c, lap, pts, line, 0, sheet.track_len, 3.0f, 2.0f, false);
    Text(c, lap.x0 + 10, lap.y1 - 22, 1, "TOP DOWN, NORTH UP, LINE 3 M WIDE FOR READING");

    // Legend, and the crop on the strongest braking onset.
    Legend(c, 1090, 80, 590);
    float best = -1, best_s = 0;
    for (float s = 20; s < sheet.track_len - 20; s += 1.0f) {
        const float rise = line.pos_at(s + 12) - line.pos_at(s - 12);
        if (rise > best) { best = rise; best_s = s; }
    }
    const float zs0 = best_s - 14, zs1 = best_s + 14;
    float       zminx = 1e9f, zmaxx = -1e9f, zminz = 1e9f, zmaxz = -1e9f;
    for (const Pt& q : pts)
        if (q.s >= zs0 && q.s <= zs1) {
            zminx = (std::min)(zminx, q.x); zmaxx = (std::max)(zmaxx, q.x);
            zminz = (std::min)(zminz, q.z); zmaxz = (std::max)(zmaxz, q.z);
        }
    const View zoom = Fit(1090, 190, 1680, 590, zminx - 6, zmaxx + 6, zminz - 6, zmaxz + 6, 40);
    Panel(c, zoom);
    DrawLine(c, zoom, pts, line, zs0, zs1, 1.0f, 2.0f, true);
    char cap[160];
    std::snprintf(cap, sizeof cap, "ZOOM: BRAKING ONSET, %.0f M TO %.0f M ALONG THE LAP (DOTS = RIBBON VERTICES)", double(zs0), double(zs1));
    Text(c, zoom.x0 + 8, zoom.y0 + 8, 1, cap);

    // The fallback: cue-sheet braking zones only.
    const std::vector<unsigned char> cf = ReadFile(cue);
    coachcue::Sheet                  cs;
    if (!cf.empty() && coachcue::Parse(cf.data(), cf.size(), cs)) {
        const auto zones = coachline::BrakeZones(cs, sheet.track_len);
        const Line fb    = BuildFromZones(zones, sheet.track_len);
        const View fv    = Fit(1090, 610, 1680, 980, minx, maxx, minz, maxz, 25);
        Panel(c, fv);
        DrawLine(c, fv, pts, fb, 0, sheet.track_len, 4.0f, 2.0f, false);
        Text(c, fv.x0 + 8, fv.y0 + 8, 1, "FALLBACK: NO CHANNELS, COLOURED FROM THE CUE SHEET BRAKING ZONES");
    }

    if (!WritePng(c, argv[2])) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    std::printf("wrote %s (%dx%d), zoom at %.0f m\n", argv[2], W, H, double(best_s));
    return 0;
}
