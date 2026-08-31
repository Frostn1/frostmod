// The rules the keyframed replay camera runs on (src/replaycam.h).
//
// The path is what the viewer actually sees, so the failures that matter are silent ones: a
// pan that unwinds the long way round a +-180 seam, a key list that loses an entry on load,
// a path that keeps driving the camera after its last key, a cut that eases instead of
// cutting, a camera that aims at a rider and points the mirror-image way. Each of those is
// a check below.
//
// Like the other tests here this is pure -- no Win32, no game -- so CI can run it.

#include "../src/replaycam.h"

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n");                                                  \
        }                                                                       \
    } while (0)

using namespace rcam;

static bool Near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }

static Key K(int t, float x, float yaw = 0.0f, float fov = 45.0f) {
    Key k; k.t = t; k.x = x; k.yaw = yaw; k.fov = fov; return k;
}

/// A path of plain smooth keys, uniform knots -- the v1 shape, so the checks that predate
/// v2 keep testing what they were written for.
static Path P(const std::vector<Key>& keys, int curve = CurveUniform) {
    Path p; p.keys = keys; p.curve = curve; return p;
}

int main() {
    // ---- snapping -----------------------------------------------------------
    {
        CHECK(SnapMs(0) == 0, "0 snapped to %d", SnapMs(0));
        CHECK(SnapMs(29) == 30, "29 snapped to %d", SnapMs(29));
        CHECK(SnapMs(14) == 0, "14 snapped to %d", SnapMs(14));
        CHECK(SnapMs(16) == 30, "16 snapped to %d", SnapMs(16));
        CHECK(SnapMs(1000) == 990, "1000 snapped to %d", SnapMs(1000));
        CHECK(SnapMs(-16) == -30, "-16 snapped to %d", SnapMs(-16));
    }

    // ---- angle wrapping ------------------------------------------------------
    {
        CHECK(Near(WrapDeg(0.0f), 0.0f), "wrap(0)=%f", WrapDeg(0.0f));
        CHECK(Near(WrapDeg(180.0f), 180.0f), "wrap(180)=%f", WrapDeg(180.0f));
        CHECK(Near(WrapDeg(190.0f), -170.0f), "wrap(190)=%f", WrapDeg(190.0f));
        CHECK(Near(WrapDeg(-190.0f), 170.0f), "wrap(-190)=%f", WrapDeg(-190.0f));
        CHECK(Near(WrapDeg(720.0f + 45.0f), 45.0f), "wrap(765)=%f", WrapDeg(765.0f));

        CHECK(Near(UnwrapNear(170.0f, -170.0f), 190.0f), "unwrap=%f", UnwrapNear(170.0f, -170.0f));
        CHECK(Near(UnwrapNear(-170.0f, 170.0f), -190.0f), "unwrap=%f", UnwrapNear(-170.0f, 170.0f));
    }

    // ---- the key list --------------------------------------------------------
    {
        std::vector<Key> keys;
        Upsert(keys, K(300, 1.0f));
        Upsert(keys, K(0,   2.0f));
        Upsert(keys, K(150, 3.0f));
        CHECK(keys.size() == 3, "expected 3 keys, got %zu", keys.size());
        CHECK(keys[0].t == 0 && keys[1].t == 150 && keys[2].t == 300, "keys are not sorted by time");

        Upsert(keys, K(150, 9.0f));
        CHECK(keys.size() == 3, "an in-place replace grew the list to %zu", keys.size());
        CHECK(Near(keys[1].x, 9.0f), "replace kept the old value %f", keys[1].x);

        CHECK(PrevKeyTime(keys, 150) == 0, "prev(150)=%d", PrevKeyTime(keys, 150));
        CHECK(NextKeyTime(keys, 150) == 300, "next(150)=%d", NextKeyTime(keys, 150));
        CHECK(PrevKeyTime(keys, 0) == -1, "prev(0) should not exist");
        CHECK(NextKeyTime(keys, 300) == -1, "next(300) should not exist");
        CHECK(IndexAt(keys, 150) == 1, "IndexAt(150)=%d", IndexAt(keys, 150));
        CHECK(IndexAt(keys, 151) == -1, "IndexAt only takes exact snapped times");

        CHECK(PickIndex(keys, 160, 30) == 1, "PickIndex missed the key 10 ms away");
        CHECK(PickIndex(keys, 600, 30) == -1, "PickIndex reached 300 ms outside its tolerance");

        CHECK(!EraseNearest(keys, 600, 30), "erase removed a key 300 ms outside its tolerance");
        CHECK(keys.size() == 3, "a refused erase still changed the list (%zu keys)", keys.size());
        CHECK(EraseNearest(keys, 160, 30), "erase within tolerance found nothing");
        CHECK(keys.size() == 2, "after erase: %zu keys", keys.size());
    }

    // ---- nudging a key -------------------------------------------------------
    {
        std::vector<Key> keys;
        Upsert(keys, K(0, 0.0f));
        Upsert(keys, K(300, 1.0f));
        Upsert(keys, K(600, 2.0f));
        CHECK(NudgeKey(keys, 1, 30) == 330, "nudge forward landed at %d", keys[1].t);
        CHECK(NudgeKey(keys, 1, -60) == 270, "nudge back landed at %d", keys[1].t);
        CHECK(NudgeKey(keys, 1, -300) == -1, "a nudge onto its neighbour was allowed");
        CHECK(keys[1].t == 270, "a refused nudge still moved the key to %d", keys[1].t);
        CHECK(NudgeKey(keys, 0, -90) == -90, "the first key cannot move earlier");
    }

    // ---- evaluation: coverage ------------------------------------------------
    {
        Path p;
        Pose out;
        CHECK(!Evaluate(p, 0, out), "an empty path evaluated");

        Upsert(p.keys, K(990, 5.0f));
        CHECK(!Evaluate(p, 990, out), "a one-key path evaluated - it spans no time");

        Upsert(p.keys, K(1980, 6.0f));
        CHECK(!Evaluate(p, 989, out), "the path drove the camera before its first key");
        CHECK(!Evaluate(p, 1981, out), "the path drove the camera after its last key");
        CHECK(Evaluate(p, 990, out) && Near(out.x, 5.0f), "the first key is not hit exactly");
        CHECK(Evaluate(p, 1980, out) && Near(out.x, 6.0f), "the last key is not hit exactly");
    }

    // ---- evaluation: keys are passed through exactly --------------------------
    {
        Path p = P({});
        Upsert(p.keys, K(0,    0.0f,  10.0f, 40.0f));
        Upsert(p.keys, K(600,  10.0f, 80.0f, 50.0f));
        Upsert(p.keys, K(1200, 30.0f, 20.0f, 60.0f));
        Upsert(p.keys, K(1800, 31.0f, -5.0f, 45.0f));

        for (int curve = 0; curve < CurveCount; ++curve) {
            p.curve = curve;
            for (const Key& k : p.keys) {
                Pose out;
                if (!Evaluate(p, (float)k.t, out)) { CHECK(false, "key at %d did not evaluate", k.t); continue; }
                CHECK(Near(out.x, k.x) && Near(out.yaw, k.yaw) && Near(out.fov, k.fov),
                      "%s: key at %d came back as x=%f yaw=%f fov=%f",
                      CurveName(curve), k.t, out.x, out.yaw, out.fov);
            }
        }

        p.curve = CurveUniform;
        Pose a, b;
        Evaluate(p, 300, a);
        Evaluate(p, 400, b);
        CHECK(a.x > 0.0f && a.x < 10.0f, "midpoint x=%f left the segment", a.x);
        CHECK(b.x > a.x, "the path went backwards between 300 and 400 ms");
    }

    // ---- evaluation: two keys interpolate linearly ---------------------------
    {
        Path p = P({});
        Upsert(p.keys, K(0,   0.0f));
        Upsert(p.keys, K(900, 9.0f));
        Pose out;
        CHECK(Evaluate(p, 300, out) && Near(out.x, 3.0f, 1e-2f), "two-key midpoint x=%f, expected 3", out.x);
        CHECK(Evaluate(p, 600, out) && Near(out.x, 6.0f, 1e-2f), "two-key midpoint x=%f, expected 6", out.x);
    }

    // ---- evaluation: a pan across the seam takes the short way ---------------
    {
        Path p = P({});
        Upsert(p.keys, K(0,   0.0f, 170.0f));
        Upsert(p.keys, K(600, 0.0f, -170.0f));   // 20 degrees on, not 340 back

        float prev = 170.0f, travel = 0.0f;
        for (int t = 0; t <= 600; t += 30) {
            Pose out;
            if (!Evaluate(p, (float)t, out)) { CHECK(false, "seam path gap at %d ms", t); break; }
            travel += std::fabs(WrapDeg(out.yaw - prev));
            prev = out.yaw;
        }
        CHECK(travel < 40.0f, "the camera swung %f degrees to cover a 20 degree pan", travel);
    }

    // ---- uneven spacing does not divide by zero or blow up -------------------
    {
        Path p = P({});
        Upsert(p.keys, K(0,     0.0f));
        Upsert(p.keys, K(30,    1.0f));      // one sample apart
        Upsert(p.keys, K(60000, 2.0f));      // a minute later
        for (int curve = 0; curve < CurveCount; ++curve) {
            p.curve = curve;
            for (int t = 0; t <= 60000; t += 137) {
                Pose out;
                if (!Evaluate(p, (float)t, out)) continue;
                CHECK(std::isfinite(out.x) && std::isfinite(out.yaw) && std::isfinite(out.fov),
                      "%s: non-finite pose at %d ms", CurveName(curve), t);
            }
        }
    }

    // ---- centripetal knots stop a tight pair throwing a loop -----------------
    // Two keys a tenth of a metre apart, then ten metres away. On time knots the tangent
    // the far key contributes is far bigger than the first segment, so the camera lurches
    // backwards before it sets off. That lurch is the whole reason centripetal is default.
    {
        Path p = P({});
        Upsert(p.keys, K(0,   0.0f));
        Upsert(p.keys, K(300, 0.1f));
        Upsert(p.keys, K(600, 10.0f));

        float worst[CurveCount] = { 0.0f, 0.0f };
        for (int curve = 0; curve < CurveCount; ++curve) {
            p.curve = curve;
            for (int t = 0; t <= 300; t += 5) {
                Pose out;
                if (!Evaluate(p, (float)t, out)) continue;
                if (-out.x > worst[curve]) worst[curve] = -out.x;   // how far behind the start
            }
        }
        CHECK(worst[CurveUniform] > 0.2f,
              "the uniform lurch this test is about did not happen (%f m)", worst[CurveUniform]);
        CHECK(worst[CurveCentripetal] < worst[CurveUniform] * 0.25f,
              "centripetal lurched %f m against uniform's %f m",
              worst[CurveCentripetal], worst[CurveUniform]);
    }

    // ---- hold: the camera arrives and settles instead of sailing through -----
    {
        Path p = P({});
        Upsert(p.keys, K(0,   0.0f));
        Upsert(p.keys, K(300, 5.0f));
        Upsert(p.keys, K(600, 10.0f));

        Pose before, after;
        Evaluate(p, 270, before); Evaluate(p, 330, after);
        const float smoothSpeed = std::fabs(after.x - before.x);

        p.keys[1].ease = EaseHold;
        Evaluate(p, 270, before); Evaluate(p, 330, after);
        const float holdSpeed = std::fabs(after.x - before.x);
        CHECK(holdSpeed < smoothSpeed * 0.35f,
              "a hold key moved %f m across itself where smooth moved %f m", holdSpeed, smoothSpeed);

        Pose at;
        CHECK(Evaluate(p, 300, at) && Near(at.x, 5.0f), "a hold key is no longer hit exactly");
    }

    // ---- cut: the shot is locked off, then jumps ------------------------------
    {
        Path p = P({});
        Upsert(p.keys, K(0,   0.0f, 10.0f));
        Upsert(p.keys, K(300, 5.0f, 20.0f));
        Upsert(p.keys, K(600, 10.0f, 90.0f));
        p.keys[1].ease = EaseCut;

        Pose a, b, c;
        CHECK(Evaluate(p, 330, a) && Near(a.x, 5.0f) && Near(a.yaw, 20.0f),
              "a cut did not park on its key (x=%f yaw=%f)", a.x, a.yaw);
        CHECK(Evaluate(p, 570, b) && Near(b.x, 5.0f), "the parked shot drifted to x=%f", b.x);
        CHECK(Evaluate(p, 600, c) && Near(c.x, 10.0f), "the cut did not land on the next key");
        CHECK(ShotCount(p.keys) == 2, "a path with one cut has %d shots", ShotCount(p.keys));
        CHECK(ShotOfKey(p.keys, 0) == 1 && ShotOfKey(p.keys, 2) == 2, "shots are numbered wrong");

        // The segment before a cut arrives and settles rather than carrying speed into it.
        Pose e1, e2;
        Evaluate(p, 240, e1); Evaluate(p, 300, e2);
        CHECK(std::fabs(e2.x - e1.x) < 1.4f, "the camera was still moving %f m into a cut",
              std::fabs(e2.x - e1.x));
    }

    // ---- the rig -------------------------------------------------------------
    {
        Pose base; base.x = 1.0f; base.y = 2.0f; base.z = 3.0f; base.yaw = 10.0f;

        Pose locked = base;
        ApplyRig(RigLocked, 1.0f, 12.0f, locked);
        CHECK(Near(locked.x, base.x) && Near(locked.yaw, base.yaw), "a locked rig moved the camera");

        Pose off = base;
        ApplyRig(RigHandheld, 0.0f, 12.0f, off);
        CHECK(Near(off.x, base.x), "amount 0 still shook the camera");

        Pose a = base, b = base;
        ApplyRig(RigHandheld, 1.0f, 12.0f, a);
        ApplyRig(RigHandheld, 1.0f, 12.0f, b);
        CHECK(Near(a.x, b.x) && Near(a.yaw, b.yaw), "the rig is not reproducible");
        CHECK(!Near(a.x, base.x, 1e-6f) || !Near(a.yaw, base.yaw, 1e-6f), "the rig did nothing at all");

        // Bounded, and continuous: a frame's worth of time is a frame's worth of movement.
        for (int i = 0; i < 4000; ++i) {
            const float t = (float)i * 0.017f;
            Pose p1 = base, p2 = base;
            ApplyRig(RigDrone, 2.0f, t, p1);
            ApplyRig(RigDrone, 2.0f, t + 0.017f, p2);
            CHECK(std::fabs(p1.x - base.x) < 0.5f && std::fabs(p1.y - base.y) < 0.5f,
                  "the drone rig moved %f m at t=%f", p1.y - base.y, t);
            CHECK(std::fabs(p2.x - p1.x) < 0.02f, "the rig jumped %f m in one frame", p2.x - p1.x);
        }
    }

    // ---- aiming --------------------------------------------------------------
    {
        Riders riders;
        riders.n = 1;
        riders.r[0].raceNum = 7; riders.r[0].x = 10.0f; riders.r[0].y = 1.0f; riders.r[0].z = 0.0f;
        Conv conv;

        Path p = P({});
        Upsert(p.keys, K(0,   0.0f, 123.0f));    // a keyed yaw the aim must override
        Upsert(p.keys, K(600, 0.0f, 123.0f));
        p.keys[0].target = 7;

        Pose out;
        CHECK(Evaluate(p, 300, &riders, conv, out), "an aimed path did not evaluate");
        CHECK(Near(out.yaw, 90.0f, 0.5f), "aiming at +X gave yaw %f, expected 90", out.yaw);
        CHECK(out.pitch > 3.0f && out.pitch < 8.0f, "aiming 1 m up at 10 m gave pitch %f", out.pitch);

        // A rider nobody is reporting leaves the keyed angles alone.
        p.keys[0].target = 99;
        CHECK(Evaluate(p, 300, &riders, conv, out) && Near(out.yaw, 123.0f),
              "an absent target overrode the keyed yaw with %f", out.yaw);

        // No riders at all (injected mode) is the same story.
        p.keys[0].target = 7;
        CHECK(Evaluate(p, 300, out) && Near(out.yaw, 123.0f),
              "aiming happened with no rider data (%f)", out.yaw);

        // Look-off: two targets on one segment ease from one to the other.
        riders.n = 2;
        riders.r[1].raceNum = 8; riders.r[1].x = 0.0f; riders.r[1].y = 1.0f; riders.r[1].z = 10.0f;
        p.keys[1].target = 8;
        Pose s, m, e;
        Evaluate(p, 0,   &riders, conv, s);
        Evaluate(p, 300, &riders, conv, m);
        Evaluate(p, 600, &riders, conv, e);
        CHECK(Near(s.yaw, 90.0f, 0.5f), "the look-off did not start on rider 7 (%f)", s.yaw);
        CHECK(Near(e.yaw, 0.0f, 0.5f),  "the look-off did not end on rider 8 (%f)", e.yaw);
        CHECK(m.yaw > 20.0f && m.yaw < 70.0f, "the look-off midpoint sat at %f", m.yaw);
    }

    // ---- framing -------------------------------------------------------------
    {
        CHECK(Near(FramedFov(45.0f, 10.0f, 10.0f), 45.0f, 0.01f), "framing changed fov at the same distance");
        const float closer = FramedFov(45.0f, 10.0f, 5.0f);
        const float farther = FramedFov(45.0f, 10.0f, 20.0f);
        CHECK(closer > 45.0f, "half the distance should widen the fov, got %f", closer);
        CHECK(farther < 45.0f, "twice the distance should narrow the fov, got %f", farther);
        CHECK(FramedFov(45.0f, 0.0f, 5.0f) == 45.0f, "framing with no keyed distance changed the fov");
    }

    // ---- solving the angle convention ---------------------------------------
    {
        // Build samples from a camera whose globals count backwards and are offset half a
        // turn, with pitch inverted too - the worst case the solver has to recover.
        const float yawSign = -1.0f, yawOffset = 180.0f, pitchSign = -1.0f;
        ConvSample s[8];
        for (int i = 0; i < 8; ++i) {
            const float head = -150.0f + (float)i * 40.0f;
            const float elev = -20.0f + (float)i * 6.0f;
            const float ch = std::cos(elev * kRadPerDeg);
            s[i].fx = std::sin(head * kRadPerDeg) * ch;
            s[i].fy = std::sin(elev * kRadPerDeg);
            s[i].fz = std::cos(head * kRadPerDeg) * ch;
            s[i].yaw   = WrapDeg(yawSign * (head - yawOffset));
            s[i].pitch = pitchSign * elev;
        }
        Conv c;
        CHECK(SolveConvention(s, 8, c), "the solver could not read a clean set of samples");
        CHECK(c.solved && c.yawSign == yawSign && Near(c.yawOffset, yawOffset) && c.pitchSign == pitchSign,
              "solved sign=%f offset=%f pitch=%f", c.yawSign, c.yawOffset, c.pitchSign);

        // And that it aims correctly once solved: a target dead ahead of +Z.
        float yaw = 0, pitch = 0;
        AimAngles(c, 0, 0, 0, 0, 0, 10, yaw, pitch);
        CHECK(Near(WrapDeg(c.yawSign * yaw + c.yawOffset), 0.0f, 0.01f),
              "aiming down +Z with the solved convention gave a heading of %f",
              WrapDeg(c.yawSign * yaw + c.yawOffset));

        Conv c2;
        CHECK(!SolveConvention(s, 2, c2), "two samples were enough to solve a convention");
        ConvSample still[4];
        for (int i = 0; i < 4; ++i) { still[i] = s[0]; }
        CHECK(!SolveConvention(still, 4, c2), "a camera that never turned solved a convention");

        ConvSample bad[8];
        for (int i = 0; i < 8; ++i) { bad[i] = s[i]; bad[i].yaw = WrapDeg(bad[i].yaw + (i % 2 ? 37.0f : -37.0f)); }
        CHECK(!SolveConvention(bad, 8, c2), "samples that fit nothing still solved");
        CHECK(!c2.solved, "a refused solve marked itself solved");
    }

    // ---- the track-position axis --------------------------------------------
    {
        Path p;
        p.anchor = AnchorTrack;
        for (int i = 0; i < 3; ++i) { Key k; k.t = i * 300; k.x = (float)i; p.keys.push_back(k); }
        p.keys[0].tp = 0.90f; p.keys[1].tp = 0.95f; p.keys[2].tp = 0.02f;   // across the line

        std::vector<float> u;
        Params(p, u);
        CHECK(ParamsValid(p, u), "an unwrapped lap axis came back unusable");
        CHECK(Near(u[2], 1.02f), "the lap wrap unwrapped to %f, expected 1.02", u[2]);

        float now = 0;
        CHECK(ParamNow(p, u, 0, 0.98f, now) && Near(now, 0.98f), "before the line mapped to %f", now);
        CHECK(ParamNow(p, u, 0, 0.01f, now) && Near(now, 1.01f), "after the line mapped to %f", now);
        CHECK(!ParamNow(p, u, 0, -1.0f, now), "a path anchored to a rider nobody is reporting still played");

        Pose out;
        CHECK(Evaluate(p, 0.90f, out) && Near(out.x, 0.0f), "the first key is not hit on the lap axis");
        CHECK(Evaluate(p, 1.02f, out) && Near(out.x, 2.0f), "the last key is not hit on the lap axis");
        CHECK(!Evaluate(p, 0.50f, out), "the path drove the camera half a lap away");

        // A key with no lap fraction cannot be on the lap axis at all.
        p.keys[1].tp = -1.0f;
        Params(p, u);
        CHECK(!ParamsValid(p, u), "a key with no lap fraction was accepted on the lap axis");
    }

    // ---- retiming by distance ------------------------------------------------
    {
        std::vector<Key> keys;
        Key a, b, c;
        a.t = 0;    a.x = 0.0f;
        b.t = 300;  b.x = 1.0f;      // a metre in the first third...
        c.t = 900;  c.x = 100.0f;    // ...and ninety-nine in the rest
        keys = { a, b, c };
        CHECK(RetimeByDistance(keys), "retiming refused a path with plenty of room");
        CHECK(keys.front().t == 0 && keys.back().t == 900, "retiming moved the ends to %d..%d",
              keys.front().t, keys.back().t);
        CHECK(keys[1].t < 60, "the one-metre leg kept %d ms of a 900 ms path", keys[1].t);

        // A cut is a shot someone timed. Distance cannot argue with it.
        std::vector<Key> held = { a, b, c };
        held[0].ease = EaseCut;                       // 0..300 is a 300 ms locked-off shot
        CHECK(RetimeByDistance(held), "retiming refused a path with a cut in it");
        CHECK(held[1].t - held[0].t == 300, "the cut shot became %d ms", held[1].t - held[0].t);

        std::vector<Key> still = { a, a, a };
        still[1].t = 300; still[2].t = 900;
        CHECK(!RetimeByDistance(still), "a path that never moves was retimed anyway");
    }

    // ---- file round trip -----------------------------------------------------
    {
        Path p;
        p.curve = CurveUniform; p.rig = RigDrone; p.rigAmount = 1.25f;
        p.anchor = AnchorClock; p.autoFov = true;
        Upsert(p.keys, K(0,    1.5f,  90.0f, 40.0f));
        Upsert(p.keys, K(990,  2.5f, -30.0f, 55.0f));
        p.keys[0].y = 3.25f; p.keys[0].z = -4.5f; p.keys[0].pitch = -12.0f; p.keys[0].roll = 7.5f;
        p.keys[0].ease = EaseHold; p.keys[0].target = 42; p.keys[0].aimDist = 18.5f; p.keys[0].tp = 0.25f;
        p.keys[1].ease = EaseCut;

        Path back;
        std::string err;
        CHECK(Parse(Serialize(p), back, &err), "round trip failed to parse: %s", err.c_str());
        CHECK(back.keys.size() == p.keys.size(), "round trip returned %zu of %zu keys",
              back.keys.size(), p.keys.size());
        CHECK(back.curve == p.curve && back.rig == p.rig && Near(back.rigAmount, p.rigAmount) &&
              back.anchor == p.anchor && back.autoFov == p.autoFov,
              "the path settings changed across the round trip");
        for (size_t i = 0; i < back.keys.size() && i < p.keys.size(); ++i) {
            const Key& x = back.keys[i]; const Key& y = p.keys[i];
            CHECK(x.t == y.t && Near(x.x, y.x) && Near(x.y, y.y) && Near(x.z, y.z) &&
                  Near(x.yaw, y.yaw) && Near(x.pitch, y.pitch) && Near(x.roll, y.roll) &&
                  Near(x.fov, y.fov) && x.ease == y.ease && x.target == y.target &&
                  Near(x.aimDist, y.aimDist) && Near(x.tp, y.tp),
                  "key %zu changed across the round trip", i);
        }
    }

    // ---- parsing: comments, blank lines, CRLF, out-of-order keys -------------
    {
        Path p;
        std::string err;
        const std::string text =
            "frostmod-replaycam 1\r\n"
            "# a comment\r\n"
            "\r\n"
            "  600 1 2 3 10 0 0 45\r\n"
            "0 0 0 0 190 0 0 45\r\n";
        CHECK(Parse(text, p, &err), "failed to parse a well-formed file: %s", err.c_str());
        CHECK(p.keys.size() == 2, "parsed %zu keys, expected 2", p.keys.size());
        CHECK(p.keys[0].t == 0 && p.keys[1].t == 600, "keys were not sorted on load");
        CHECK(Near(p.keys[0].yaw, -170.0f), "yaw 190 was not folded on load: %f", p.keys[0].yaw);
    }

    // ---- parsing: a v1 file still flies -------------------------------------
    {
        Path p;
        std::string err;
        CHECK(Parse("frostmod-replaycam 1\n"
                    "0 0 0 0 0 0 0 45\n"
                    "300 1 0 0 0 0 0 45\n", p, &err), "a v1 file no longer loads: %s", err.c_str());
        CHECK(p.keys.size() == 2, "the v1 file lost keys");
        CHECK(p.keys[0].ease == EaseSmooth && p.keys[0].target == kNoTarget,
              "a v1 key came back with something other than the v1 defaults");
        CHECK(p.anchor == AnchorClock && p.rig == RigLocked,
              "a v1 file came back with a rig or an anchor it never had");
    }

    // ---- parsing: refuses rubbish rather than loading half a path ------------
    {
        Path p;
        std::string err;
        CHECK(!Parse("", p, &err), "an empty file parsed");
        CHECK(!Parse("something else 1\n0 0 0 0 0 0 0 45\n", p, &err), "a foreign file parsed");
        CHECK(!Parse("frostmod-replaycam 99\n", p, &err), "a future version parsed");
        CHECK(!Parse("frostmod-replaycam 1\n0 0 0\n", p, &err), "a short key line parsed");
        CHECK(!Parse("frostmod-replaycam 2\n0 0 0 0 0 0 0 45 sideways -1 0 -1\n", p, &err),
              "an unknown ease parsed");
        CHECK(!Parse("frostmod-replaycam 2\npath spiral locked 1.0 clock 0\n", p, &err),
              "an unknown curve parsed");
        CHECK(!Parse("frostmod-replaycam 2\npath uniform locked 1.0 track 0\n"
                     "0 0 0 0 0 0 0 45 smooth -1 0 -1\n"
                     "300 1 0 0 0 0 0 45 smooth -1 0 -1\n", p, &err),
              "a track-anchored file with no lap fractions parsed");
        CHECK(p.keys.empty(), "a failed parse left %zu keys behind", p.keys.size());
    }

    // ---- duplicate times collapse on load -----------------------------------
    {
        Path p;
        std::string err;
        CHECK(Parse("frostmod-replaycam 1\n"
                    "300 1 0 0 0 0 0 45\n"
                    "310 2 0 0 0 0 0 45\n", p, &err), "parse failed: %s", err.c_str());
        CHECK(p.keys.size() == 1, "two keys snapping to 300 ms produced %zu keys", p.keys.size());
    }

    if (g_failures == 0) {
        std::printf("replaycam: all checks passed\n");
        return 0;
    }
    std::printf("replaycam: %d check(s) failed\n", g_failures);
    return 1;
}
