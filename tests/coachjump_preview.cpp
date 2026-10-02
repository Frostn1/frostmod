// Offline previews of the jump calls (src/coachjump.h) on a real Coach sheet, for a person to look
// at: not a test, and not part of the plugin. Built with the tests so it can't rot.
//
//   coachjump_preview <sheet.hud> <grid.bin> <out-prefix> [profile-from-m]
//   coachjump_preview --synthetic <out-prefix>   a supercross rhythm lane instead
//
// `grid.bin` is the track's terrain as MXB Coach reads it (u32 width, u32 height, f32 metres a
// sample, then the heights, row by row; Coach's `write_a_sheet_from_recorded_sessions` dev test
// writes one with GRID_OUT). Writes three binary PPMs:
//   <prefix>-topdown.ppm  the whole lap from above, coloured as the line is, with every call
//   <prefix>-profile.ppm  the ground along the line and the lap's flights, for the busiest stretch
//   <prefix>-rider.ppm    the view from the bike 35 m before the biggest jump, ribbon and marks
//                         drawn exactly as the plugin draws them

#include "../src/coachjump.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace coachjump;

static std::vector<uint8_t> ReadFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct Grid {
    uint32_t w = 0, h = 0;
    float    mps = 0;
    std::vector<float> v;
    bool load(const std::string& p) {
        const auto b = ReadFile(p);
        if (b.size() < 12) return false;
        std::memcpy(&w, &b[0], 4), std::memcpy(&h, &b[4], 4), std::memcpy(&mps, &b[8], 4);
        if (b.size() < 12 + size_t(w) * h * 4) return false;
        v.resize(size_t(w) * h);
        std::memcpy(v.data(), &b[12], v.size() * 4);
        return true;
    }
    // As MXB Coach's ground::height_at.
    float at(float x, float z) const {
        if (!(mps > 0)) return NAN;
        const float gx = x / mps, gz = z / mps;
        if (!(gx >= 0) || !(gz >= 0) || gx > float(w - 1) || gz > float(h - 1)) return NAN;
        const uint32_t c0 = uint32_t(gx), r0 = uint32_t(gz), c1 = (std::min)(c0 + 1, w - 1), r1 = (std::min)(r0 + 1, h - 1);
        const float    fx = gx - float(c0), fz = gz - float(r0);
        auto           H  = [&](uint32_t c, uint32_t r) { return v[size_t(r) * w + c]; };
        const float    top = H(c0, r0) + (H(c1, r0) - H(c0, r0)) * fx, bot = H(c0, r1) + (H(c1, r1) - H(c0, r1)) * fx;
        return top + (bot - top) * fz;
    }
};

