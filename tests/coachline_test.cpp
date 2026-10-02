// The ground line's maths (src/coachline.h): the camera capture, projection, height and ribbon,
// against known camera poses and against a real Coach sheet. Pure C++, runs anywhere.
//
//   coachline_test               the checks
//   coachline_test out.ppm       also renders the projected ribbon over a synthetic view, for a
//                                person to look at (the PR's preview PNG is made from this)

#include "../src/coachline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef COACHLINE_DATA
#define COACHLINE_DATA "tests/data"
#endif

static int g_failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            ++g_failures;                                    \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n  (%s)\n", #cond);                \
        }                                                    \
    } while (0)

using namespace coachline;

// The world pass MX Bikes sets up, exactly as FrostMod's diagnostic logged it.
static Mat4 GameFrustum() {
    Mat4 p;
    Frustum(-0.018, 0.018, -0.010, 0.010, 0.023, 1000.0, p);
    return p;
}

// gluLookAt: a camera at `eye` looking at `at`, y up.
static Mat4 LookAt(float ex, float ey, float ez, float ax, float ay, float az) {
    float fx = ax - ex, fy = ay - ey, fz = az - ez;
    const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    fx /= fl, fy /= fl, fz /= fl;
    // s = f x up(0,1,0)
    float sx = -fz, sz = fx;
    const float sy = 0, sl = std::sqrt(sx * sx + sz * sz);
    sx /= sl, sz /= sl;
    // u = s x f
    const float ux = sy * fz - sz * fy, uy = sz * fx - sx * fz, uz = sx * fy - sy * fx;
    Mat4 v;
    v.m[0] = sx, v.m[4] = sy, v.m[8] = sz;
    v.m[1] = ux, v.m[5] = uy, v.m[9] = uz;
    v.m[2] = -fx, v.m[6] = -fy, v.m[10] = -fz;
    v.m[3] = v.m[7] = v.m[11] = 0;
    v.m[12] = -(sx * ex + sy * ey + sz * ez);
    v.m[13] = -(ux * ex + uy * ey + uz * ez);
    v.m[14] = (fx * ex + fy * ey + fz * ez);
    v.m[15] = 1;
    return v;
}

static void PutF(unsigned char* b, float f) { std::memcpy(b, &f, 4); }

// A track of straight segments, `seg` metres each, `total` long, its height a gentle 5 m swell.
static float SwellAt(float c) { return 5.0f * std::sin(c / 400.0f * 6.2831853f); }
static bool  BuildTrack(coachhud::Track& t, float total, float seg) {
    const int n = int(std::ceil(total / seg));
    std::vector<unsigned char> b(size_t(n) * 28, 0);
    for (int i = 0; i < n; ++i) {
        unsigned char* r = &b[size_t(i) * 28];
        PutF(r + 4, (std::min)(seg, total - seg * float(i)));
        PutF(r + 24, SwellAt(seg * float(i)));
    }
    return t.build(n, b.data(), 28, nullptr);
}

static std::vector<unsigned char> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------------------
// The offline preview: sky, a dirt ground plane with a 5 m grid seen through the same camera,
// the ribbon on top, and a dark block where the bike sits. Binary PPM: no image library.
struct Image {
    int                w, h;
    std::vector<float> rgb;
    Image(int W, int H) : w(W), h(H), rgb(size_t(W) * size_t(H) * 3, 0) {}
    void blend(int x, int y, float r, float g, float b, float a) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        float* p = &rgb[(size_t(y) * size_t(w) + size_t(x)) * 3];
        p[0] += (r - p[0]) * a, p[1] += (g - p[1]) * a, p[2] += (b - p[2]) * a;
    }
    void tri(float ax, float ay, float bx, float by, float cx, float cy, float r, float g, float b, float a) {
        const int   x0 = (std::max)(0, int(std::floor((std::min)({ax, bx, cx}))));
        const int   x1 = (std::min)(w - 1, int(std::ceil((std::max)({ax, bx, cx}))));
        const int   y0 = (std::max)(0, int(std::floor((std::min)({ay, by, cy}))));
        const int   y1 = (std::min)(h - 1, int(std::ceil((std::max)({ay, by, cy}))));
        const float d  = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
        if (std::fabs(d) < 1e-6f) return;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float px = float(x) + 0.5f, py = float(y) + 0.5f;
                const float u  = ((bx - px) * (cy - py) - (cx - px) * (by - py)) / d;
                const float v  = ((cx - px) * (ay - py) - (ax - px) * (cy - py)) / d;
                if (u >= 0 && v >= 0 && u + v <= 1) blend(x, y, r, g, b, a);
            }
    }
    bool save(const char* path) const {
        FILE* f = std::fopen(path, "wb");
        if (!f) return false;
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (float c : rgb) std::fputc(int((std::max)(0.0f, (std::min)(1.0f, c)) * 255.0f + 0.5f), f);
        std::fclose(f);
        return true;
    }
};

static void RenderPreview(const char* path, const Mat4& p, const Mat4& v, const std::vector<Vert>& verts, float rx,
                          float ry, float rz, float ground_y) {
    Image img(1280, 720);
    float ex, ey, ez;
    CameraPos(v, ex, ey, ez);
    const float tx = 1.0f / p.m[0], ty = 1.0f / p.m[5];
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) {
            const float  nx = (float(x) + 0.5f) / float(img.w) * 2 - 1, ny = 1 - (float(y) + 0.5f) / float(img.h) * 2;
            const float  ex_ = nx * tx, ey_ = ny * ty, ez_ = -1;  // the ray in eye space
            const float* m   = v.m;                                // to world: the view's transpose
            const float  dx  = m[0] * ex_ + m[1] * ey_ + m[2] * ez_;
            const float  dy  = m[4] * ex_ + m[5] * ey_ + m[6] * ez_;
            const float  dz  = m[8] * ex_ + m[9] * ey_ + m[10] * ez_;
            float*       px  = &img.rgb[(size_t(y) * size_t(img.w) + size_t(x)) * 3];
            if (dy >= -1e-4f) {
                const float k = (std::min)(1.0f, float(y) / (float(img.h) * 0.5f));
                px[0] = 0.45f + 0.25f * k, px[1] = 0.62f + 0.18f * k, px[2] = 0.85f + 0.08f * k;
                continue;
            }
            const float t = (ground_y - ey) / dy;
            const float gx = ex + dx * t, gz = ez + dz * t;
            const float tol  = 0.0025f * t + 0.004f;
            const bool  grid = std::fabs(gx / 5.0f - std::round(gx / 5.0f)) < tol ||
                              std::fabs(gz / 5.0f - std::round(gz / 5.0f)) < tol;
            const float haze = (std::min)(1.0f, t / 220.0f);
            float r = 0.47f, g = 0.36f, b = 0.25f;  // dirt
            if (grid) r *= 0.82f, g *= 0.82f, b *= 0.82f;
            px[0] = r + (0.70f - r) * haze, px[1] = g + (0.76f - g) * haze, px[2] = b + (0.86f - b) * haze;
        }
    const std::vector<ScreenRow> rows = ProjectRibbon(p, v, Axes{}, verts);
    auto X = [&](const Screen& s) { return s.x * float(img.w); };
    auto Y = [&](const Screen& s) { return s.y * float(img.h); };
    for (size_t i = 0; i + 1 < rows.size(); ++i) {
        const ScreenRow& a  = rows[i];
        const ScreenRow& b  = rows[i + 1];
        const float      al = 0.70f * (a.alpha + b.alpha) * 0.5f;
        const float cr = a.rgba[0], cg = a.rgba[1], cb = a.rgba[2];
        img.tri(X(a.l), Y(a.l), X(a.r), Y(a.r), X(b.l), Y(b.l), cr, cg, cb, al);
        img.tri(X(a.r), Y(a.r), X(b.r), Y(b.r), X(b.l), Y(b.l), cr, cg, cb, al);
    }
    const Screen s = Project(p, v, rx, ry, rz);
    if (s.ok)
        for (int y = -60; y <= 20; ++y)
            for (int x = -16; x <= 16; ++x)
                img.blend(int(X(s)) + x, int(Y(s)) + y, 0.10f, 0.10f, 0.12f, 0.85f);
    if (img.save(path)) std::printf("preview written to %s\n", path);
}

// A triangle with a colour per corner, blended across it the way GL's smooth shading does.
static void Tri3(Image& img, const float* a, const float* b, const float* c, const float* ca, const float* cb,
                 const float* cc, float alpha) {
    const int   x0 = (std::max)(0, int(std::floor((std::min)({a[0], b[0], c[0]}))));
    const int   x1 = (std::min)(img.w - 1, int(std::ceil((std::max)({a[0], b[0], c[0]}))));
    const int   y0 = (std::max)(0, int(std::floor((std::min)({a[1], b[1], c[1]}))));
    const int   y1 = (std::min)(img.h - 1, int(std::ceil((std::max)({a[1], b[1], c[1]}))));
    const float d  = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
    if (std::fabs(d) < 1e-6f) return;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float px = float(x) + 0.5f, py = float(y) + 0.5f;
            const float u  = ((b[0] - px) * (c[1] - py) - (c[0] - px) * (b[1] - py)) / d;
            const float v  = ((c[0] - px) * (a[1] - py) - (a[0] - px) * (c[1] - py)) / d;
            const float w  = 1 - u - v;
            if (u < 0 || v < 0 || w < 0) continue;
            img.blend(x, y, u * ca[0] + v * cb[0] + w * cc[0], u * ca[1] + v * cb[1] + w * cc[1],
                      u * ca[2] + v * cb[2] + w * cc[2], alpha);
        }
}

