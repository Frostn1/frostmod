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
        const float cr = a.brake ? 1.0f : 0.10f, cg = a.brake ? 0.22f : 0.50f, cb = a.brake ? 0.18f : 1.0f;
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

int main(int argc, char** argv) {
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
    CHECK(vs.size() == 2 * size_t(kAhead / kStep + 1), "rows: %zu verts", vs.size());
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
    for (const Vert& x : rb.verts()) braking += x.brake ? 1 : 0;
    CHECK(braking >= 8 && braking <= 14, "about 10 m of the ribbon is a braking zone: %zu verts", braking);
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
        CHECK(rv.size() == 2 * size_t(kAhead / kStep + 1), "%zu verts for 60 m at 2 m", rv.size());
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

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("coachline_test: ok\n");
    return 0;
}