struct Image {
    int                w, h;
    std::vector<float> rgb, depth;
    Image(int W, int H) : w(W), h(H), rgb(size_t(W) * H * 3, 0), depth(size_t(W) * H, INFINITY) {}
    void blend(int x, int y, float r, float g, float b, float a) {
        if (x < 0 || y < 0 || x >= w || y >= h || !(a > 0)) return;
        float* p = &rgb[(size_t(y) * w + x) * 3];
        p[0] += (r - p[0]) * a, p[1] += (g - p[1]) * a, p[2] += (b - p[2]) * a;
    }
    void rect(float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
        for (int y = int(std::floor(y0)); y < int(std::ceil(y1)); ++y)
            for (int x = int(std::floor(x0)); x < int(std::ceil(x1)); ++x) blend(x, y, r, g, b, a);
    }
    // A triangle with a depth per corner (metres from the eye), tested against `depth` with a
    // bias, as the plugin's polygon offset does.
    void tri(const float* a, const float* b, const float* c, const float* col, float bias = 0.25f) {
        const int   x0 = (std::max)(0, int(std::floor((std::min)({a[0], b[0], c[0]}))));
        const int   x1 = (std::min)(w - 1, int(std::ceil((std::max)({a[0], b[0], c[0]}))));
        const int   y0 = (std::max)(0, int(std::floor((std::min)({a[1], b[1], c[1]}))));
        const int   y1 = (std::min)(h - 1, int(std::ceil((std::max)({a[1], b[1], c[1]}))));
        const float d  = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
        if (std::fabs(d) < 1e-6f) return;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float px = float(x) + 0.5f, py = float(y) + 0.5f;
                const float u  = ((b[0] - px) * (c[1] - py) - (c[0] - px) * (b[1] - py)) / d;
                const float v  = ((c[0] - px) * (a[1] - py) - (a[0] - px) * (c[1] - py)) / d;
                if (u < 0 || v < 0 || u + v > 1) continue;
                const float z = u * a[2] + v * b[2] + (1 - u - v) * c[2];
                if (z > depth[size_t(y) * w + x] + bias * (1.0f + z * 0.01f)) continue;
                blend(x, y, col[0], col[1], col[2], col[3]);
            }
    }
    // Text in the marks' 5 x 7 font, `px` pixels a font pixel, top-left at (x, y).
    void text(const std::string& t, float x, float y, float px, float r, float g, float b, float a) {
        for (size_t i = 0; i < t.size(); ++i) {
            const uint8_t* gl = coachmark::Glyph(t[i]);
            if (!gl) continue;
            for (int row = 0; row < 7; ++row)
                for (int col = 0; col < 5; ++col)
                    if (gl[row] & (16 >> col)) {
                        const float X = x + (float(i) * coachmark::kAdvance + float(col)) * px, Y = y + float(row) * px;
                        rect(X, Y, X + px, Y + px, r, g, b, a);
                    }
        }
    }
    void line(float x0, float y0, float x1, float y1, float width, float r, float g, float b, float a) {
        const float len = std::hypot(x1 - x0, y1 - y0);
        const int   n   = (std::max)(1, int(len * 2));
        for (int k = 0; k <= n; ++k) {
            const float f = float(k) / float(n), x = x0 + (x1 - x0) * f, y = y0 + (y1 - y0) * f;
            for (int dy = -int(width); dy <= int(width); ++dy)
                for (int dx = -int(width); dx <= int(width); ++dx)
                    if (dx * dx + dy * dy <= width * width) {
                        const int X = int(x) + dx, Y = int(y) + dy;
                        if (X < 0 || Y < 0 || X >= w || Y >= h) continue;
                        float* p = &rgb[(size_t(Y) * w + X) * 3];
                        p[0] += (r - p[0]) * a, p[1] += (g - p[1]) * a, p[2] += (b - p[2]) * a;
                    }
        }
    }
    bool save(const std::string& path) const {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (float c : rgb) std::fputc(int((std::max)(0.0f, (std::min)(1.0f, c)) * 255.0f + 0.5f), f);
        std::fclose(f);
        std::printf("wrote %s\n", path.c_str());
        return true;
    }
};

static float TextW(const std::string& t, float px) { return (float(t.size()) * coachmark::kAdvance - 1) * px; }

// ---------------------------------------------------------------------------------------