// A 5x7 pixel font, just the letters the legend uses.
static const char* Glyph(char ch) {
    switch (ch) {
        case 'A': return " ### #   ##   #######   ##   ##   #";
        case 'B': return "#### #   ##   ##### #   ##   ##### ";
        case 'C': return " ### #   ##    #    #    #   # ### ";
        case 'D': return "#### #   ##   ##   ##   ##   ##### ";
        case 'E': return "######    #    #### #    #    #####";
        case 'F': return "######    #    #### #    #    #    ";
        case 'G': return " ### #   ##    # ####   ##   # ####";
        case 'H': return "#   ##   ##   #######   ##   ##   #";
        case 'I': return " ###   #    #    #    #    #   ### ";
        case 'K': return "#   ##  # # #  ##   # #  #  # #   #";
        case 'L': return "#    #    #    #    #    #    #####";
        case 'M': return "#   ### ### # ##   ##   ##   ##   #";
        case 'N': return "#   ###  ## # ##  ###   ##   ##   #";
        case 'O': return " ### #   ##   ##   ##   ##   # ### ";
        case 'P': return "#### #   ##   ##### #    #    #    ";
        case 'R': return "#### #   ##   ##### # #  #  # #   #";
        case 'S': return " #####    #     ###     #    ##### ";
        case 'T': return "#####  #    #    #    #    #    #  ";
        case 'U': return "#   ##   ##   ##   ##   ##   # ### ";
        case 'V': return "#   ##   ##   ##   ##   # # #   #  ";
        case 'Y': return "#   ##   # # #   #    #    #    #  ";
        case 'Z': return "#####    #   #   #   #   #    #####";
        case '5': return "######    ####     #    ##   # ### ";
        case '7': return "#####    #   #   #   #    #    #   ";
        case '-': return "               #####               ";
        default: return "                                   ";
    }
}
static void Text(Image& img, int x, int y, const char* s, int scale, float r, float g, float b) {
    for (; *s; ++s, x += 6 * scale) {
        const char* gl = Glyph(*s);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (gl[row * 5 + col] == '#')
                    for (int dy = 0; dy < scale; ++dy)
                        for (int dx = 0; dx < scale; ++dx) img.blend(x + col * scale + dx, y + row * scale + dy, r, g, b, 1);
    }
}

