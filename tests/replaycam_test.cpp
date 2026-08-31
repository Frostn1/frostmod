// The rules the keyframed replay camera runs on (src/replaycam.h).
//
// The path is what the viewer actually sees, so the failures that matter are silent ones: a
// pan that unwinds the long way round a +-180 seam, a key list that loses an entry on load,
// a path that keeps driving the camera after its last key. Each of those is a check below.
//
// Like the other tests here this is pure — no Win32, no game — so CI can run it.

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

    // ---- angle wrapping -----------------------------------------------------
    {
        CHECK(Near(WrapDeg(0.0f), 0.0f), "wrap(0)=%f", WrapDeg(0.0f));
        CHECK(Near(WrapDeg(180.0f), 180.0f), "wrap(180)=%f", WrapDeg(180.0f));
        CHECK(Near(WrapDeg(190.0f), -170.0f), "wrap(190)=%f", WrapDeg(190.0f));
        CHECK(Near(WrapDeg(-190.0f), 170.0f), "wrap(-190)=%f", WrapDeg(-190.0f));
        CHECK(Near(WrapDeg(720.0f + 45.0f), 45.0f), "wrap(765)=%f", WrapDeg(765.0f));
        // the seam: -170 is 20 degrees from 170, not 340
        CHECK(Near(UnwrapNear(170.0f, -170.0f), 190.0f), "unwrap=%f", UnwrapNear(170.0f, -170.0f));
        CHECK(Near(UnwrapNear(-170.0f, 170.0f), -190.0f), "unwrap=%f", UnwrapNear(-170.0f, 170.0f));
    }

    // ---- key list -----------------------------------------------------------
    {
        std::vector<Key> keys;
        Upsert(keys, K(300, 3.0f));
        Upsert(keys, K(0,   1.0f));
        Upsert(keys, K(150, 2.0f));
        CHECK(keys.size() == 3, "expected 3 keys, got %zu", keys.size());
        CHECK(keys[0].t == 0 && keys[1].t == 150 && keys[2].t == 300, "keys are not sorted by time");

        // a second key at the same (snapped) ms replaces rather than duplicates
        Upsert(keys, K(148, 9.0f));
        CHECK(keys.size() == 3, "an in-place replace grew the list to %zu", keys.size());
        CHECK(Near(keys[1].x, 9.0f), "replace kept the old value %f", keys[1].x);

        CHECK(PrevKeyTime(keys, 150) == 0, "prev(150)=%d", PrevKeyTime(keys, 150));
        CHECK(NextKeyTime(keys, 150) == 300, "next(150)=%d", NextKeyTime(keys, 150));
        CHECK(PrevKeyTime(keys, 0) == -1, "prev(0) should not exist");
        CHECK(NextKeyTime(keys, 300) == -1, "next(300) should not exist");
        CHECK(IndexAt(keys, 150) == 1, "IndexAt(150)=%d", IndexAt(keys, 150));
        CHECK(IndexAt(keys, 151) == -1, "IndexAt only takes exact snapped times");

        CHECK(!EraseNearest(keys, 600, 30), "erase removed a key 300 ms outside its tolerance");
        CHECK(keys.size() == 3, "a refused erase still changed the list (%zu keys)", keys.size());
        CHECK(EraseNearest(keys, 160, 30), "erase within tolerance found nothing");
        CHECK(keys.size() == 2, "after erase: %zu keys", keys.size());
    }

    // ---- evaluation: coverage ----------------------------------------------
    {
        std::vector<Key> keys;
        Pose p;
        CHECK(!Evaluate(keys, 0, p), "an empty path evaluated");

        Upsert(keys, K(990, 5.0f));
        CHECK(!Evaluate(keys, 990, p), "a one-key path evaluated - it spans no time");

        Upsert(keys, K(1980, 6.0f));
        CHECK(!Evaluate(keys, 989, p), "the path drove the camera before its first key");
        CHECK(!Evaluate(keys, 1981, p), "the path drove the camera after its last key");
        CHECK(Evaluate(keys, 990, p) && Near(p.x, 5.0f), "the first key is not hit exactly");
        CHECK(Evaluate(keys, 1980, p) && Near(p.x, 6.0f), "the last key is not hit exactly");
    }

    // ---- evaluation: keys are passed through exactly -------------------------
    {
        std::vector<Key> keys;
        Upsert(keys, K(0,    0.0f,  10.0f, 40.0f));
        Upsert(keys, K(600,  10.0f, 80.0f, 50.0f));
        Upsert(keys, K(1200, 30.0f, 20.0f, 60.0f));
        Upsert(keys, K(1800, 31.0f, -5.0f, 45.0f));

        for (const Key& k : keys) {
            Pose p;
            if (!Evaluate(keys, k.t, p)) { CHECK(false, "key at %d did not evaluate", k.t); continue; }
            CHECK(Near(p.x, k.x) && Near(p.yaw, k.yaw) && Near(p.fov, k.fov),
                  "key at %d came back as x=%f yaw=%f fov=%f", k.t, p.x, p.yaw, p.fov);
        }

        // between two keys the curve stays inside a sane band and moves monotonically here
        Pose a, b;
        Evaluate(keys, 300, a);
        Evaluate(keys, 400, b);
        CHECK(a.x > 0.0f && a.x < 10.0f, "midpoint x=%f left the segment", a.x);
        CHECK(b.x > a.x, "the path went backwards between 300 and 400 ms");
    }

    // ---- evaluation: two keys interpolate linearly ---------------------------
    {
        std::vector<Key> keys;
        Upsert(keys, K(0,   0.0f));
        Upsert(keys, K(900, 9.0f));
        Pose p;
        CHECK(Evaluate(keys, 300, p) && Near(p.x, 3.0f, 1e-2f), "two-key midpoint x=%f, expected 3", p.x);
        CHECK(Evaluate(keys, 600, p) && Near(p.x, 6.0f, 1e-2f), "two-key midpoint x=%f, expected 6", p.x);
    }

    // ---- evaluation: a pan across the seam takes the short way ---------------
    {
        std::vector<Key> keys;
        Upsert(keys, K(0,   0.0f, 170.0f));
        Upsert(keys, K(600, 0.0f, -170.0f));   // 20 degrees on, not 340 back

        float prev = 170.0f;
        float travel = 0.0f;
        for (int t = 0; t <= 600; t += 30) {
            Pose p;
            if (!Evaluate(keys, t, p)) { CHECK(false, "seam path gap at %d ms", t); break; }
            travel += std::fabs(WrapDeg(p.yaw - prev));
            prev = p.yaw;
        }
        CHECK(travel < 40.0f, "the camera swung %f degrees to cover a 20 degree pan", travel);
    }

    // ---- uneven spacing does not divide by zero or blow up -------------------
    {
        std::vector<Key> keys;
        Upsert(keys, K(0,     0.0f));
        Upsert(keys, K(30,    1.0f));      // one sample apart
        Upsert(keys, K(60000, 2.0f));      // a minute later
        for (int t = 0; t <= 60000; t += 137) {
            Pose p;
            if (!Evaluate(keys, t, p)) continue;
            CHECK(std::isfinite(p.x) && std::isfinite(p.yaw) && std::isfinite(p.fov),
                  "non-finite pose at %d ms", t);
        }
    }

    // ---- file round trip -----------------------------------------------------
    {
        std::vector<Key> keys;
        Upsert(keys, K(0,    1.5f,  90.0f, 40.0f));
        Upsert(keys, K(990,  2.5f, -30.0f, 55.0f));
        keys[0].y = 3.25f; keys[0].z = -4.5f; keys[0].pitch = -12.0f; keys[0].roll = 7.5f;

        std::vector<Key> back;
        std::string err;
        CHECK(Parse(Serialize(keys), back, &err), "round trip failed to parse: %s", err.c_str());
        CHECK(back.size() == keys.size(), "round trip returned %zu of %zu keys", back.size(), keys.size());
        for (size_t i = 0; i < back.size() && i < keys.size(); ++i) {
            CHECK(back[i].t == keys[i].t &&
                  Near(back[i].x, keys[i].x) && Near(back[i].y, keys[i].y) && Near(back[i].z, keys[i].z) &&
                  Near(back[i].yaw, keys[i].yaw) && Near(back[i].pitch, keys[i].pitch) &&
                  Near(back[i].roll, keys[i].roll) && Near(back[i].fov, keys[i].fov),
                  "key %zu changed across the round trip", i);
        }
    }

    // ---- parsing: comments, blank lines, CRLF, out-of-order keys -------------
    {
        std::vector<Key> keys;
        std::string err;
        const std::string text =
            "frostmod-replaycam 1\r\n"
            "# a comment\r\n"
            "\r\n"
            "  600 1 2 3 10 0 0 45\r\n"
            "0 0 0 0 190 0 0 45\r\n";
        CHECK(Parse(text, keys, &err), "failed to parse a well-formed file: %s", err.c_str());
        CHECK(keys.size() == 2, "parsed %zu keys, expected 2", keys.size());
        CHECK(keys[0].t == 0 && keys[1].t == 600, "keys were not sorted on load");
        CHECK(Near(keys[0].yaw, -170.0f), "yaw 190 was not folded on load: %f", keys[0].yaw);
    }

    // ---- parsing: refuses rubbish rather than loading half a path ------------
    {
        std::vector<Key> keys;
        std::string err;
        CHECK(!Parse("", keys, &err), "an empty file parsed");
        CHECK(!Parse("something else 1\n0 0 0 0 0 0 0 45\n", keys, &err), "a foreign file parsed");
        CHECK(!Parse("frostmod-replaycam 99\n", keys, &err), "a future version parsed");
        CHECK(!Parse("frostmod-replaycam 1\n0 0 0\n", keys, &err), "a short key line parsed");
        CHECK(keys.empty(), "a failed parse left %zu keys behind", keys.size());
    }

    // ---- duplicate times collapse on load -----------------------------------
    {
        std::vector<Key> keys;
        std::string err;
        CHECK(Parse("frostmod-replaycam 1\n"
                    "300 1 0 0 0 0 0 45\n"
                    "310 2 0 0 0 0 0 45\n", keys, &err), "parse failed: %s", err.c_str());
        CHECK(keys.size() == 1, "two keys snapping to 300 ms produced %zu keys", keys.size());
    }

    if (g_failures == 0) {
        std::printf("replaycam: all checks passed\n");
        return 0;
    }
    std::printf("replaycam: %d check(s) failed\n", g_failures);
    return 1;
}
