// The ground line's maths (src/coachline.h): matrix validation, projection and the ribbon.
// Pure C++, runs anywhere.

#include "../src/coachline.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            ++g_failures;                                      \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            std::printf(__VA_ARGS__);                          \
            std::printf("\n  (%s)\n", #cond);                  \
        }                                                      \
    } while (0)

using namespace coachline;

static Mat4 Persp(float fovy_deg, float aspect, float n, float f) {
    Mat4        p;
    const float t = 1.0f / std::tan(fovy_deg * 0.5f * 3.14159265f / 180.0f);
    std::memset(p.m, 0, sizeof(p.m));
    p.m[0]  = t / aspect;
    p.m[5]  = t;
    p.m[10] = (f + n) / (n - f);
    p.m[11] = -1.0f;
    p.m[14] = 2 * f * n / (n - f);
    return p;
}

// Camera at (cx, cy, cz) looking down -z of the world (identity rotation, translated).
static Mat4 ViewAt(float cx, float cy, float cz) {
    Mat4 v;
    v.m[12] = -cx;
    v.m[13] = -cy;
    v.m[14] = -cz;
    return v;
}

static void PutF(unsigned char* b, float f) { std::memcpy(b, &f, 4); }

int main() {
    const Mat4 p = Persp(60, 16.0f / 9.0f, 0.1f, 1000);
    CHECK(ValidProjection(p), "a perspective is accepted");
    Mat4 ortho;
    ortho.m[0] = 0.002f, ortho.m[5] = 0.002f, ortho.m[10] = -0.002f, ortho.m[15] = 1;
    CHECK(!ValidProjection(ortho), "an ortho HUD pass is refused");
    CHECK(!ValidProjection(Persp(5, 1.7f, 0.1f, 100)), "a 5 degree fov is refused");
    Mat4 nan = p;
    nan.m[0] = NAN;
    CHECK(!ValidProjection(nan), "NaN is refused");

    const Mat4 v = ViewAt(0, 2, 10);
    CHECK(ValidView(v), "a rigid view is accepted");
    Mat4 scaled = v;
    scaled.m[0] = 2.0f;
    CHECK(!ValidView(scaled), "a scaled model matrix is not a camera");
    CHECK(!ValidView(p), "a projection is not a view");

    // A point straight ahead of the camera lands in the middle; behind it is not ok.
    const Screen mid = Project(p, v, 0, 2, 0);
    CHECK(mid.ok && std::fabs(mid.x - 0.5f) < 1e-4f && std::fabs(mid.y - 0.5f) < 1e-4f, "centre %f %f", mid.x, mid.y);
    CHECK(std::fabs(mid.depth - 10.0f) < 1e-3f, "depth %f", mid.depth);
    CHECK(!Project(p, v, 0, 2, 20).ok, "behind the camera");
    const Screen low = Project(p, v, 0, 0, 0);
    CHECK(low.ok && low.y > 0.5f, "ground below the horizon is lower on screen, y=%f", low.y);
    const Screen right = Project(p, v, 3, 2, 0);
    CHECK(right.x > 0.5f, "world +x is right of centre");
    CHECK(PlausibleCapture(p, v, 0, 1, 0), "bike in front of camera is plausible");
    CHECK(!PlausibleCapture(p, v, 0, 1, 20), "bike behind the camera is not");

    // Mul: identity and a translation.
    const Mat4 vp = Mul(p, v);
    float      c[4] = {0, 2, 0, 1};
    float      w    = vp.m[3] * c[0] + vp.m[7] * c[1] + vp.m[11] * c[2] + vp.m[15] * c[3];
    CHECK(std::fabs(w - 10.0f) < 1e-3f, "VP w %f", w);

    CHECK(Fade(0) == 1.0f && Fade(kFadeFar) == 0.0f && Fade((kFadeNear + kFadeFar) * 0.5f) > 0.4f, "fade");

    // A track of four 100 m straights east, the height climbing 0, 1, 2, 3 m per segment.
    std::vector<unsigned char> segs(4 * 28, 0);
    for (int i = 0; i < 4; ++i) {
        unsigned char* r = &segs[size_t(i) * 28];
        PutF(r + 4, 100.0f);
        PutF(r + 24, float(i));
        if (i == 0) {
            PutF(r + 16, 0.0f);
            PutF(r + 20, 0.0f);
        }
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
    CHECK(vs.size() >= 2 * 50 && vs.size() % 2 == 0, "rows: %zu verts", vs.size());
    CHECK(std::fabs(vs[0].s) < 1e-3f, "starts at the rider");
    bool rising = true, wide = true;
    for (size_t i = 2; i < vs.size(); i += 2)
        if (vs[i].y < vs[i - 2].y - 1e-4f) rising = false;
    for (size_t i = 0; i + 1 < vs.size(); i += 2)
        if (std::fabs(std::fabs(vs[i].z - vs[i + 1].z) - 2 * kHalfWidth) > 1e-3f) wide = false;
    CHECK(rising, "ribbon follows the climbing ground");
    CHECK(wide, "ribbon is the right width");
    CHECK(vs.back().s <= kAhead, "ahead limit");
    CHECK(!rb.update(ref, track, 0.051f, 0.0f), "a small move does not rebuild");
    CHECK(rb.update(ref, track, 0.20f, 0.0f), "a big move rebuilds");
    CHECK(rb.update(ref, track, 0.20f, 1.0f), "a bias change rebuilds");
    std::vector<coachhud::RefPoint> none;
    rb.update(none, track, 0.2f, 0.0f);
    CHECK(rb.verts().empty(), "no sheet, no line");

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("coachline_test: ok\n");
    return 0;
}