static void TopDown(const coachhud::Sheet& sh, const Grid& g, const std::vector<Call>& calls, const std::string& out) {
    float x0 = INFINITY, x1 = -INFINITY, z0 = INFINITY, z1 = -INFINITY;
    for (const auto& p : sh.ref) x0 = (std::min)(x0, p.x), x1 = (std::max)(x1, p.x), z0 = (std::min)(z0, p.z), z1 = (std::max)(z1, p.z);
    const float m = 25;
    x0 -= m, x1 += m, z0 -= m, z1 += m;
    const int   W = 1800;
    const float scale = float(W) / (x1 - x0);
    const int   H = int((z1 - z0) * scale) + 120;
    Image       img(W, H);
    // Hillshade, north up (z grows up the image).
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float wx = x0 + (float(x) + 0.5f) / scale, wz = z1 - (float(y) - 120 + 0.5f) / scale;
            const float h = g.at(wx, wz), hx = g.at(wx + 0.5f, wz), hz = g.at(wx, wz + 0.5f);
            float*      p = &img.rgb[(size_t(y) * W + x) * 3];
            if (!std::isfinite(h) || !std::isfinite(hx) || !std::isfinite(hz)) {
                p[0] = p[1] = p[2] = 0.12f;
                continue;
            }
            // Light from the north-west.
            const float nx = -(hx - h) / 0.5f, nz = -(hz - h) / 0.5f, ny = 1.0f;
            const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
            const float l  = (std::max)(0.0f, (-0.5f * nx + 0.7f * ny + 0.5f * nz) / nl / 0.990f);
            p[0] = 0.20f + 0.32f * l, p[1] = 0.16f + 0.26f * l, p[2] = 0.11f + 0.18f * l;
        }
    auto X = [&](float wx) { return (wx - x0) * scale; };
    auto Y = [&](float wz) { return (z1 - wz) * scale + 120; };
    // The line in its colours.
    const std::vector<float> rgba = coachline::LineColours(sh.ref, sh.drive);
    for (size_t i = 0; i + 1 < sh.ref.size(); ++i) {
        const auto &a = sh.ref[i], &b = sh.ref[i + 1];
        if (std::hypot(b.x - a.x, b.z - a.z) > coachline::kMaxGapM) continue;
        const float r = rgba.empty() ? 0.3f : rgba[i * 4], gg = rgba.empty() ? 0.5f : rgba[i * 4 + 1],
                    bb = rgba.empty() ? 1.0f : rgba[i * 4 + 2];
        img.line(X(a.x), Y(a.z), X(b.x), Y(b.z), 2.2f, r, gg, bb, 0.9f);
    }
    // The flights, white over the line; the lips as bars across it; the landings as boxes.
    const auto& ref = sh.ref;
    for (const Call& c : calls) {
        if (c.kind != ROLL)
            for (size_t i = c.takeoff; i < c.landing && i + 1 < ref.size(); ++i)
                img.line(X(ref[i].x), Y(ref[i].z), X(ref[i + 1].x), Y(ref[i + 1].z), 1.2f, 1, 1, 1, 0.95f);
        const size_t i  = c.takeoff, j = (std::min)(i + 2, ref.size() - 1);
        float        dx = ref[j].x - ref[i].x, dz = ref[j].z - ref[i].z;
        const float  l  = std::hypot(dx, dz);
        if (l > 1e-3f) dx /= l, dz /= l;
        const float lx = -dz, lz = dx;  // left
        img.line(X(ref[i].x - lx * 3), Y(ref[i].z - lz * 3), X(ref[i].x + lx * 3), Y(ref[i].z + lz * 3), 1.6f, 1, 1, 1,
                 c.kind == ROLL ? 0.6f : 1.0f);
        if (c.kind != ROLL) {
            const auto& L = ref[c.landing];
            img.rect(X(L.x) - 5, Y(L.z) - 5, X(L.x) + 5, Y(L.z) + 5, 0.55f, 0.85f, 1.0f, 0.9f);
            img.rect(X(L.x) - 3, Y(L.z) - 3, X(L.x) + 3, Y(L.z) + 3, 0.1f, 0.1f, 0.1f, 0.6f);
        }
        // The label to the line's right, in a dark box.
        const std::string t  = Label(c) + (SpeedHint(c).empty() ? "" : " " + SpeedHint(c));
        const float       px = 2.0f, tw = TextW(t, px);
        float tx = X(ref[i].x - lx * 7), ty = Y(ref[i].z - lz * 7);
        tx -= (lx < 0 ? 0 : tw), ty -= 7;
        img.rect(tx - 4, ty - 4, tx + tw + 4, ty + 7 * px + 4, 0.05f, 0.05f, 0.06f, 0.8f);
        float cr = 1, cg = 1, cb = 1;
        if (c.kind == ROLL) cr = 0.72f, cg = 0.86f;
        if (c.kind == JUMP_ON || c.kind == JUMP_OFF || c.kind == TABLE || c.kind == STEP_UP || c.kind == STEP_DOWN) cb = 0.35f, cg = 0.88f;
        img.text(t, tx, ty, px, cr, cg, cb, 1);
    }
    // Title and legend.
    img.rect(0, 0, float(W), 110, 0.06f, 0.06f, 0.07f, 1);
    img.text("755 COMPOUND - JUMP CALLS FROM COACH'S LAP", 24, 18, 4, 1, 1, 1, 1);
    int k[kKinds] = {};
    for (const Call& c : calls) ++k[c.kind];
    std::string legend;
    for (int i = 0; i < kKinds; ++i)
        if (k[i]) legend += std::string(legend.empty() ? "" : "   ") + KindName(Kind(i)) + " " + std::to_string(k[i]);
    img.text(legend, 24, 66, 3, 0.8f, 0.85f, 0.9f, 1);
    img.save(out);
}

