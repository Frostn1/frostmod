// Pip, the Coach pet (src/coachpet.h): where it runs (the ghost's lead, kept in front of the
// rider and on the ribbon, eased), what it does over a jump (flies the call's arc, scared over a
// triple, amazed over a single, happy after the landing), and the sprite it draws. Pure C++.

#include "../src/coachpet.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

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

using namespace coachpet;

// A flat ribbon along +x from the rider, rows every 2 m out to 60 m, 2 m wide, on y = 5.
static std::vector<coachline::Vert> Flat() {
    std::vector<coachline::Vert> v;
    for (float s = 0; s <= 60.0f; s += 2.0f) {
        coachline::Vert l{s, 5.0f, +1.0f, s, {0, 0, 1, 1}};
        coachline::Vert r{s, 5.0f, -1.0f, s, {0, 0, 1, 1}};
        v.push_back(l);
        v.push_back(r);
    }
    return v;
}

static coachjump::Call Jump(coachjump::Kind k, float from, float to, float top) {
    coachjump::Call c;
    c.kind      = k;
    c.takeoff_s = from;
    c.landing_s = to;
    const float len = to - from;
    for (float s = 0; s <= len; s += 1.0f) c.arc.push_back(4.0f * top * (s / len) * (1.0f - s / len));
    return c;
}

static void Lead() {
    std::vector<coachhud::RefPoint> ref;
    for (int i = 0; i <= 100; ++i) ref.push_back({float(i) / 100.0f, float(i) * 0.1f, float(i) * 10.0f, 0.0f});
    const coachline::Arc arc = coachline::ArcOf(ref);
    CHECK(std::fabs(MetreAtTime(ref, arc, 2.05f) - 205.0f) < 0.01f, "ghost at 2.05 s is at %.2f m", double(MetreAtTime(ref, arc, 2.05f)));
    CHECK(MetreAtTime(ref, arc, -1.0f) == 0.0f, "before the line: the first point");
    CHECK(std::isnan(MetreAtTime({}, arc, 1.0f)), "no points: NaN");

    CHECK(LeadTarget(NAN, 100, 1000) == kDefaultLead, "no ghost: the default lead");
    CHECK(LeadTarget(120, 100, 1000) == 20.0f, "ghost 20 m ahead: 20 m");
    CHECK(LeadTarget(90, 100, 1000) == kMinLead, "rider ahead of the ghost: still in front, %.1f", double(LeadTarget(90, 100, 1000)));
    CHECK(LeadTarget(400, 100, 1000) == kMaxLead, "far ahead: kept on the ribbon");
    CHECK(LeadTarget(10, 990, 1000) == 20.0f, "round the line: 20 m");

    Follow f;
    CHECK(f.step(30, 0.016f) == 30.0f, "the first step takes the target");
    const float a = f.step(10, 0.1f);
    CHECK(std::fabs(a - (30.0f - kLeadRate * 0.1f)) < 1e-4f, "eased, not jumped: %.2f", double(a));
}

static void Poses() {
    const std::vector<coachjump::Call> calls = {Jump(coachjump::SINGLE, 100, 120, 3), Jump(coachjump::TRIPLE, 200, 240, 6),
                                                Jump(coachjump::ROLL, 300, 320, 0)};
    Pose p = PoseAt(calls, 110, 1000, 1.0f);
    CHECK(p.air && p.face == WOW, "over the single: wow");
    CHECK(std::fabs(p.flight - 3.0f) < 1e-3f, "the top of the single's arc: %.2f", double(p.flight));
    p = PoseAt(calls, 220, 1000, 1.0f);
    CHECK(p.air && p.face == SCARED && std::fabs(p.flight - 6.0f) < 1e-3f, "over the triple: scared, 6 m up");
    p = PoseAt(calls, 125, 1000, 1.0f);
    CHECK(!p.air && p.face == HAPPY && p.flight == 0.0f, "just landed: happy, on the ground");
    p = PoseAt(calls, 160, 1000, 1.0f);
    CHECK(!p.air && p.face == IDLE, "between jumps: idle");
    p = PoseAt(calls, 310, 1000, 1.0f);
    CHECK(!p.air, "a rolled face is not flown");
    CHECK(PoseAt(calls, 160, 1000, kBlinkEvery * 3 + 0.05f).face == BLINK, "blinks now and then");
}

static void Sprites() {
    for (int f = IDLE; f <= SCARED; ++f)
        for (int r = 0; r < kH; ++r)
            for (int st = -1; st <= 1; ++st) {
                const char* row = Row(r, Face(f), st);
                CHECK(std::strlen(row) == size_t(kW), "face %d row %d is %zu wide", f, r, std::strlen(row));
                for (int c = 0; c < kW; ++c) {
                    float rgba[4];
                    CHECK(row[c] == '.' || Colour(row[c], rgba), "face %d row %d: unknown pixel '%c'", f, r, row[c]);
                }
            }

    const std::vector<coachline::Vert> v = Flat();
    std::vector<coachmark::Quad> out;
    Face face = BLINK;
    CHECK(Build(out, v, {}, 14.0f, 100.0f, 1000.0f, 0.5f, &face), "built on the ribbon");
    CHECK(!out.empty() && face == IDLE, "%zu quads, face %d", out.size(), int(face));
    float lo = 1e9f, hi = -1e9f, xmin = 1e9f, xmax = -1e9f, zmin = 1e9f, zmax = -1e9f;
    for (const coachmark::Quad& q : out) {
        for (int c = 0; c < 4; ++c) {
            lo = std::fmin(lo, q.p[c][1]), hi = std::fmax(hi, q.p[c][1]);
            xmin = std::fmin(xmin, q.p[c][0]), xmax = std::fmax(xmax, q.p[c][0]);
            zmin = std::fmin(zmin, q.p[c][2]), zmax = std::fmax(zmax, q.p[c][2]);
        }
        CHECK(q.s == 14.0f && std::fabs(q.m - 114.0f) < 1e-3f, "placed by its lead and its lap metre");
    }
    CHECK(lo >= 5.0f - 1e-4f && lo <= 5.0f + kHopM + 1e-4f, "stands on the ribbon: feet at %.3f", double(lo));
    CHECK(std::fabs((hi - lo) - kHeight) < 0.01f, "%.2f m tall", double(hi - lo));
    CHECK(std::fabs(xmin - 14.0f) < 1e-4f && std::fabs(xmax - 14.0f) < 1e-4f, "upright across the line, at 14 m");
    CHECK(std::fabs((zmax - zmin) - kHeight) < 0.01f, "as wide as it is tall: %.2f", double(zmax - zmin));

    out.clear();
    const std::vector<coachjump::Call> calls = {Jump(coachjump::DOUBLE, 110, 130, 4)};
    CHECK(Build(out, v, calls, 20.0f, 100.0f, 1000.0f, 0.5f, &face) && face == WOW, "over the double: wow");
    lo = 1e9f;
    for (const coachmark::Quad& q : out)
        for (int c = 0; c < 4; ++c) lo = std::fmin(lo, q.p[c][1]);
    CHECK(std::fabs(lo - 9.0f) < 0.01f, "flies 4 m over the ground at the top: feet at %.2f", double(lo));

    out.clear();
    CHECK(!Build(out, v, {}, 80.0f, 100.0f, 1000.0f, 0.5f) && out.empty(), "past the ribbon: nothing");
}

int main() {
    Lead();
    Poses();
    Sprites();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("coachpet: all checks passed\n");
    return 0;
}
