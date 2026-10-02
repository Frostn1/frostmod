// The ground line's colours (src/coachcolor.h): gas green, coast white, light brake yellow, heavy
// brake red, blended and never cut, no blue. Pure C++, runs anywhere.

#include "../src/coachcolor.h"
#include "../src/coachline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

using namespace coachcolor;

// gas 0-100, coast 100-140, heavy brake 140-170, light brake 170-190, gas 190-400.
static std::vector<Sample> Profile() {
    std::vector<Sample> v;
    float               speed = 15;
    for (float s = 0; s <= 400.0f; s += 0.5f) {
        Sample x;
        x.s = s;
        if (s < 100) {
            x.throttle = 1;
            speed += 0.5f * 0.02f;
        } else if (s < 140) {
            x.throttle = 0;
        } else if (s < 170) {
            x.brake = 1;
            speed -= 0.5f * 0.3f;
        } else if (s < 190) {
            x.brake = 0.4f;
            speed -= 0.5f * 0.05f;
        } else {
            x.throttle = 1;
            speed += 0.5f * 0.05f;
        }
        x.speed = speed;
        v.push_back(x);
    }
    return v;
}

// Blue never leads: the hue is never blue-ish.
static bool NoBlue(const Rgba& c) { return c.b <= (std::max)(c.r, c.g) + 1e-4f; }