// The ground along the line and the lap's flights over the busiest 160 m.
static void Profile(const coachhud::Sheet& sh, const Line& l, const std::vector<Call>& calls, const std::string& out,
                    float from = NAN) {
    if (calls.empty()) return;
    const float span = 160;
    float       best_s = 0;
    int         most   = -1;
    for (const Call& c : calls) {
        int n = 0;
        for (const Call& d : calls) n += d.takeoff_s >= c.takeoff_s - 10 && d.takeoff_s < c.takeoff_s - 10 + span;
        if (n > most) most = n, best_s = c.takeoff_s - 10;
    }
    if (std::isfinite(from)) best_s = from;  // a stretch asked for by name
    const float s0 = best_s, s1 = best_s + span;
    float lo = INFINITY, hi = -INFINITY;
    for (size_t i = 0; i < l.size(); ++i)
        if (l.s[i] >= s0 && l.s[i] <= s1 && std::isfinite(l.ground[i])) lo = (std::min)(lo, l.ground[i]), hi = (std::max)(hi, l.ground[i]);
    if (!std::isfinite(lo)) return;
    hi += 6, lo -= 1;
    const int W = 1800, H = 600;
    Image     img(W, H);
    for (float& c : img.rgb) c = 0.09f;
    const float sx = float(W - 80) / span, sy = (float(H) - 160) / (hi - lo) * 0.5f;  // heights x2 for legibility
    auto        X  = [&](float s) { return 40 + (s - s0) * sx; };
    auto        Y  = [&](float h) { return float(H) - 40 - (h - lo) * sy; };
    // Metre grid.
    for (float s = std::ceil(s0 / 10) * 10; s <= s1; s += 10) img.line(X(s), 100, X(s), float(H) - 40, 0.5f, 0.3f, 0.3f, 0.33f, 0.5f);
    // Ground, filled.
    for (int x = 40; x < W - 40; ++x) {
        const float g = GroundAt(l, s0 + float(x - 40) / sx);
        if (!std::isfinite(g)) continue;
        img.rect(float(x), Y(g), float(x + 1), float(H) - 40, 0.42f, 0.32f, 0.22f, 1);
    }
    // The lap's flights: the bike's height above the ground, less its usual ride height, from AIRH.
    const bool have_above = sh.air.size() == sh.ref.size() * 3;
    std::vector<float> above;
    float usual = 0.6f;
    if (have_above) {
        for (size_t i = 0; i < sh.ref.size(); ++i)
            if (std::isfinite(sh.air[i * 3 + 1])) above.push_back(sh.air[i * 3 + 1]);
        std::nth_element(above.begin(), above.begin() + long(above.size() / 2), above.end());
        usual = above[above.size() / 2];
    }
    for (const Call& c : calls) {
        if (c.takeoff_s > s1 || c.landing_s < s0) continue;
        if (c.kind != ROLL) {
            float px = NAN, py = NAN;
            for (size_t i = c.takeoff; i <= c.landing; ++i) {
                float h = NAN;
                if (have_above && std::isfinite(sh.air[i * 3 + 1])) h = l.ground[i] + (sh.air[i * 3 + 1] - usual);
                else if (!c.arc.empty()) {
                    const size_t k = (std::min)(c.arc.size() - 1, size_t(std::lround(l.s[i] - c.takeoff_s)));
                    h = l.ground[i] + c.arc[k];
                }
                if (!std::isfinite(h)) continue;
                const float x = X(l.s[i]), y = Y(h);
                if (std::isfinite(px)) img.line(px, py, x, y, 1.5f, 1, 1, 1, 0.95f);
                px = x, py = y;
            }
            const float lg = GroundAt(l, c.landing_s);
            img.rect(X(c.landing_s) - 6, Y(lg) - 3, X(c.landing_s) + 6, Y(lg) + 3, 0.55f, 0.85f, 1.0f, 1);
        }
        const float tg = GroundAt(l, c.takeoff_s);
        img.rect(X(c.takeoff_s) - 2, Y(tg) - 14, X(c.takeoff_s) + 2, Y(tg) + 2, 1, 1, 1, 1);
        const std::string t  = Label(c);
        const float       tw = TextW(t, 3);
        float             cr = 1, cg = 1, cb = 1;
        if (c.kind == ROLL) cr = 0.72f, cg = 0.86f;
        if (c.kind == JUMP_ON || c.kind == JUMP_OFF || c.kind == TABLE || c.kind == STEP_UP || c.kind == STEP_DOWN) cb = 0.35f, cg = 0.88f;
        img.text(t, X(c.takeoff_s) - tw * 0.5f, Y(tg) - 70, 3, cr, cg, cb, 1);
        const std::string hint = SpeedHint(c);
        if (!hint.empty()) img.text(hint, X(c.takeoff_s) - TextW(hint, 2) * 0.5f, Y(tg) - 44, 2, 0.8f, 0.8f, 0.8f, 1);
    }
    char title[160];
    std::snprintf(title, sizeof(title), "GROUND ALONG THE LINE %d-%d M  - HEIGHT X2 - WHITE IS COACH'S LAP IN THE AIR", int(s0), int(s1));
    img.text(title, 40, 30, 3, 1, 1, 1, 1);
    img.save(out);
}