// The lap from above, north up, coloured by its tone with a colour per point blended between,
// a ring where each braking zone starts, a legend, and a zoomed crop of one transition.
static void RenderToneMap(const char* path, const coachhud::Sheet& sh, const std::vector<float>& tone,
                          const std::vector<std::pair<size_t, size_t>>& zones) {
    Image img(1660, 1200);
    for (float& c : img.rgb) c = 0.16f;
    float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
    for (const auto& r : sh.ref) {
        x0 = (std::min)(x0, r.x), x1 = (std::max)(x1, r.x);
        z0 = (std::min)(z0, r.z), z1 = (std::max)(z1, r.z);
    }
    // Draws the lap into a box: world (cx, cz) at the box centre, `sc` px a metre, `half` the
    // painted half width in metres.
    auto draw = [&](float bx, float by, float bw, float bh, float cx, float cz, float sc, float half) {
        auto P = [&](float x, float z, float out[2]) {
            out[0] = bx + bw * 0.5f + (x - cx) * sc;
            out[1] = by + bh * 0.5f - (z - cz) * sc;
        };
        for (size_t i = 0; i + 1 < sh.ref.size(); ++i) {
            const auto& a = sh.ref[i];
            const auto& c = sh.ref[i + 1];
            const float dx = c.x - a.x, dz = c.z - a.z, l = std::hypot(dx, dz);
            if (l < 1e-3f || l > kMaxGapM) continue;
            const float nx = -dz / l * half, nz = dx / l * half;
            float pa[2], pb[2], pc[2], pd[2], ca[3], cc[3];
            P(a.x + nx, a.z + nz, pa), P(a.x - nx, a.z - nz, pb), P(c.x + nx, c.z + nz, pc), P(c.x - nx, c.z - nz, pd);
            // Only segments wholly inside the box: the crop must not spill onto the map.
            if ((std::min)({pa[0], pb[0], pc[0], pd[0]}) < bx || (std::max)({pa[0], pb[0], pc[0], pd[0]}) > bx + bw ||
                (std::min)({pa[1], pb[1], pc[1], pd[1]}) < by || (std::max)({pa[1], pb[1], pc[1], pd[1]}) > by + bh)
                continue;
            ToneColour(tone[i], ca[0], ca[1], ca[2]);
            ToneColour(tone[i + 1], cc[0], cc[1], cc[2]);
            Tri3(img, pa, pb, pc, ca, ca, cc, 0.95f);
            Tri3(img, pb, pd, pc, ca, cc, cc, 0.95f);
        }
    };
    const float sc = 1100.0f / (std::max)(x1 - x0, z1 - z0);
    draw(0, 0, 1200, 1200, (x0 + x1) * 0.5f, (z0 + z1) * 0.5f, sc, 3.5f / sc * 1.0f + 0.35f);
    auto MX = [&](float x) { return 600.0f + (x - (x0 + x1) * 0.5f) * sc; };
    auto MY = [&](float z) { return 600.0f - (z - (z0 + z1) * 0.5f) * sc; };
    for (const auto& z : zones)
        for (int dy = -8; dy <= 8; ++dy)
            for (int dx = -8; dx <= 8; ++dx)
                if (dx * dx + dy * dy <= 64 && dx * dx + dy * dy >= 36)
                    img.blend(int(MX(sh.ref[z.first].x)) + dx, int(MY(sh.ref[z.first].z)) + dy, 1, 1, 1, 1);
    // Legend, top right: the gradient bar and the four names under its stops.
    Text(img, 1250, 30, "755 COMPOUND", 3, 0.9f, 0.9f, 0.9f);
    for (int x = 0; x < 380; ++x) {
        float r, g, b;
        ToneColour(float(x) / 379.0f * 3.0f, r, g, b);
        for (int y = 0; y < 26; ++y) img.blend(1260 + x, 80 + y, r, g, b, 1);
    }
    Text(img, 1252, 116, "GAS", 2, 0.85f, 0.85f, 0.85f);
    Text(img, 1358, 116, "COAST", 2, 0.85f, 0.85f, 0.85f);
    Text(img, 1458, 116, "LIGHT", 2, 0.85f, 0.85f, 0.85f);
    Text(img, 1554, 116, "HEAVY", 2, 0.85f, 0.85f, 0.85f);
    Text(img, 1458, 138, "BRAKE", 2, 0.85f, 0.85f, 0.85f);
    Text(img, 1554, 138, "BRAKE", 2, 0.85f, 0.85f, 0.85f);
    // The zoomed crop: 40 m around the start of the first braking zone, at 9 px a metre and the
    // ribbon's real 0.7 m width, so the fade between colours is visible at the scale it is drawn.
    if (!zones.empty()) {
        const auto& at = sh.ref[zones[0].first];
        const float zx = 1250, zy = 200, zw = 370, zh = 370, zsc = 9.0f;
        for (int y = 0; y < int(zh); ++y)
            for (int x = 0; x < int(zw); ++x) img.blend(int(zx) + x, int(zy) + y, 0.36f, 0.27f, 0.18f, 1);  // dirt
        draw(zx, zy, zw, zh, at.x, at.z, zsc, kHalfWidth);
        for (int k = 0; k < int(zw); ++k) {
            img.blend(int(zx) + k, int(zy), 1, 1, 1, 1), img.blend(int(zx) + k, int(zy + zh), 1, 1, 1, 1);
        }
        for (int k = 0; k < int(zh); ++k) {
            img.blend(int(zx), int(zy) + k, 1, 1, 1, 1), img.blend(int(zx + zw), int(zy) + k, 1, 1, 1, 1);
        }
        Text(img, int(zx), int(zy + zh) + 10, "ZOOM - FIRST BRAKE ZONE", 2, 0.85f, 0.85f, 0.85f);
        // Where the crop is on the map.
        const float hw = zw / zsc * 0.5f * sc;
        for (int k = -int(hw); k <= int(hw); ++k) {
            const float mx = MX(at.x), my = MY(at.z);
            img.blend(int(mx) + k, int(my - hw), 1, 1, 1, 0.8f), img.blend(int(mx) + k, int(my + hw), 1, 1, 1, 0.8f);
            img.blend(int(mx - hw), int(my) + k, 1, 1, 1, 0.8f), img.blend(int(mx + hw), int(my) + k, 1, 1, 1, 0.8f);
        }
    }
    if (img.save(path)) std::printf("tone map written to %s\n", path);
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // --- The game's own projection ------------------------------------------------------
    const Mat4 p = GameFrustum();
    CHECK(ValidProjection(p), "the logged world frustum is a perspective");
    const float fovy = 2.0f * std::atan(1.0f / p.m[5]) * 57.2957795f;
    CHECK(std::fabs(fovy - 47.0f) < 1.0f, "the logged frustum is a 47 degree vertical fov, got %f", fovy);
    CHECK(std::fabs(Aspect(p) - 1.8f) < 0.01f, "and 16:9 (as logged, to three places), got %f", Aspect(p));
    Mat4 cube;
    CHECK(Frustum(-0.018, 0.018, -0.018, 0.018, 0.018, 100.0, cube) && std::fabs(Aspect(cube) - 1.0f) < 1e-4f,
          "the environment cube's frustum is square");
    Mat4 bad;
    CHECK(!Frustum(-1, 1, -1, 1, 0, 10, bad), "a zero near plane is refused, as GL refuses it");
    Mat4 ortho;
    ortho.m[0] = 0.002f, ortho.m[5] = 0.002f, ortho.m[10] = -0.002f, ortho.m[15] = 1;
    CHECK(!ValidProjection(ortho), "an ortho HUD pass is refused");
    Mat4 nan = p;
    nan.m[0] = NAN;
    CHECK(!ValidProjection(nan), "NaN is refused");

    // --- Known camera poses: 2 m up at z = 10, looking toward -z ---------------------------
    const Mat4 v = LookAt(0, 2, 10, 0, 2, 0);
    CHECK(ValidView(v), "a look-at view is rigid");
    float cx, cy, cz;
    CameraPos(v, cx, cy, cz);
    CHECK(std::fabs(cx) < 1e-4f && std::fabs(cy - 2) < 1e-4f && std::fabs(cz - 10) < 1e-4f, "camera at %f %f %f", cx,
          cy, cz);
    Mat4 scaled = v;
    scaled.m[0] = 2.0f;
    CHECK(!ValidView(scaled), "a scaled model matrix is not a camera");
    Mat4 sheared = v;
    sheared.m[4] = 0.3f;
    CHECK(!ValidView(sheared), "a sheared one is not either");
    CHECK(!ValidView(p), "a projection is not a view");

    // A point straight ahead lands at the centre of the screen, at its distance.
    const Screen mid = Project(p, v, 0, 2, 0);
    CHECK(mid.ok && std::fabs(mid.x - 0.5f) < 1e-4f && std::fabs(mid.y - 0.5f) < 1e-4f, "centre %f %f", mid.x, mid.y);
    CHECK(std::fabs(mid.depth - 10.0f) < 1e-3f, "depth %f", mid.depth);
    // A turned camera: from (5, 3, 5) looking at (25, 3, -15), the target is still dead centre.
    const Mat4   turned = LookAt(5, 3, 5, 25, 3, -15);
    const Screen tc     = Project(p, turned, 25, 3, -15);
    CHECK(tc.ok && std::fabs(tc.x - 0.5f) < 1e-4f && std::fabs(tc.y - 0.5f) < 1e-4f, "turned centre %f %f", tc.x,
          tc.y);
    // Behind the camera, and level with it, are culled.
    CHECK(!Project(p, v, 0, 2, 20).ok, "behind the camera is culled");
    CHECK(!Project(p, v, 3, 2, 10).ok, "beside the camera, in its own plane, is culled");
    CHECK(!Project(p, turned, -15, 3, 25).ok, "behind a turned camera is culled");
    // Up is up and right is right.
    const Screen low = Project(p, v, 0, 0, 0);
    CHECK(low.ok && low.y > 0.5f, "ground below the eye is lower on screen, y=%f", low.y);
    CHECK(Project(p, v, 3, 2, 0).x > 0.5f, "world +x is right of centre looking toward -z");

    // The game's composition: frustum on PROJECTION, the camera multiplied onto it.
    const Mat4   pv  = Mul(p, v);
    const Mat4   id;
    const Screen via = Project(pv, id, 1, 1, 3);
    const Screen dir = Project(p, v, 1, 1, 3);
    CHECK(via.ok && std::fabs(via.x - dir.x) < 1e-4f && std::fabs(via.y - dir.y) < 1e-4f, "P*V == P then V");

    // A ribbon's width on screen shrinks with distance, in proportion: rows 5, 10, 20, 40 m on.
    float last_w = 1e9f;
    for (float d : {5.0f, 10.0f, 20.0f, 40.0f}) {
        const Screen l = Project(p, v, -kHalfWidth, 0, 10 - d), r = Project(p, v, kHalfWidth, 0, 10 - d);
        const float  w = r.x - l.x;
        CHECK(l.ok && r.ok && w > 0 && w < last_w, "width at %.0f m: %f (closer was %f)", d, w, last_w);
        if (last_w < 1e9f) CHECK(std::fabs(w * 2 - last_w) < 1e-3f, "width halves: %f vs %f", w, last_w);
        last_w = w;
    }

    // --- Picking the camera out of a frame -------------------------------------------------
    // A frame as the game issues it: six cube faces round the bike, then the world pass twice
    // (a depth pre-pass and the colour pass), on a 16:9 window.
    const float            rx = 0, ry = 0.6f, rz = 0;  // the rider
    std::vector<Candidate> frame;
    for (int i = 0; i < 6; ++i) frame.push_back({cube, LookAt(0, 1, 0, float(i), 1, 5)});
    frame.push_back({p, LookAt(0, 2.2f, 4, 0, 1, -6)});
    frame.push_back({p, LookAt(0, 2.2f, 4, 0, 1, -6)});
    Pick pk = PickCamera(frame, 2560.0f / 1440.0f, rx, ry, rz);
    CHECK(pk.index == 7 && pk.axes == 0, "the world pass is picked, not a cube face: %d",
          pk.index);
    CHECK(PickCamera(frame, 4.0f / 3.0f, rx, ry, rz).index < 0, "nothing fits a 4:3 window");
    const std::vector<Candidate> tv = {{p, LookAt(80, 10, 0, 0, 0, 0)}};
    CHECK(PickCamera(tv, 16.0f / 9.0f, rx, ry, rz).index < 0, "a TV camera 80 m away is refused");
    // A world with z mirrored: the camera only sits next to the rider once z is flipped.
    const std::vector<Candidate> flipped = {{p, LookAt(0, 2, -104, 0, 1, -94)}};
    pk = PickCamera(flipped, 16.0f / 9.0f, 0, 0.6f, 100);
    CHECK(pk.index == 0 && pk.axes >= 0 && AxesName(AxesAt(pk.axes)) == "x,y,-z", "a z-mirrored world is recognised: %s",
          pk.axes >= 0 ? AxesName(AxesAt(pk.axes)).c_str() : "none");

    // --- Real-shaped captures, as the game assembles them on GL_PROJECTION -------------------
    // The bike where Sean rides, far from the origin; a chase camera 4 m behind and 1.8 m up.
    {
        const float bx = 243.7f, by = 12.4f, bz = 131.5f;
        const float ex = bx - 4.0f, ey = by + 1.8f, ez = bz;  // looking east along +x
        // (a) The view handed over whole, as gluLookAt builds it.
        const Mat4 whole = LookAt(ex, ey, ez, bx + 8, by, bz);
        // (b) The same camera assembled the classic way: rotations, then glTranslate(-eye).
        //     Pitch down, then yaw so the eye's -z looks along world +x.
        const float pitch = std::atan2(ey - by, 12.0f) * 57.2957795f;
        Mat4 built = Mul(Rotate(pitch, 1, 0, 0), Rotate(90.0f, 0, 1, 0));
        built      = Mul(built, Translate(-ex, -ey, -ez));
        CHECK(ValidView(built), "a rotate-then-translate camera is rigid");
        float cx2, cy2, cz2;
        CameraPos(built, cx2, cy2, cz2);
        CHECK(std::fabs(cx2 - ex) < 1e-3f && std::fabs(cy2 - ey) < 1e-3f && std::fabs(cz2 - ez) < 1e-3f,
              "and the camera is read back at the eye: %f %f %f", cx2, cy2, cz2);
        const Screen ahead = Project(p, built, bx + 8, by, bz);
        CHECK(ahead.ok && std::fabs(ahead.x - 0.5f) < 1e-3f, "a point dead ahead is centred across, x=%f", ahead.x);
        // Only the rotation, the translation missed (what v0.42.0 kept): a camera at the origin,
        // far from the bike, and nothing is picked under any axes.
        const Mat4 rot_only = Mul(Rotate(pitch, 1, 0, 0), Rotate(90.0f, 0, 1, 0));
        CHECK(PickCamera({{p, rot_only}}, 16.0f / 9.0f, bx, by, bz).index < 0, "a rotation alone is no camera here");
        for (const Mat4& view : {whole, built}) {
            const Pick k = PickCamera({{p, view}}, 2560.0f / 1440.0f, bx, by, bz);
            CHECK(k.index == 0 && k.axes == 0 && k.dist > 4.0f && k.dist < 4.5f, "picked with plain axes at %f m",
                  k.dist);
        }
        // A z-up GL world: GL (x, y, z) = telemetry (x, -z, y). The same camera, expressed there.
        Axes zup;
        zup.src[0] = 0, zup.src[1] = 2, zup.src[2] = 1;
        zup.sgn[0] = 1, zup.sgn[1] = -1, zup.sgn[2] = 1;
        float gx, gy, gz, tx, ty, tz;
        zup.apply(ex, ey, ez, gx, gy, gz);
        zup.apply(bx + 8, by, bz, tx, ty, tz);
        // look-at with z up: build it by hand from the y-up LookAt in GL coordinates rotated
        // -90 degrees about x (y-up to z-up).
        const Mat4 zview = Mul(LookAt(gx, gz, -gy, tx, tz, -ty), Rotate(-90.0f, 1, 0, 0));
        float zx, zy, zz;
        CameraPos(zview, zx, zy, zz);
        CHECK(std::fabs(zx - gx) < 1e-2f && std::fabs(zy - gy) < 1e-2f && std::fabs(zz - gz) < 1e-2f,
              "z-up camera at %f %f %f, want %f %f %f", zx, zy, zz, gx, gy, gz);
        const Pick zk = PickCamera({{p, zview}}, 16.0f / 9.0f, bx, by, bz);
        CHECK(zk.index == 0 && zk.axes >= 0 && AxesName(AxesAt(zk.axes)) == "x,-z,y", "a z-up world is found: %s",
              zk.axes >= 0 ? AxesName(AxesAt(zk.axes)).c_str() : "none");
        if (zk.axes >= 0) {
            const Screen zs = Project(p, zview, bx + 8, by, bz, AxesAt(zk.axes));
            CHECK(zs.ok && std::fabs(zs.x - 0.5f) < 1e-3f && std::fabs(zs.y - 0.5f) < 1e-3f,
                  "and through it the look-at point lands at the centre: %f %f", zs.x, zs.y);
        }
        // The lock: thirty frames of the same axes, then kept; a miss in between restarts it.
        AxesLock lock;
        bool took = false;
        for (int f = 0; f < kLockFrames - 1; ++f) took |= lock.vote(0);
        CHECK(!took && lock.locked() < 0, "not locked before %d frames", kLockFrames);
        lock.vote(-1);
        for (int f = 0; f < kLockFrames - 1; ++f) lock.vote(0);
        CHECK(lock.locked() < 0, "a frame without a camera restarts the count");
        CHECK(lock.vote(0) && lock.locked() == 0, "locked on the thirtieth");
        lock.vote(5);
        CHECK(lock.locked() == 0, "and kept");
        // With the axes locked, only those are tried.
        CHECK(PickCamera({{p, zview}}, 16.0f / 9.0f, bx, by, bz, 0).index < 0, "locked to plain axes, a z-up camera is not taken");
    }
    // --- The camera from a synthetic shader-uniform stream ---------------------------------
    // What v0.42.1 saw in the game: the fixed-function "camera" is a constant identity with z
    // flipped while the bike moves. The real one goes to the shaders. One frame's uploads:
    //   p3/l0  an MVP per object (scenery all over the track; one location, many uploads)
    //   p7/l1  the bike's model matrix (rigid, at the bike)
    //   p7/l2  the view-projection
    //   p9/l4  the view on its own, uploaded row-major (as a row-vector shader would want it)
    // plus that constant fixed-function matrix, which must never be taken for the camera.
    {
        Mat4 P;
        {
            const float f = 1.0f / std::tan(30.0f * 0.017453292f), n = 0.1f, fa = 2000.0f;
            for (float& x : P.m) x = 0;
            P.m[0] = f / (16.0f / 9.0f), P.m[5] = f, P.m[10] = (fa + n) / (n - fa), P.m[11] = -1;
            P.m[14] = 2 * fa * n / (n - fa);
        }
        Mat4 ffz;  // identity, z flipped: what the fixed-function capture kept finding
        ffz.m[10] = -1;
        KeyLock lock;
        uint64_t first_key = 0;
        for (int frame = 0; frame < kLockFrames + 5; ++frame) {
            const float bx = 269.98f + 0.6f * float(frame), by = 2.54f, bz = 117.36f;  // riding east
            const Mat4  V  = LookAt(bx - 4.0f, by + 1.8f, bz, bx + 8.0f, by, bz);
            const Mat4  VP = Mul(P, V);
            std::vector<Upload> ups;
            for (int o = 0; o < 6; ++o) {
                const Mat4 M = Translate(100.0f + 40.0f * float(o), 0, 60.0f - 25.0f * float(o));
                ups.push_back({3, 0, Mul(VP, M)});
            }
            ups.push_back({7, 1, Translate(bx, by, bz)});
            ups.push_back({7, 2, VP});
            ups.push_back({9, 4, Transposed(V)});
            ups.push_back({0, 0, ffz});
            const UniformPick pk = PickUniform(ups, bx, by, bz, lock.locked());
            if (frame == 0) {
                CHECK(pk.index == 7 && pk.perspective && !pk.transposed && pk.axes == 0,
                      "the view-projection is picked: index %d persp %d T %d axes %d", pk.index, int(pk.perspective),
                      int(pk.transposed), pk.axes);
                CHECK(std::fabs(pk.dist - std::sqrt(16.0f + 1.8f * 1.8f)) < 0.01f, "its eye is the camera: %f m",
                      pk.dist);
                first_key = pk.key;
                // Drawn through as it stands, it agrees with P then V.
                const Screen a = Project(VP, Mat4{}, bx + 8, by, bz), b = Project(P, V, bx + 8, by, bz);
                CHECK(a.ok && std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f && std::fabs(a.x - 0.5f) < 1e-3f,
                      "VP projects like P*V: %f %f", a.x, a.y);
                float ex, ey, ez;
                bool  persp = false;
                CHECK(EyeOf(VP, ex, ey, ez, persp) && persp && std::fabs(ex - (bx - 4)) < 0.01f &&
                          std::fabs(ey - (by + 1.8f)) < 0.01f && std::fabs(ez - bz) < 0.01f,
                      "the projective eye solve: %f %f %f", ex, ey, ez);
                CHECK(EyeOf(ffz, ex, ey, ez, persp) && !persp && std::fabs(ex) + std::fabs(ey) + std::fabs(ez) < 1e-4f,
                      "the constant fixed-function matrix puts its eye at the origin");
                // Without the view-projection, the row-major view is found, transposed.
                std::vector<Upload> no_vp(ups.begin(), ups.end());
                no_vp.erase(no_vp.begin() + 7);
                const UniformPick v2 = PickUniform(no_vp, bx, by, bz);
                CHECK(v2.index >= 0 && no_vp[size_t(v2.index)].program == 9 && v2.transposed && !v2.perspective,
                      "the row-major view is read transposed: index %d", v2.index);
                // The object MVPs and the bike's model matrix alone give no camera.
                const std::vector<Upload> junk(ups.begin(), ups.begin() + 7);
                CHECK(PickUniform(junk, bx, by, bz).index < 0, "scenery MVPs and a model matrix are not a camera");
            }
            lock.vote(pk.key);
            if (frame >= kLockFrames) CHECK(lock.locked() == first_key, "locked on the view-projection's key");
        }
        // Once locked, the same key is followed even when another camera-shaped upload appears.
        // A z-up GL world: the view-projection built in GL (x, -z, y) of the telemetry.
        const float bx = 300.0f, by = 5.0f, bz = 90.0f;
        const Mat4  Vz = Mul(LookAt(bx - 4.0f, by + 1.8f, -bz, bx + 8.0f, by, -bz), Rotate(-90.0f, 1, 0, 0));
        // In GL z-up coordinates the eye is (bx-4, bz, by+1.8): check, then pick.
        float ex, ey, ez;
        CameraPos(Vz, ex, ey, ez);
        CHECK(std::fabs(ex - (bx - 4)) < 0.01f && std::fabs(ey - bz) < 0.02f && std::fabs(ez - (by + 1.8f)) < 0.02f,
              "z-up eye %f %f %f", ex, ey, ez);
        const UniformPick zk = PickUniform({{5, 3, Mul(P, Vz)}}, bx, by, bz);
        CHECK(zk.index == 0 && zk.axes >= 0 && AxesName(AxesAt(zk.axes)) == "x,z,y",
              "a z-up shader world is found: %s", zk.axes >= 0 ? AxesName(AxesAt(zk.axes)).c_str() : "none");
        if (zk.axes >= 0) {
            const Screen zs = Project(Mul(P, Vz), Mat4{}, bx + 8, by, bz, AxesAt(zk.axes));
            CHECK(zs.ok && std::fabs(zs.x - 0.5f) < 1e-3f && std::fabs(zs.y - 0.5f) < 1e-3f,
                  "and the look-at point lands centre: %f %f", zs.x, zs.y);
        }
        CHECK(PickUniform({{5, 3, Mul(P, Vz)}}, bx, by, bz, first_key).index < 0, "a locked key ignores the rest");
    }

    // --- The camera baked into the fixed-function modelview loads ---------------------------
    // What v0.42.2 saw: no matrix uniforms, a projection of frustum times a constant z flip. The
    // game loads view * model per object; the track's model is the identity, so its load is the
    // view. One frame: scenery with real model matrices, the bike's own (view * bike model), the
    // track's (the view), in GL coordinates = telemetry (x, y, -z) (the z flip's partner).
    {
        Axes gl;  // x, y, -z
        gl.sgn[2] = -1;
        const int gl_idx = FallbackAxes();
        CHECK(AxesName(AxesAt(gl_idx)) == "x,y,-z", "the right-handed GL world is x,y,-z");
        AxesLock lock;
        for (int frame = 0; frame < kLockFrames + 2; ++frame) {
            const float bx = 269.98f + 0.6f * float(frame), by = 2.54f, bz = 117.36f;  // riding east
            float ex, ey, ez, tx, ty, tz;
            gl.apply(bx - 4.0f, by + 1.8f, bz, ex, ey, ez);
            gl.apply(bx + 8.0f, by, bz, tx, ty, tz);
            const Mat4 V = LookAt(ex, ey, ez, tx, ty, tz);
            ModelviewPick best;
            for (int o = 0; o < 20; ++o)
                ModelviewOffer(best, Mul(V, Mul(Translate(50.0f * float(o), 0, -30.0f * float(o)), Rotate(37.0f * float(o), 0, 1, 0))),
                               bx, by, bz, lock.locked());
            float gx, gy, gz;
            gl.apply(bx, by, bz, gx, gy, gz);
            ModelviewOffer(best, Mul(V, Mul(Translate(gx, gy, gz), Rotate(90.0f, 0, 1, 0))), bx, by, bz, lock.locked());
            ModelviewOffer(best, V, bx, by, bz, lock.locked());  // the track
            if (frame == 0) {
                CHECK(best.ok && best.axes == gl_idx && std::fabs(best.dist - std::sqrt(16.0f + 3.24f)) < 0.01f,
                      "the track's load is the camera, under x,y,-z: axes %s dist %f",
                      best.axes >= 0 ? AxesName(AxesAt(best.axes)).c_str() : "none", best.dist);
                // Drawn through the game's projection (frustum * z flip) and that view, a point
                // on the line ahead lands where the game would put it.
                Mat4 zf;
                zf.m[10] = -1;
                const Screen s1 = Project(Mul(p, zf), best.view, bx + 8, by, bz, AxesAt(best.axes));
                const Screen s2 = Project(Mul(p, zf), V, tx, ty, tz);
                CHECK(s1.ok == s2.ok && std::fabs(s1.x - s2.x) < 1e-4f && std::fabs(s1.y - s2.y) < 1e-4f,
                      "telemetry through the picked axes = GL through the game's matrices");
            }
            lock.vote(best.ok ? best.axes : -1);
        }
        CHECK(lock.locked() == gl_idx, "and the axes lock");
        // Scenery alone gives no camera.
        ModelviewPick none;
        ModelviewOffer(none, Translate(500, 0, 500), 269.98f, 2.54f, 117.36f);
        ModelviewOffer(none, Mul(Translate(269.98f, 2.54f, -117.36f), Rotate(30, 0, 1, 0)), 269.98f, 2.54f, 117.36f);
        CHECK(!none.ok, "a model matrix at the bike, or far off, is not a camera");
    }

    // --- The fallback helmet camera from the telemetry alone ---------------------------------
    {
        const float bx = 269.98f, by = 2.54f, bz = 117.36f;
        Mat4 ob;
        CHECK(!OnboardView(bx, by, bz, 0.2f, 0, 0.1f, ob), "too slow to know the heading: no view");
        // Riding north (+z in telemetry) at 15 m/s on the flat.
        CHECK(OnboardView(bx, by, bz, 0, 0, 15, ob), "a view while riding");
        const Axes  gax = AxesAt(FallbackAxes());
        float ex, ey, ez;
        CameraPos(ob, ex, ey, ez);
        CHECK(std::fabs(ex - bx) < 1e-3f && std::fabs(ey - (by + kHeadUp)) < 1e-3f &&
                  std::fabs(ez - -(bz + kHeadFwd)) < 1e-3f,
              "the eye sits at the helmet, in GL coordinates: %f %f %f", ex, ey, ez);
        // A point straight ahead along the travel, tilted kHeadDown below it, is screen centre.
        const float d = 30.0f, drop = d * std::tan(kHeadDown * 0.017453292f);
        const Screen c = Project(p, ob, bx, by + kHeadUp - drop, bz + kHeadFwd + d, gax);
        CHECK(c.ok && std::fabs(c.x - 0.5f) < 1e-3f && std::fabs(c.y - 0.5f) < 1e-3f, "dead ahead is centred: %f %f", c.x,
              c.y);
        // East is to the right when facing north, the ground ahead is below centre, behind is culled.
        CHECK(Project(p, ob, bx + 3, by, bz + 20, gax).x > 0.5f, "east is right when riding north");
        CHECK(Project(p, ob, bx, by, bz + 5, gax).y > 0.5f, "the ground just ahead is low on screen");
        CHECK(!Project(p, ob, bx, by, bz - 10, gax).ok, "behind the rider is culled");
        // Riding east, uphill: the view pitches up with the travel.
        CHECK(OnboardView(bx, by, bz, 10, 3, 0, ob), "uphill");
        const Screen up = Project(p, ob, bx + 30, by + kHeadUp + 9.0f - 30.0f * std::tan(kHeadDown * 0.017453292f), bz, gax);
        CHECK(up.ok && std::fabs(up.x - 0.5f) < 0.01f && std::fabs(up.y - 0.5f) < 0.02f,
              "riding east up a 3-in-10 slope, the slope ahead is near centre: %f %f", up.x, up.y);
        CHECK(Project(p, ob, bx + 20, by, bz + 3, gax).x < 0.5f, "north is left when riding east");
        // The ribbon through the fallback: narrows with distance like any other.
        const Screen n1 = Project(p, ob, bx + 10, by, bz - kHalfWidth, gax), n2 = Project(p, ob, bx + 10, by, bz + kHalfWidth, gax);
        const Screen f1 = Project(p, ob, bx + 40, by + 9, bz - kHalfWidth, gax), f2 = Project(p, ob, bx + 40, by + 9, bz + kHalfWidth, gax);
        CHECK(n1.ok && f1.ok && std::fabs(n2.x - n1.x) > std::fabs(f2.x - f1.x), "and the ribbon narrows ahead");
    }

    // Every signed permutation is distinct and AxesAt(0) is the identity.
    CHECK(AxesName(AxesAt(0)) == "x,y,z", "identity first");
    {
        std::vector<std::string> names;
        for (int k = 0; k < kNumAxes; ++k) names.push_back(AxesName(AxesAt(k)));
        std::sort(names.begin(), names.end());
        CHECK(std::unique(names.begin(), names.end()) == names.end(), "48 distinct axes");
    }
    CHECK(PickCamera(frame, 16.0f / 9.0f, NAN, 0, 0).index < 0, "no rider, no camera");
    CHECK(PickCamera({}, 16.0f / 9.0f, rx, ry, rz).index < 0, "no candidates, no camera");

    // --- Height ----------------------------------------------------------------------------
    HeightBias hb;
    CHECK(hb.offset() == 0 && !hb.ready(), "nothing learnt yet");
    for (int i = 0; i < 200; ++i) hb.add(0.62f);
    CHECK(hb.ready() && hb.offset() == 0.0f, "a bike's height over the centreline: the heights are the ground");
    hb.reset();
    for (int i = 0; i < 200; ++i) hb.add(42.6f);
    CHECK(std::fabs(hb.offset() - 42.0f) < 0.01f, "heights all 0 on a track at 42 m: moved up, %f", hb.offset());
    hb.add(NAN);
    CHECK(std::isfinite(hb.offset()), "NaN is ignored");

    // --- The ribbon on a synthetic track ------------------------------------------------------
    std::vector<unsigned char> segs(4 * 28, 0);
    for (int i = 0; i < 4; ++i) {
        unsigned char* r = &segs[size_t(i) * 28];
        PutF(r + 4, 100.0f);
        PutF(r + 24, float(i));
    }
    coachhud::Track track;
    CHECK(track.build(4, segs.data(), 28, nullptr), "track builds");
    float h = -1;
    CHECK(track.height_at_lap(0.125f, h) && std::fabs(h - 0.5f) < 1e-3f, "height interpolates, got %f", h);
    std::vector<coachhud::RefPoint> ref;
    for (int i = 0; i <= 40; ++i) {
        coachhud::RefPoint r;
        r.pos = float(i) / 40.0f * 0.999f;
        r.x   = r.pos * 400.0f;
        r.z   = 1.0f;
        ref.push_back(r);
    }
    Ribbon rb;
    CHECK(rb.update(ref, track, 0.05f, 0.0f), "first update builds");
    const auto& vs = rb.verts();
    CHECK(vs.size() == 2 * size_t(kAhead / kStepTerrain + 1), "rows: %zu verts", vs.size());
    CHECK(std::fabs(vs[0].s) < 1e-3f && std::fabs((vs[0].x + vs[1].x) * 0.5f - 20.0f) < 0.5f,
          "starts at the rider, x=%f", vs[0].x);
    bool rising = true, wide = true;
    for (size_t i = 2; i < vs.size(); i += 2)
        if (vs[i].y < vs[i - 2].y - 1e-4f) rising = false;
    for (size_t i = 0; i + 1 < vs.size(); i += 2)
        if (std::fabs(std::fabs(vs[i].z - vs[i + 1].z) - 2 * kHalfWidth) > 1e-3f) wide = false;
    CHECK(rising, "ribbon follows the climbing ground");
    CHECK(wide, "ribbon is the right width");
    CHECK(vs.back().s <= kAhead, "ahead limit");
    CHECK(!rb.update(ref, track, 0.0505f, 0.0f), "a small move does not rebuild");
    CHECK(rb.update(ref, track, 0.20f, 0.0f), "a big move rebuilds");
    CHECK(rb.update(ref, track, 0.20f, 1.0f), "a height offset change rebuilds");
    // Braking zones tint the rows inside them.
    coachcue::Sheet cs;
    cs.cues.push_back({90.0f, coachcue::BRAKE, 0, "Brake"});
    cs.cues.push_back({100.0f, coachcue::THROTTLE, 0, "Gas"});
    const std::vector<Zone> zones = BrakeZones(cs, 400.0f);
    CHECK(zones.size() == 1 && zones[0].from_m == 90.0f && zones[0].to_m == 100.0f, "one 10 m zone");
    CHECK(rb.update(ref, track, 0.20f, 1.0f, zones), "zones rebuild");
    size_t braking = 0;
    for (const Vert& x : rb.verts()) braking += x.rgba[1] < 0.5f ? 1 : 0;  // red
    CHECK(braking >= 36 && braking <= 44, "about 10 m of the ribbon is a braking zone: %zu verts", braking);
    const std::vector<coachhud::RefPoint> none;
    rb.update(none, track, 0.2f, 0.0f);
    CHECK(rb.verts().empty(), "no sheet, no line");
    Ribbon fresh;
    CHECK(!fresh.update(ref, coachhud::Track(), 0.2f, 0.0f) && fresh.verts().empty(), "no track, no line");
    CHECK(!fresh.update(ref, track, NAN, 0.0f) && fresh.verts().empty(), "no position, no line");

    // --- Coach's real sheet: 755 Compound, KTM 250 SX-F -------------------------------------
    const std::string sheet_path = std::string(COACHLINE_DATA) + "/755_Compound.MX2OEM_2023_KTM_250_SX-F.hud";
    const std::vector<unsigned char> file = ReadFile(sheet_path);
    coachhud::Sheet sheet;
    CHECK(!file.empty() && coachhud::Parse(file.data(), file.size(), sheet), "the 755 sheet parses: %s",
          sheet_path.c_str());
    CHECK(sheet.ref.size() > 1000 && std::fabs(sheet.track_len - 1322.79f) < 0.1f, "%zu points, %f m",
          sheet.ref.size(), double(sheet.track_len));
    if (sheet.ref.size() > 400) {
        coachhud::Track t755;
        CHECK(BuildTrack(t755, sheet.track_len, 10.0f), "a 755-length track for the heights");
        // A sample pose: the rider on Coach's line at point 300, the chase camera 3.5 m behind
        // and 1.8 m above the bike, looking 8 m ahead of it.
        const coachhud::RefPoint& at   = sheet.ref[300];
        const coachhud::RefPoint& next = sheet.ref[306];
        float hx = next.x - at.x, hz = next.z - at.z;
        const float hl = std::sqrt(hx * hx + hz * hz);
        hx /= hl, hz /= hl;
        float ground = 0;
        t755.height_at_lap(at.pos, ground);
        const float bike_y = ground + kOriginGuess;
        const Mat4  cam = LookAt(at.x - hx * 3.5f, bike_y + 1.8f, at.z - hz * 3.5f, at.x + hx * 8.0f, ground,
                                 at.z + hz * 8.0f);
        const Pick  pick = PickCamera({{p, cam}}, 16.0f / 9.0f, at.x, bike_y, at.z);
        CHECK(pick.index == 0, "the chase camera is accepted for this rider");
        HeightBias hb755;
        for (int i = 0; i < 100; ++i) hb755.add(bike_y - ground);
        Ribbon r755;
        CHECK(r755.update(sheet.ref, t755, at.pos, hb755.offset()), "a ribbon is built on the real sheet");
        const std::vector<Vert>& rv = r755.verts();
        CHECK(rv.size() == 2 * size_t(kAhead / kStepTerrain + 1), "%zu verts for 60 m at 0.5 m", rv.size());
        // It starts at the bike, lies on the ground (not at the bike's height), and is the
        // painted width all along.
        if (rv.size() >= 2)
            CHECK(std::hypot((rv[0].x + rv[1].x) * 0.5f - at.x, (rv[0].z + rv[1].z) * 0.5f - at.z) < 1.5f,
                  "starts at the rider");
        bool on_ground = true, width_ok = true;
        for (size_t i = 0; i + 1 < rv.size(); i += 2) {
            float       g   = 0;
            const float lap = at.pos + rv[i].s / sheet.track_len;
            t755.height_at_lap(lap - std::floor(lap), g);
            if (std::fabs(rv[i].y - (g + kLift)) > 0.3f) on_ground = false;
            if (std::fabs(std::hypot(rv[i].x - rv[i + 1].x, rv[i].z - rv[i + 1].z) - 2 * kHalfWidth) > 1e-3f)
                width_ok = false;
        }
        CHECK(on_ground, "every row lies on the ground");
        CHECK(width_ok, "every row is the painted width");
        // Through the camera: bottom of the screen up toward the horizon, narrowing as it goes,
        // and all of it in view - the sheet's line is where the bike is.
        const std::vector<ScreenRow> rows = ProjectRibbon(p, cam, AxesAt(pick.axes), rv);
        CHECK(rows.size() == rv.size() / 2, "no row is behind the camera: %zu of %zu", rows.size(), rv.size() / 2);
        if (!rows.empty()) {
            CHECK(rows.front().l.y > 0.6f, "the near end is low on screen, y=%f", rows.front().l.y);
            CHECK(rows.back().l.y < rows.front().l.y - 0.1f, "the far end is higher, y=%f", rows.back().l.y);
            const float wn = std::fabs(rows.front().r.x - rows.front().l.x);
            const float wf = std::fabs(rows.back().r.x - rows.back().l.x);
            CHECK(wf < wn * 0.25f, "and much narrower: %f near, %f far", wn, wf);
            bool on_screen = true;
            for (const ScreenRow& r : rows)
                if (r.l.x < -0.2f || r.l.x > 1.2f || r.l.y < 0.2f || r.l.y > 1.2f) on_screen = false;
            CHECK(on_screen, "the whole ribbon is in view");
            CHECK(rows.back().alpha == 0.0f && rows.front().alpha == 1.0f, "faded out by 60 m");
        }
        if (argc > 1) RenderPreview(argv[1], p, cam, rv, at.x, bike_y, at.z, ground);
    }

    // --- Lying on the track's own ground ("TRRN") --------------------------------------------
    // A bumpy heightfield: whoops 5 m apart along x, a 1-in-5 camber across z, a 2 m swell. Coach
    // samples it at five offsets across the line at each point; the ribbon must lie on it.
    {
        auto H = [](float x, float z) {
            return 3.0f + 0.35f * std::sin(x / 5.0f * 6.2831853f) + 0.2f * z + 2.0f * std::sin(x / 120.0f);
        };
        std::vector<unsigned char> sg(28, 0);
        PutF(&sg[4], 400.0f);
        coachhud::Track flat;
        CHECK(flat.build(1, sg.data(), 28, nullptr), "a one-segment track");
        std::vector<coachhud::RefPoint> line;
        for (int i = 0; i <= 400; ++i) {
            coachhud::RefPoint r;
            r.pos = float(i) / 400.0f * 0.999f;
            r.t   = float(i) * 0.05f;
            r.x   = float(i);  // a straight east, a metre a point
            r.z   = 10.0f;
            line.push_back(r);
        }
        RibbonExtras ex;
        ex.version = 1, ex.terrain_k = 5, ex.terrain_step = 0.5f;
        for (const auto& r : line)
            for (int j = 0; j < 5; ++j) {
                const float off = (float(j) - 2.0f) * 0.5f;  // + is left: travelling east, left is +z
                ex.terrain.push_back(H(r.x, r.z + off));
            }
        Ribbon tr;
        CHECK(tr.update(line, flat, 0.1f, 0.0f, {}, &ex), "a ribbon on the track's ground");
        const auto& tv = tr.verts();
        CHECK(tv.size() == 2 * size_t(kAhead / kStepTerrain + 1), "rows every half metre: %zu verts", tv.size());
        // On the ground plus its slope-dependent lift, within what a metre between samples can show
        // of 5 m whoops (0.35 m high: 7 cm at most).
        float lo = 1e9f, hi = -1e9f;
        for (const Vert& x : tv) lo = (std::min)(lo, x.y - H(x.x, x.z)), hi = (std::max)(hi, x.y - H(x.x, x.z));
        CHECK(lo > kLiftBase - 0.08f && hi < kLiftMax + 0.08f, "every vertex on the ground plus its lift: %f..%f m", double(lo),
              double(hi));
        bool camber = true;
        for (size_t i = 0; i + 1 < tv.size(); i += 2)
            if (!(tv[i].y > tv[i + 1].y)) camber = false;  // left (+z) is the higher side of this camber
        CHECK(camber, "and it leans with the camber across");
        // A sheet with the chunks: read; a chunk that claims more than is there: ignored.
        coachhud::Sheet sh;
        std::vector<unsigned char> b;
        auto put = [&b](const void* p, size_t n) {
            b.insert(b.end(), static_cast<const unsigned char*>(p), static_cast<const unsigned char*>(p) + n);
        };
        auto u32 = [&](uint32_t v) { put(&v, 4); };
        auto f32 = [&](float v) { put(&v, 4); };
        put("MXHD", 4), u32(1), f32(400.0f), u32(3);
        for (int i = 0; i < 3; ++i) f32(float(i) * 0.3f), f32(float(i)), f32(float(i)), f32(0);
        u32(0), u32(0);  // no sections, no flags
        const size_t trrn = b.size();
        put("TRRN", 4), u32(12 + 3 * 2 * 4), u32(3), u32(2), f32(0.5f);
        for (int i = 0; i < 6; ++i) f32(1.5f);
        put("DRIV", 4), u32(4 + 3 * 12), u32(3);
        for (int i = 0; i < 3; ++i) f32(10), f32(1), f32(0);
        put("JUNK", 4), u32(9999);
        CHECK(coachhud::Parse(b.data(), b.size(), sh) && sh.terrain_k == 2 && sh.terrain.size() == 6 &&
                  sh.drive.size() == 9,
              "TRRN and DRIV chunks read, a bad trailing chunk ignored");
        b[trrn + 8] = 9;  // TRRN's N no longer the point count
        coachhud::Sheet sh2;
        CHECK(coachhud::Parse(b.data(), b.size(), sh2) && sh2.terrain.empty() && sh2.ref.size() == 3 &&
                  sh2.drive.size() == 9,
              "a TRRN that disagrees with the points is dropped, the sheet kept");
    }

    // --- Keeping the camera steady -----------------------------------------------------------
    {
        CameraFollow f;
        const float eye[3] = {100, 5, -50};
        f.hit(eye, 1000);
        bool held = true, fresh_ok = true;
        for (int miss = 1; miss <= 10; ++miss) {  // ten missed frames at 100 fps
            held = held && f.holding(1000ull + unsigned(miss) * 10);
            if (miss * 10 <= int(kStaleMs)) fresh_ok = fresh_ok && f.fresh(1000ull + unsigned(miss) * 10);
        }
        CHECK(held && fresh_ok, "the lock survives ten missed frames");
        CHECK(f.holding(1000ull + kHoldMs) && !f.holding(1000ull + kHoldMs + 1), "and lets go after %u ms", kHoldMs);
        CHECK(f.anchor(1200) != nullptr && f.anchor(1000ull + kHoldMs + 50) == nullptr, "searching freely once let go");
        // A frame with the real camera 0.4 m on and an outlier 8 m off, both near the bike: the
        // outlier is never taken while the camera is being followed.
        const float bx = 104, by = 3.5f, bz = 50;  // telemetry; GL = x, y, -z
        Axes gl;
        gl.sgn[2] = -1;
        const int gi = FallbackAxes();
        float gx, gy, gz;
        gl.apply(bx + 8, by, bz, gx, gy, gz);
        const Mat4 real    = LookAt(100.4f, 5, -50, gx, gy, gz);
        const Mat4 outlier = LookAt(108, 5, -54, gx, gy, gz);
        ModelviewPick best;
        ModelviewFollow(best, outlier, bx, by, bz, gi, eye);
        CHECK(!best.ok, "a one-frame outlier 8 m from the camera is ignored");
        ModelviewFollow(best, real, bx, by, bz, gi, eye);
        ModelviewFollow(best, outlier, bx, by, bz, gi, eye);
        CHECK(best.ok && std::fabs(best.jump - 0.4f) < 0.01f, "the camera that moved 0.4 m is followed: %f", best.jump);
    }

    // --- How Coach's lap was ridden, on the real 755 sheet ----------------------------------
    {
        coachhud::Sheet sh;
        const std::vector<unsigned char> f =
            ReadFile(std::string(COACHLINE_DATA) + "/755_Compound.MX2OEM_2023_KTM_250_SX-F.hud");
        CHECK(!f.empty() && coachhud::Parse(f.data(), f.size(), sh), "the 755 sheet");
        const std::vector<float> tone = Tone(sh.ref, sh.drive);
        CHECK(tone.size() == sh.ref.size(), "a tone from the lap's own times");
        const auto zones = ToneZones(sh.ref, tone);
        std::printf("755 Compound: %zu braking zones per lap\n", zones.size());
        CHECK(zones.size() >= 8 && zones.size() <= 30, "a braking zone per corner or so: %zu", zones.size());
        size_t gas = 0, coast = 0, light = 0, heavy = 0;
        for (float t : tone) (t < 0.5f ? gas : t < 1.5f ? coast : t < 2.5f ? light : heavy)++;
        std::printf("755 Compound: gas %zu, coast %zu, light brake %zu, heavy brake %zu points\n", gas, coast, light, heavy);
        CHECK(gas > tone.size() / 5, "on the gas for much of the lap: %zu of %zu", gas, tone.size());
        CHECK(light + heavy > tone.size() / 20 && light + heavy < tone.size() / 2, "braking for 5-50%% of it");
        // A fade, never a seam: between neighbouring points (about a metre apart) the tone moves
        // a fraction of a band at most.
        float steepest = 0;
        for (size_t i = 1; i < tone.size(); ++i) {
            const float d = std::hypot(sh.ref[i].x - sh.ref[i - 1].x, sh.ref[i].z - sh.ref[i - 1].z);
            if (d > 0.2f && d < kMaxGapM) steepest = (std::max)(steepest, std::fabs(tone[i] - tone[i - 1]) / d);
        }
        CHECK(steepest < 0.75f, "the tone changes by at most %.2f a metre (got %.2f): no seams", 0.75, double(steepest));
        // And LineColours gives that, as RGBA per point at the line's alpha.
        const std::vector<float> rgba = LineColours(sh.ref, sh.drive);
        CHECK(rgba.size() == sh.ref.size() * 4 && rgba[3] == kLineAlpha, "RGBA per point");
        // The gradient's stops, and that it is continuous between them.
        float r, g, b, r2, g2, b2;
        ToneColour(0, r, g, b);
        CHECK(g > 0.8f && r < 0.3f && b < 0.3f, "0 is green");
        ToneColour(1, r, g, b);
        CHECK(r > 0.9f && g > 0.9f && b > 0.9f, "1 is white");
        ToneColour(2, r, g, b);
        CHECK(r > 0.9f && g > 0.8f && b < 0.1f, "2 is yellow");
        ToneColour(3, r, g, b);
        CHECK(r > 0.9f && g < 0.2f && b < 0.1f, "3 is red");
        bool smooth = true;
        for (int k = 0; k < 300; ++k) {
            ToneColour(k * 0.01f, r, g, b);
            ToneColour(k * 0.01f + 0.01f, r2, g2, b2);
            if (std::fabs(r - r2) + std::fabs(g - g2) + std::fabs(b - b2) > 0.05f) smooth = false;
        }
        CHECK(smooth, "the gradient has no jumps");
        // No blue anywhere.
        bool blue = false;
        for (int k = 0; k <= 300; ++k) {
            ToneColour(k * 0.01f, r, g, b);
            if (b > r && b > g) blue = true;
        }
        CHECK(!blue, "no blue in the gradient");
        if (argc > 2 && !sh.ref.empty()) RenderToneMap(argv[2], sh, tone, zones);
    }

    // --- Sean's Ridgedale run, replayed (v0.43.2) -------------------------------------------
    // (1) A sheet whose lap positions have drifted from the game's: Coach numbers its points by
    //     metres along the lap it recorded over the track's length, so a lap ridden 6% longer than
    //     the centreline reads 6% ahead by its end. The line "turned 90 degrees": it started where
    //     the sheet's position said, not where the rider was.
    {
        coachhud::Sheet sh;
        const std::vector<unsigned char> f =
            ReadFile(std::string(COACHLINE_DATA) + "/755_Compound.MX2OEM_2023_KTM_250_SX-F.hud");
        CHECK(!f.empty() && coachhud::Parse(f.data(), f.size(), sh), "the 755 sheet");
        std::vector<coachhud::RefPoint> drift = sh.ref;
        for (auto& r : drift) r.pos = (std::min)(1.0f, r.pos * 1.06f);
        coachhud::Track t;
        CHECK(BuildTrack(t, sh.track_len, 10.0f), "a track for the heights");
        const size_t k  = 900;  // late in the lap, where the drift is largest
        const auto&  me = sh.ref[k];
        Ribbon old_way, new_way;
        old_way.update(drift, t, me.pos, 0.0f);
        Anchor at;
        at.x = me.x, at.z = me.z, at.ground = 3.0f;
        new_way.update(drift, t, me.pos, 0.0f, {}, nullptr, &at);
        auto start = [](const Ribbon& r) {
            const auto& v = r.verts();
            return std::make_pair((v[0].x + v[1].x) * 0.5f, (v[0].z + v[1].z) * 0.5f);
        };
        const auto so = start(old_way), sn = start(new_way);
        const float off_old = std::hypot(so.first - me.x, so.second - me.z);
        const float off_new = std::hypot(sn.first - me.x, sn.second - me.z);
        std::printf("drifted sheet: the ribbon started %.1f m from the rider by lap position, %.2f m by where they are\n",
                    double(off_old), double(off_new));
        CHECK(off_old > 20.0f, "the drift is real: %.1f m by lap position", double(off_old));
        CHECK(off_new < 1.5f, "anchored, it starts at the rider: %.2f m", double(off_new));
        // And it runs along the line from there: its first rows head where Coach's line heads.
        const auto& v  = new_way.verts();
        const float lx = (v[8].x + v[9].x) * 0.5f - sn.first, lz = (v[8].z + v[9].z) * 0.5f - sn.second;
        const float tx = sh.ref[k + 3].x - me.x, tz = sh.ref[k + 3].z - me.z;
        const float cosang = (lx * tx + lz * tz) / (std::hypot(lx, lz) * std::hypot(tx, tz));
        CHECK(cosang > 0.95f, "along the line, not across it: cos %.2f", double(cosang));
        // Its height is the rider's ground plus the centreline's rise, not the sheet's place's.
        float h0, h2;
        t.height_at_lap(me.pos, h0);
        t.height_at_lap(me.pos + 2.0f / sh.track_len, h2);
        CHECK(std::fabs(v[0].y - (3.0f + kLiftTerrain)) < 0.01f, "the first row on the rider's ground: %f", double(v[0].y));
        CHECK(std::fabs(v[8].y - (3.0f + (h2 - h0) + kLiftTerrain)) < 0.02f, "and rising with the centreline");
        // A rider nowhere near the line (in the pits): no anchor, the old way.
        float fr;
        CHECK(AnchorPoint(drift, me.x + 500, me.z, me.pos, fr) < 0, "no anchor 500 m off the line");
    }

    // (2) The camera lock on Ridgedale: helmet camera, eye 1.63 m over the bike, axes x,y,z. A
    //     track mesh placed with a quarter turn has a view exactly as near under the rotated axes;
    //     the lock must take the plain axes, every frame, and never change mid-event.
    {
        AxesLock lock;
        CameraFollow follow;
        int changes = 0, last = -1;
        for (int frame = 0; frame < 300; ++frame) {
            const float bx = 207.45f + 0.3f * float(frame), by = 1.09f, bz = 124.35f;
            const Mat4  V  = LookAt(bx, by + 1.63f, bz, bx + 10.0f, by, bz);
            ModelviewPick best;
            const float*  prev = lock.locked() >= 0 ? follow.anchor(1000ull + unsigned(frame) * 10) : nullptr;
            for (const Mat4& m : {Mul(V, Rotate(90.0f, 0, 1, 0)), V, Mul(V, Rotate(-90.0f, 0, 1, 0))}) {
                if (prev) ModelviewFollow(best, m, bx, by, bz, lock.locked(), prev);
                else ModelviewOffer(best, m, bx, by, bz, lock.locked());
            }
            lock.vote(best.ok ? best.axes : -1);
            if (best.ok) {
                float e[3];
                CameraPos(best.view, e[0], e[1], e[2]);
                follow.hit(e, 1000ull + unsigned(frame) * 10);
            }
            if (lock.locked() != last) ++changes, last = lock.locked();
        }
        CHECK(lock.locked() == 0, "locked on x,y,z, not a quarter-turned copy: %s",
              lock.locked() >= 0 ? AxesName(AxesAt(lock.locked())).c_str() : "none");
        CHECK(changes == 1, "and the axes changed once (the lock), never after: %d", changes);
    }

    // (3) Snapping to the ground the game drew: a ribbon built a metre under the visible ground.
    {
        const float ground = 2.0f;
        const Mat4  V      = LookAt(0, ground + 1.6f, 0, 20, ground, 0);  // helmet, looking east
        const Axes  id;
        // The unprojection undoes the projection.
        const Screen sp = Project(p, V, 15, ground, 1.0f);
        float        ux, uy, uz;
        Mat4         pv = Mul(p, V);
        const float* m  = pv.m;
        const float  cz = m[2] * 15 + m[6] * ground + m[10] * 1.0f + m[14], cw = m[3] * 15 + m[7] * ground + m[11] * 1.0f + m[15];
        CHECK(Unproject(p, V, id, sp.x, sp.y, (cz / cw + 1) * 0.5f, ux, uy, uz) && std::fabs(ux - 15) < 0.01f &&
                  std::fabs(uy - ground) < 0.01f && std::fabs(uz - 1.0f) < 0.01f,
              "unproject(project(p)) = p: %f %f %f", double(ux), double(uy), double(uz));
        // The ribbon, east along z = 0, a metre too low.
        std::vector<Vert> rv;
        for (int i = 0; i <= 120; ++i) {
            const float s = float(i) * 0.5f;
            rv.push_back({s, ground - 1.0f, kHalfWidth, s, {1, 1, 1, 0.7f}});
            rv.push_back({s, ground - 1.0f, -kHalfWidth, s, {1, 1, 1, 0.7f}});
        }
        DepthSnap snap;
        for (int iter = 0; iter < 8; ++iter)
            for (int k = 0; k < kSnapN; ++k) {
                // Where the game's depth says the ray to the ribbon's centre at kSnapAt[k] meets
                // the ground plane: the hit, as the depth readback would give it.
                const float rx = kSnapAt[k], ry = ground - 1.0f + snap.at(kSnapAt[k]);
                const float ex = 0, ey = ground + 1.6f;
                const float t  = (ground - ey) / (ry - ey);  // along the ray, where it is at ground height
                if (!(t > 0)) continue;
                const float hx = ex + (rx - ex) * t;
                const float dy = SnapReading(rv, snap, hx, ground, 0.0f, 0.0f, 0.0f);
                if (std::isfinite(dy)) snap.feed(k, snap.at(kSnapAt[k]) + dy);
            }
        CHECK(snap.any(), "the depth gave readings");
        CHECK(std::fabs(snap.at(12.0f) - 1.0f) < 0.1f && std::fabs(snap.at(32.0f) - 1.0f) < 0.1f,
              "and the correction lifts the ribbon onto the ground: %f at 12 m, %f at 32 m", double(snap.at(12)),
              double(snap.at(32)));
        DepthSnap junk;
        CHECK(!junk.feed(0, 7.0f) && !junk.any(), "a 7 m reading (a bike, a hillside) is refused");
        CHECK(!std::isfinite(SnapReading(rv, junk, 1.0f, ground, 0.5f, 0.0f, 0.0f)), "a hit at the rider's bike is refused");
        CHECK(!std::isfinite(SnapReading(rv, junk, 20.0f, ground, 6.0f, 0.0f, 0.0f)), "a hit 6 m off the line is refused");
    }

    // (4) Colours: Ridgedale's sheet had no DRIV, and steady speed read as coasting.
    {
        std::vector<coachhud::RefPoint> line;
        float t = 0;
        for (int i = 0; i <= 600; ++i) {
            coachhud::RefPoint r;
            const float x = float(i);
            // 0-200 m steady 20 m/s, 200-220 m braking hard to 8 m/s (about 8 m/s^2), then
            // accelerating back.
            const float v = x < 200 ? 20.0f : x < 220 ? 20.0f - (x - 200) * 0.6f : (std::min)(20.0f, 8.0f + (x - 220) * 0.1f);
            if (i) t += 1.0f / v;
            r.pos = x / 601.0f, r.t = t, r.x = x, r.z = 0;
            line.push_back(r);
        }
        const std::vector<float> tone = Tone(line);
        CHECK(!tone.empty() && tone[100] < 0.1f, "steady speed without DRIV is gas, not coast: %f", double(tone[100]));
        CHECK(tone[211] > 2.3f, "hard braking is red: %f", double(tone[211]));
        CHECK(tone[400] < 0.1f, "accelerating is gas: %f", double(tone[400]));
        CHECK(std::string(ColourSource(line, {})) == "speed", "source=speed without DRIV");
        // With DRIV: throttle 0.3 is gas, 0.02 is coast, 0.15 halfway; the brake lever is braking.
        std::vector<float> drive;
        for (size_t i = 0; i < line.size(); ++i) {
            const float thr = i < 150 ? 0.3f : i < 300 ? 0.02f : i < 450 ? 0.15f : 0.0f;
            const float brk = i >= 450 ? 0.8f : 0.0f;
            drive.insert(drive.end(), {15.0f, thr, brk});
        }
        const std::vector<float> dt = Tone(line, drive);
        CHECK(dt[75] < 0.05f, "throttle 0.3 is gas: %f", double(dt[75]));
        CHECK(std::fabs(dt[225] - 1.0f) < 0.05f, "throttle 0.02 is coast: %f", double(dt[225]));
        CHECK(dt[375] > 0.3f && dt[375] < 0.7f, "throttle 0.15 is between: %f", double(dt[375]));
        CHECK(dt[525] > 2.4f, "the brake at 0.8 is red: %f", double(dt[525]));
        CHECK(std::string(ColourSource(line, drive)) == "DRIV", "source=DRIV with it");
    }

    // --- v0.43.4: the track's own ground grid, crashes, and the blinking -----------------------
    {
        // A grid file as Coach writes it: 300 x 200 at 1 m, a 3 m swell with whoops on it.
        auto H = [](float x, float z) {
            return 5.0f + 3.0f * std::sin(x / 80.0f) + 0.3f * std::sin(x / 4.0f * 6.2831853f) + 0.05f * z;
        };
        const uint32_t W = 300, Hn = 200;
        std::vector<unsigned char> gb;
        auto put = [&gb](const void* p, size_t n) {
            gb.insert(gb.end(), static_cast<const unsigned char*>(p), static_cast<const unsigned char*>(p) + n);
        };
        auto u32 = [&](uint32_t v) { put(&v, 4); };
        auto f32 = [&](float v) { put(&v, 4); };
        put("MXGR", 4), u32(1), u32(W), u32(Hn), f32(1.0f), f32(0), f32(0), f32(0.0f), f32(0.001f);
        for (uint32_t r = 0; r < Hn; ++r)
            for (uint32_t c = 0; c < W; ++c) {
                const uint16_t q = uint16_t(std::lround(H(float(c), float(r)) / 0.001f));
                put(&q, 2);
            }
        GroundGrid grid;
        CHECK(grid.parse(gb.data(), gb.size()) && grid.ready() && grid.width() == W, "the grid file reads");
        float g = 0;
        CHECK(grid.at(100.5f, 50.25f, g) && std::fabs(g - H(100.5f, 50.25f)) < 0.1f, "bilinear (4 m whoops at 1 m samples): %f vs %f", double(g),
              double(H(100.5f, 50.25f)));
        CHECK(!grid.at(-1, 5, g) && !grid.at(5, 500, g), "off the grid is unknown");
        std::vector<unsigned char> bad = gb;
        bad[4] = 2;
        GroundGrid g2;
        CHECK(!g2.parse(bad.data(), bad.size()) && !g2.parse(gb.data(), 100), "a wrong version or a short file is refused");

        // Its alignment with the rider: the bike 0.6 m above the ground, a little noise.
        AlignCheck al;
        bool done = false;
        for (int i = 0; i < kAlignSamples && !done; ++i) {
            const float x = 20.0f + float(i) * 1.2f, z = 100.0f + 10.0f * std::sin(float(i) * 0.05f);
            done = al.add(grid, x, H(x, z) + 0.6f + 0.05f * std::sin(float(i)), z);
        }
        CHECK(done && al.result().ok && al.result().dx == 0 && al.result().dz == 0 && std::fabs(al.result().dy - 0.6f) < 0.05f,
              "trh aligned dx=%.0f dz=%.0f dy=%.2f spread=%.2f", double(al.result().dx), double(al.result().dz),
              double(al.result().dy), double(al.result().spread));
        // Shifted 2 m in x (a grid whose origin is off by two samples): found and corrected.
        AlignCheck al2;
        done = false;
        for (int i = 0; i < kAlignSamples && !done; ++i) {
            const float x = 20.0f + float(i) * 1.2f, z = 100.0f;
            done = al2.add(grid, x, H(x + 2.0f, z) + 0.6f, z);
        }
        CHECK(done && al2.result().ok && al2.result().dx == 2.0f, "a 2 m shift is found: dx=%.0f", double(al2.result().dx));
        // Another track's ground: the bike's height above it wanders by metres.
        AlignCheck al3;
        done = false;
        for (int i = 0; i < kAlignSamples && !done; ++i) {
            const float x = 20.0f + float(i) * 1.2f, z = 100.0f;
            done = al3.add(grid, x, 10.0f + 4.0f * std::sin(float(i) * 0.37f), z);
        }
        CHECK(done && !al3.result().ok, "the wrong ground is refused: spread %.2f", double(al3.result().spread));

        // The ribbon on the grid: every edge on the ground, lifted by the slope, never under it.
        std::vector<unsigned char> sg(28, 0);
        PutF(&sg[4], 300.0f);
        coachhud::Track flat;
        flat.build(1, sg.data(), 28, nullptr);
        std::vector<coachhud::RefPoint> line;
        for (int i = 0; i <= 290; ++i) {
            coachhud::RefPoint r;
            r.pos = float(i) / 291.0f, r.t = float(i) * 0.05f, r.x = float(i) + 2.0f, r.z = 100.0f;
            line.push_back(r);
        }
        RibbonExtras ex;
        ex.version = 1, ex.grid = &grid;
        Anchor at;
        at.x = line[50].x, at.z = 100.0f, at.ground = 0.0f;  // a wrong ground estimate: the grid wins
        Ribbon rb2;
        CHECK(rb2.update(line, flat, line[50].pos, 0.0f, {}, &ex, &at), "a ribbon on the grid");
        float worst_under = 0, worst_over = 0;
        for (const Vert& x : rb2.verts()) {
            const float d = x.y - H(x.x, x.z);
            worst_under   = (std::min)(worst_under, d);
            worst_over    = (std::max)(worst_over, d);
        }
        CHECK(worst_under >= -0.01f, "never under the ground: %f", double(worst_under));
        CHECK(worst_over <= kLiftMax + 0.02f, "and at most %.2f m over it: %f", double(kLiftMax), double(worst_over));
        CHECK(LiftFor(0, 0) == kLiftBase && LiftFor(0.5f, 0) > LiftFor(0.1f, 0) && LiftFor(5, 5) == kLiftMax,
              "more lift where it is steep, capped");
    }

    // Crashes, replayed: riding, down, sliding, up, then the game putting the bike back 40 m away.
    {
        RiderState rs;
        rs.reset();
        float t = 0, x = 100, z = 100;
        auto ride = [&](int n, bool crashed, float dx) {
            for (int i = 0; i < n; ++i) t += 0.02f, x += dx, rs.sample(t, crashed, x, 2.0f, z);
        };
        ride(100, false, 0.4f);
        CHECK(rs.visible(), "riding: shown");
        const unsigned g0 = rs.generation();
        ride(5, true, 0.4f);
        CHECK(!rs.visible() && rs.phase() == RiderState::DOWN && rs.generation() > g0, "down: hidden, everything restarts");
        ride(150, true, 0.05f);  // sliding along on the ground
        CHECK(!rs.visible(), "still hidden while down");
        const unsigned g1 = rs.generation();
        ride(10, false, 0.0f);   // up
        CHECK(!rs.visible() && rs.generation() > g1, "getting up: still hidden, and started again");
        ride(int(kSettleS / 0.02f) + 2, false, 0.0f);
        CHECK(rs.visible(), "shown again after %.2f s", double(kSettleS));
        const unsigned g2 = rs.generation();
        x -= 40.0f;               // the game puts the bike back on the track
        ride(1, false, 0.0f);
        CHECK(!rs.visible() && rs.generation() > g2 && rs.resets() == 1, "a 40 m jump is a reset: hidden, restarted");
        ride(int(kSettleS / 0.02f) + 2, false, 0.4f);
        CHECK(rs.visible() && rs.crashes() == 1, "and shown again; one crash, one reset");

        // The camera through it: a helmet camera 1.63 m off the bike; the crash camera 6 m off.
        EyeWatch ew;
        bool     ok = true;
        unsigned long long now = 0;
        for (int i = 0; i < 100; ++i) ok = ew.ok(1.63f + 0.02f * std::sin(float(i)), now += 10) && ok;
        CHECK(ok, "the helmet camera is fine");
        CHECK(ew.ok(6.0f, now += 10) && ew.ok(6.0f, now += 100), "a 0.1 s blip is ridden out");
        CHECK(ew.ok(1.63f, now += 10), "and forgotten");
        bool kept = true;
        for (int i = 0; i < 40; ++i) kept = ew.ok(6.0f, now += 10) && kept;
        CHECK(!kept, "6 m off for 0.4 s: let go and found again");

        // And after the reset, the ribbon starts at the bike's real place, not where it was.
        coachhud::Sheet sh;
        const std::vector<unsigned char> f =
            ReadFile(std::string(COACHLINE_DATA) + "/755_Compound.MX2OEM_2023_KTM_250_SX-F.hud");
        if (coachhud::Parse(f.data(), f.size(), sh)) {
            coachhud::Track tk;
            BuildTrack(tk, sh.track_len, 10.0f);
            Ribbon rr;
            Anchor a1;
            a1.x = sh.ref[400].x, a1.z = sh.ref[400].z, a1.ground = 2;
            rr.update(sh.ref, tk, sh.ref[400].pos, 0, {}, nullptr, &a1);
            rr.clear();  // what a new generation does
            Anchor a2;
            a2.x = sh.ref[360].x + 0.5f, a2.z = sh.ref[360].z, a2.ground = 2;  // put back 40 points earlier
            rr.update(sh.ref, tk, sh.ref[360].pos, 0, {}, nullptr, &a2);
            const auto& v = rr.verts();
            CHECK(!v.empty() && std::hypot((v[0].x + v[1].x) * 0.5f - a2.x, (v[0].z + v[1].z) * 0.5f - a2.z) < 1.5f,
                  "after the reset the ribbon starts at the bike");
        }
    }

    // The depth snap's bad samples: NaN, the far plane, a droplet, a single outlier.
    {
        DepthSnap sn;
        CHECK(!sn.feed(0, NAN) && !sn.feed(0, INFINITY) && !sn.any(), "NaN and infinity are refused");
        CHECK(sn.feed(1, 0.2f), "a good reading");
        CHECK(!sn.feed(1, 2.5f) && std::fabs(sn.at(12) - 0.2f) < 1e-4f, "a single 2.3 m outlier (rain, a rider) is ignored");
        CHECK(!sn.feed(1, 9.0f), "and a far-plane hit is out of range");
        sn.feed(1, 0.25f);
        CHECK(std::fabs(sn.at(12) - 0.2125f) < 1e-3f, "readings blend slowly: %f", double(sn.at(12)));
        CHECK(sn.at(NAN) == 0.0f, "no NaN out of it either");
        std::vector<Vert> rv;
        for (int i = 0; i <= 40; ++i) {
            rv.push_back({float(i), 1.0f, 0.35f, float(i), {1, 1, 1, 1}});
            rv.push_back({float(i), 1.0f, -0.35f, float(i), {1, 1, 1, 1}});
        }
        CHECK(!std::isfinite(SnapReading(rv, sn, NAN, 1.0f, 0.0f, -50, 0)), "a NaN hit gives no reading");
    }

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("coachline_test: ok\n");
    return 0;
}