int main() {
    const Params p;
    const Line   line = Build(Profile(), 0, p);
    CHECK(!line.empty(), "a line");

    // Where the rider is doing each thing, well inside the stretch.
    const Rgba gas = line.at(50), coast = line.at(120), heavy = line.at(155), light = line.at(180);
    CHECK(gas.g > 0.7f && gas.r < 0.3f && gas.b < 0.3f, "gas is green: %.2f %.2f %.2f", gas.r, gas.g, gas.b);
    CHECK(coast.r > 0.9f && coast.g > 0.9f && coast.b > 0.9f, "coast is white: %.2f %.2f %.2f", coast.r, coast.g,
          coast.b);
    CHECK(heavy.r > 0.85f && heavy.g < 0.2f && heavy.b < 0.2f, "heavy brake is red: %.2f %.2f %.2f", heavy.r,
          heavy.g, heavy.b);
    CHECK(light.r > 0.9f && light.g > 0.5f && light.b < 0.35f, "light brake is yellowish: %.2f %.2f %.2f", light.r,
          light.g, light.b);
    CHECK(std::fabs(gas.a - 0.7f) < 1e-4f, "alpha about 0.7");

    // No blue anywhere on the line, and no jump: not more than this over one metre.
    bool  blue = false;
    float worst = 0, worst_s = 0;
    for (float s = 0; s <= 400.0f; s += 0.25f) {
        const Rgba c = line.at(s);
        if (!NoBlue(c)) blue = true;
        const float d = MaxChannelDelta(c, line.at(s + 1.0f));
        if (d > worst) {
            worst   = d;
            worst_s = s;
        }
    }
    CHECK(!blue, "no blue component leads");
    CHECK(worst < 0.30f, "max per-metre colour delta %.3f at %.1f m", double(worst), double(worst_s));

    // Monotone: from coast into the brakes the gradient only climbs (white, yellow, red) and from
    // the brakes back to gas it only descends, never overshooting either end.
    bool up = true, down = true;
    for (float s = 125; s < 150; s += 0.5f)
        if (line.pos_at(s + 0.5f) < line.pos_at(s) - 1e-5f) up = false;
    for (float s = 165; s < 215; s += 0.5f)
        if (line.pos_at(s + 0.5f) > line.pos_at(s) + 1e-5f) down = false;
    CHECK(up, "coast to brake only climbs the gradient");
    CHECK(down, "brake to gas only descends the gradient");
    float lo = 9, hi = -9;
    for (float t : line.positions()) {
        lo = (std::min)(lo, t);
        hi = (std::max)(hi, t);
    }
    CHECK(lo >= -1e-4f && hi <= 3.0001f, "positions stay on the gradient: %.2f..%.2f", double(lo), double(hi));
    bool saw_yellow = false;
    for (float s = 125; s < 150; s += 0.5f) {
        const Rgba c = line.at(s);
        if (c.r > 0.95f && c.g > 0.6f && c.g < 0.95f && c.b < 0.5f) saw_yellow = true;
    }
    CHECK(saw_yellow, "white to red passes through yellow");

    // Brake intensity from the speed trace alone (no brake input) reads as braking too.
    std::vector<Sample> only_speed;
    for (float s = 0; s <= 200.0f; s += 0.5f) {
        Sample x;
        x.s     = s;
        x.speed = s < 100 ? 20.0f : (std::max)(3.0f, 20.0f - 0.35f * (s - 100));  // ~ -7 m/s^2 at first
        only_speed.push_back(x);
    }
    const Line d = Build(only_speed, 0, p);
    CHECK(d.at(120).r > 0.9f && d.at(120).g < 0.8f, "decel alone brakes orange-ish: g=%.2f", double(d.at(120).g));
    CHECK(d.at(50).g > 0.9f && d.at(50).r > 0.9f && d.at(50).b > 0.9f, "steady speed, no input: coast white");

    // Subdivide: tighter where the colour changes, loose where it does not, never a jump.
    const std::vector<float> v = Subdivide(line, 0, 400, 2.0f, p);
    float                    maxd = 0, minstep = 99, maxstep = 0;
    for (size_t i = 1; i < v.size(); ++i) {
        maxd    = (std::max)(maxd, MaxChannelDelta(line.at(v[i - 1]), line.at(v[i])));
        minstep = (std::min)(minstep, v[i] - v[i - 1]);
        maxstep = (std::max)(maxstep, v[i] - v[i - 1]);
        CHECK(v[i] > v[i - 1], "strictly increasing");
    }
    CHECK(v.front() == 0 && v.back() == 400, "covers the run");
    CHECK(maxd <= p.max_delta + 1e-3f || minstep <= p.min_step_m + 1e-4f, "neighbour delta %.3f", double(maxd));
    CHECK(maxstep <= 2.0001f && minstep < 2.0f, "steps %.2f..%.2f", double(minstep), double(maxstep));
    CHECK(v.size() < 400, "not a vertex per sample: %zu", v.size());

    // Wrapping: a line over a lap blends across the start line.
    std::vector<Sample> lap;
    for (float s = 0; s < 400.0f; s += 1.0f) {
        Sample x;
        x.s     = s;
        x.speed = 15;
        if (s > 370 || s < 10) x.brake = 1;
        else x.throttle = 1;
        lap.push_back(x);
    }
    const Line w = Build(lap, 400.0f, p);
    CHECK(w.at(0).r > 0.85f && w.at(0).g < 0.2f, "brake across the line is red");
    CHECK(MaxChannelDelta(w.at(399.5f), w.at(400.5f)) < 0.1f, "seamless across the line");
    CHECK(MaxChannelDelta(w.at(10), w.at(410)) < 1e-4f, "periodic");

    // Fallback with no channels: the cue sheet's braking zones, still blended.
    coachcue::Sheet cs;
    cs.cues.push_back({200.0f, coachcue::BRAKE, 0, "Brake"});
    cs.cues.push_back({240.0f, coachcue::THROTTLE, 0, "Gas"});
    const auto zones = coachline::BrakeZones(cs, 800.0f);
    const Line f     = BuildFromZones(zones, 800.0f, p);
    CHECK(!f.empty(), "fallback line");
    CHECK(f.at(220).r > 0.9f && f.at(220).g < 0.7f, "inside the zone: warm: %.2f %.2f", double(f.at(220).r),
          double(f.at(220).g));
    CHECK(f.at(100).g > 0.7f && f.at(100).r < 0.3f, "away from it: green");
    float fw = 0;
    bool  fb = false;
    for (float s = 0; s < 800; s += 0.25f) {
        fw = (std::max)(fw, MaxChannelDelta(f.at(s), f.at(s + 1)));
        fb = fb || !NoBlue(f.at(s));
    }
    CHECK(fw < 0.30f && !fb, "fallback blends: max/m %.3f", double(fw));
    CHECK(BuildFromZones(std::vector<coachline::Zone>(), 800.0f, p).at(100).g > 0.7f, "no zones, all gas");
    CHECK(BuildFromZones(zones, 0, p).empty(), "no lap length, no line");

    // Bad input.
    CHECK(Build({}, 0, p).empty() && Build({Sample()}, 0, p).empty(), "too few samples");
    std::vector<Sample> nan = Profile();
    nan[100].speed          = NAN;
    nan[200].throttle       = NAN;
    const Line n            = Build(nan, 0, p);
    bool       ok           = !n.empty();
    for (float t : n.positions()) ok = ok && std::isfinite(t);
    CHECK(ok, "NaN in the channels does not poison the line");

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("coachcolor: all checks passed\n");
    return 0;
}