// gluLookAt into GL world (telemetry x, y, -z), as coachline::LookAtGL.
static void RiderView(const coachhud::Sheet& sh, const Grid& g, const std::vector<Call>& calls, const std::string& out) {
    // The biggest jump: the most faces, then the longest.
    const Call* pick = nullptr;
    for (const Call& c : calls)
        if (c.kind != ROLL && (!pick || c.faces > pick->faces || (c.faces == pick->faces && c.landing_s - c.takeoff_s > pick->landing_s - pick->takeoff_s)))
            pick = &c;
    if (!pick) return;
    const auto& ref = sh.ref;
    // The rider 35 m before its lip, along the line.
    size_t ri = pick->takeoff;
    float  d  = 0;
    while (ri > 0 && d < 35.0f) d += std::hypot(ref[ri].x - ref[ri - 1].x, ref[ri].z - ref[ri - 1].z), --ri;
    // The ribbon as the plugin builds it on the track's own ground: rows every half metre, each
    // edge on the ground under it, coloured as the line.
    const std::vector<float> rgba = coachline::LineColours(sh.ref, sh.drive);
    std::vector<coachline::Vert> verts;
    float travelled = 0, next = 0;
    for (size_t i = ri; i + 1 < ref.size() && next <= coachline::kAhead; ++i) {
        const auto &a = ref[i], &b = ref[i + 1];
        const float dx = b.x - a.x, dz = b.z - a.z, len = std::hypot(dx, dz);
        if (len < 1e-3f) continue;
        const float tx = dx / len, tz = dz / len, nx = -tz, nz = tx;
        while (next <= travelled + len && next <= coachline::kAhead) {
            const float f = (next - travelled) / len, cx = a.x + dx * f, cz = a.z + dz * f;
            float col[4] = {0.3f, 0.5f, 1.0f, 0.7f};
            if (!rgba.empty())
                for (int c = 0; c < 4; ++c) col[c] = rgba[i * 4 + c] + (rgba[(i + 1) * 4 + c] - rgba[i * 4 + c]) * f;
            const float yl = g.at(cx + nx * coachline::kHalfWidth, cz + nz * coachline::kHalfWidth) + coachline::kLiftTerrain;
            const float yr = g.at(cx - nx * coachline::kHalfWidth, cz - nz * coachline::kHalfWidth) + coachline::kLiftTerrain;
            verts.push_back({cx + nx * coachline::kHalfWidth, yl, cz + nz * coachline::kHalfWidth, next, {col[0], col[1], col[2], col[3]}});
            verts.push_back({cx - nx * coachline::kHalfWidth, yr, cz - nz * coachline::kHalfWidth, next, {col[0], col[1], col[2], col[3]}});
            next += 0.5f;
        }
        travelled += len;
    }
    std::vector<coachmark::Quad> marks;
    Marks(marks, calls, AheadOf(ref, int(ri), 0.0f, coachline::kAhead + 10), verts);

    // The camera: a chase view, 3.2 m behind the bike and 3 m over its ground, looking 28 m up
    // the line. GL world is telemetry (x, y, -z).
    const auto& P   = ref[ri];
    const auto& Q   = ref[(std::min)(ri + 3, ref.size() - 1)];
    float       hx  = Q.x - P.x, hz = Q.z - P.z;
    const float hl  = std::hypot(hx, hz);
    hx /= hl, hz /= hl;
    const float ex = P.x - hx * 3.2f, ez = P.z - hz * 3.2f, ey = g.at(P.x, P.z) + 3.0f;
    const coachmark::Spot look = coachmark::At(verts, 28.0f);
    coachline::Mat4 view = coachline::LookAtGL(ex, ey, -ez, look.x, look.y + 0.5f, -look.z);
    coachline::Mat4 proj;
    const float aspect = 16.0f / 9.0f, fovy = 60.0f * 0.017453292f, n = 0.1f;
    const float t = n * std::tan(fovy * 0.5f);
    coachline::Frustum(-t * aspect, t * aspect, -t, t, n, 1000.0, proj);
    coachline::Axes ax = coachline::AxesAt(coachline::FallbackAxes());

    const int W = 1600, H = 900;
    Image     img(W, H);
    // The terrain, ray-marched per pixel, with its depth for the ribbon's depth test.
    coachline::Mat4 inv;
    coachline::Inverse(coachline::Mul(proj, view), inv);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float wx, wy, wz, fx, fy, fz;
            const float sxf = (float(x) + 0.5f) / W, syf = (float(y) + 0.5f) / H;
            coachline::Unproject(proj, view, ax, sxf, syf, 0.0f, wx, wy, wz);
            coachline::Unproject(proj, view, ax, sxf, syf, 0.5f, fx, fy, fz);
            float dx = fx - wx, dy = fy - wy, dz = fz - wz;
            const float dl = std::sqrt(dx * dx + dy * dy + dz * dz);
            dx /= dl, dy /= dl, dz /= dl;
            float* px = &img.rgb[(size_t(y) * W + x) * 3];
            float  tt = 0.3f, hit = -1;
            float  prev_gap = ey - g.at(ex, ez);
            while (tt < 400.0f) {
                const float X = ex + dx * tt, Y = ey + dy * tt, Z = ez + dz * tt;
                const float gh = g.at(X, Z);
                if (!std::isfinite(gh)) break;
                const float gap = Y - gh;
                if (gap <= 0) {
                    // Refine between the last step and this one.
                    const float step = (std::max)(0.05f, tt * 0.004f);
                    hit = tt - step * gap / (gap - prev_gap);
                    break;
                }
                prev_gap = gap;
                tt += (std::max)(0.05f, tt * 0.004f);
            }
            if (hit < 0) {
                const float k = (std::min)(1.0f, float(y) / (H * 0.5f));
                px[0] = 0.48f + 0.22f * k, px[1] = 0.64f + 0.16f * k, px[2] = 0.88f + 0.06f * k;
                continue;
            }
            const float X = ex + dx * hit, Z = ez + dz * hit;
            const float h0 = g.at(X, Z), h1 = g.at(X + 0.3f, Z), h2 = g.at(X, Z + 0.3f);
            const float nx = -(h1 - h0) / 0.3f, nz = -(h2 - h0) / 0.3f, nl = std::sqrt(nx * nx + 1 + nz * nz);
            const float l  = (std::max)(0.15f, (0.4f * nx + 0.8f + 0.45f * nz) / nl);
            const float haze = (std::min)(1.0f, hit / 260.0f);
            // Dirt, with a faint 5 m grid so the shape of the ground reads.
            const bool grid = std::fabs(X / 5 - std::round(X / 5)) < 0.004f * hit + 0.01f || std::fabs(Z / 5 - std::round(Z / 5)) < 0.004f * hit + 0.01f;
            float r = 0.50f * l, gg = 0.38f * l, b = 0.26f * l;
            if (grid) r *= 0.88f, gg *= 0.88f, b *= 0.88f;
            px[0] = r + (0.72f - r) * haze, px[1] = gg + (0.78f - gg) * haze, px[2] = b + (0.88f - b) * haze;
            // The depth the plugin's depth test would see: distance along the view direction.
            coachline::Screen s = coachline::Project(proj, view, X, h0, Z, ax);
            img.depth[size_t(y) * W + x] = s.ok ? s.depth : hit;
        }
    auto put = [&](float x, float y, float z, float* o) {
        const coachline::Screen s = coachline::Project(proj, view, x, y, z, ax);
        o[0] = s.x * W, o[1] = s.y * H, o[2] = s.depth;
        return s.ok;
    };
    // The ribbon, faded with distance as the plugin fades it.
    for (size_t i = 0; i + 3 < verts.size(); i += 2) {
        float a[3], b[3], c[3], e[3];
        if (!put(verts[i].x, verts[i].y, verts[i].z, a) || !put(verts[i + 1].x, verts[i + 1].y, verts[i + 1].z, b) ||
            !put(verts[i + 2].x, verts[i + 2].y, verts[i + 2].z, c) || !put(verts[i + 3].x, verts[i + 3].y, verts[i + 3].z, e))
            continue;
        float col[4] = {verts[i].rgba[0], verts[i].rgba[1], verts[i].rgba[2], verts[i].rgba[3] * coachline::Fade(verts[i].s)};
        img.tri(a, b, c, col);
        img.tri(b, e, c, col);
    }
    // And the marks, in the order the plugin draws them.
    for (const coachmark::Quad& q : marks) {
        float c[4][3];
        bool  ok = true;
        for (int k = 0; k < 4; ++k) ok = ok && put(q.p[k][0], q.p[k][1], q.p[k][2], c[k]);
        if (!ok) continue;
        img.tri(c[0], c[1], c[2], q.rgba, 0.8f);
        img.tri(c[0], c[2], c[3], q.rgba, 0.8f);
    }
    char cap[160];
    std::snprintf(cap, sizeof(cap), "CHASE VIEW 35 M BEFORE THE LIP - %s OVER %d, %.0f M FLIGHT", Label(*pick).c_str(), pick->faces,
                  double(pick->landing_s - pick->takeoff_s));
    img.rect(0, 0, float(W), 44, 0.05f, 0.05f, 0.06f, 0.75f);
    img.text(cap, 16, 10, 3, 1, 1, 1, 1);
    img.save(out);
}

// A supercross rhythm lane on a synthetic grid: a triple, a double, a table jumped on and off.
static float Mound(float s, float c, float h = 1.6f) {
    if (s >= c - 3 && s <= c) return h * (s - (c - 3)) / 3;
    if (s > c && s <= c + 5) return h * (1 - (s - c) / 5);
    return 0;
}
static float Lane(float s) {
    float h = 0;
    for (float c : {100.0f, 108.0f, 116.0f, 140.0f, 148.0f}) h = (std::max)(h, Mound(s, c));
    if (s >= 172 && s < 175) h = (std::max)(h, 1.5f * (s - 172) / 3);
    if (s >= 175 && s <= 188) h = (std::max)(h, 1.5f);
    if (s > 188 && s <= 192) h = (std::max)(h, 1.5f * (1 - (s - 188) / 4));
    return h;
}
static void Synthetic(coachhud::Sheet& sh, Grid& g) {
    g.w = 1400, g.h = 200, g.mps = 0.25f;
    g.v.resize(size_t(g.w) * g.h);
    for (uint32_t r = 0; r < g.h; ++r)
        for (uint32_t c = 0; c < g.w; ++c) g.v[size_t(r) * g.w + c] = Lane(float(c) * g.mps - 20.0f);
    const float len = 300, v = 13;
    struct A { float a, b; } air[] = {{100, 120}, {140, 152}, {167, 179}, {188, 196}};
    sh.track_len = len;
    for (float m = 20; m <= 320; m += 0.5f) {
        sh.ref.push_back({(m - 20) / len, (m - 20) / v, m + 20.0f, 25.0f});
        bool up = false;
        for (const A& x : air) up = up || (m > x.a && m < x.b);
        const float gh = Lane(m), above = up ? 2.4f : 0.6f;
        sh.air.insert(sh.air.end(), {gh + above, above, up ? 1.0f : 0.0f});
        sh.drive.insert(sh.drive.end(), {v, 1.0f, 0.0f});
        for (int k = 0; k < 3; ++k) sh.terrain.push_back(gh);
    }
    sh.terrain_k = 3, sh.terrain_step = 0.5f;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--synthetic") {
        coachhud::Sheet sh;
        Grid            g;
        Synthetic(sh, g);
        const Line              l     = MakeLine(sh);
        const std::vector<Call> calls = Calls(l);
        std::printf("%s\n", Summary(l, calls).c_str());
        Profile(sh, l, calls, std::string(argv[2]) + "-profile.ppm");
        RiderView(sh, g, calls, std::string(argv[2]) + "-rider.ppm");
        return 0;
    }
    if (argc < 4) {
        std::printf("usage: coachjump_preview <sheet.hud> <grid.bin> <out-prefix>\n");
        return 0;  // built with the tests; nothing to do without a sheet
    }
    const auto      b = ReadFile(argv[1]);
    coachhud::Sheet sh;
    if (!coachhud::Parse(b.data(), b.size(), sh)) {
        std::printf("%s doesn't read as a sheet\n", argv[1]);
        return 1;
    }
    Grid g;
    if (!g.load(argv[2])) {
        std::printf("%s doesn't read as a grid\n", argv[2]);
        return 1;
    }
    const Line              l     = MakeLine(sh);
    const std::vector<Call> calls = Calls(l);
    std::printf("%s\n", Summary(l, calls).c_str());
    for (const Call& c : calls)
        std::printf("  %-8s at %6.1f m -> %6.1f m, %4.2f s, %2d faces, %s, %.0f km/h\n", KindName(c.kind), double(c.takeoff_s),
                    double(c.landing_s), double(c.airtime), c.faces, c.predicted ? "predicted" : "recorded",
                    double(c.speed * 3.6f));
    const std::string p = argv[3];
    TopDown(sh, g, calls, p + "-topdown.ppm");
    Profile(sh, l, calls, p + "-profile.ppm", argc > 4 ? float(std::atof(argv[4])) : NAN);
    RiderView(sh, g, calls, p + "-rider.ppm");
    return 0;
}
