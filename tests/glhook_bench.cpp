// MXB Coach recorder: what the GL matrix hooks cost a frame, 0.49.0 against 0.49.4, offline.
//
// Not a test (nothing here can fail a build): run it and read the table. It drives each version's
// per-call hook logic, as it stands in src/mxbcoach.cpp, with the real coachline.h / coachperf.h
// code it calls, over a synthetic frame:
//
//   world pass  glMatrixMode(PROJECTION) glLoadIdentity glFrustum, MODELVIEW, then kWorldLoads
//               rigid modelview loads (scenery views; one of them the chase camera 1.6 m from the bike)
//   HUD pass    glMatrixMode(PROJECTION) glLoadIdentity glOrtho, MODELVIEW, then for each of
//               kHudPrims primitives: glLoadMatrixf (a pixel translation: rigid, so 0.49.0's
//               search had to try all 48 axes on it), glTranslatef, glScalef
//
// The "originals" are empty functions behind a pointer, as MinHook's trampolines are; the driver's
// own cost is the same in every column and is not what is measured. Each scenario reports ns per
// frame and ns per hooked call. QPC timing of the offered loads (the instrumentation) is included
// in the 0.49.4 columns.

#include "../src/coachline.h"
#include "../src/coachperf.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <vector>

namespace {

constexpr int kWorldLoads = 1200;   // the 0.48.2 log: ~1200 modelview loads a frame at PRESEASON
constexpr int kHudPrims   = 20000;  // the synthetic heavy HUD the task asks for
constexpr int kFrames     = 60;

enum : unsigned { MODELVIEW = 0x1700, PROJECTION = 0x1701 };

// The originals: opaque to the optimiser, so the hook's call through them is real.
volatile float g_sink = 0;
__declspec(noinline) void OrigMode(unsigned m) { g_sink = g_sink + float(m & 1); }
__declspec(noinline) void OrigLoadf(const float* m) { g_sink = g_sink + m[12]; }
__declspec(noinline) void OrigIdentity() { g_sink = g_sink + 1.0f; }
__declspec(noinline) void OrigFrustum(double l, double, double, double, double, double) { g_sink = g_sink + float(l); }
__declspec(noinline) void OrigOrtho(double l, double, double, double, double, double) { g_sink = g_sink + float(l); }
__declspec(noinline) void OrigTranslatef(float x, float, float) { g_sink = g_sink + x; }
__declspec(noinline) void OrigScalef(float x, float, float) { g_sink = g_sink + x; }
void (*volatile o_mode)(unsigned)                                          = OrigMode;
void (*volatile o_loadf)(const float*)                                     = OrigLoadf;
void (*volatile o_identity)()                                              = OrigIdentity;
void (*volatile o_frustum)(double, double, double, double, double, double) = OrigFrustum;
void (*volatile o_ortho)(double, double, double, double, double, double)   = OrigOrtho;
void (*volatile o_translatef)(float, float, float)                         = OrigTranslatef;
void (*volatile o_scalef)(float, float, float)                             = OrigScalef;

// The state both versions keep.
struct State {
    unsigned                 mode   = MODELVIEW;
    DWORD                    thread = 0;
    bool                     own    = false, building = false;
    float                    snap[3] = {315.1f, 1.39f, 72.0f};  // the bike, telemetry x, y, z
    coachline::ModelviewPick best;
    coachline::AxesLock      lock;
    coachline::ViewRef       ref;
    int                      locked = -1;  // the follow path's axes, when a camera is held
    unsigned                 loads  = 0;
    bool                     wanted = true;  // 0.49.0: g_mv_wanted
    coachperf::HookGate      gate;           // 0.49.4
    coachperf::Acc           acc[coachperf::SLOT_COUNT];
    uint64_t                 frame_ticks[coachperf::SLOT_COUNT] = {};
};
State S;

bool Mine() { return !S.own && S.thread != 0 && GetCurrentThreadId() == S.thread; }

inline uint64_t Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(t.QuadPart);
}

// ---- 0.49.0's per-call logic (src/mxbcoach.cpp at 8ebf071) -----------------------------------
void OldModelLoaded(const float* m) {
    ++S.loads;
    if (!S.wanted) return;
    coachline::Mat4 a;
    std::memcpy(a.m, m, sizeof(a.m));
    if (S.locked >= 0) coachline::ModelviewOffer(S.best, a, S.snap[0], S.snap[1], S.snap[2], S.locked, &S.ref);
    else coachline::ModelviewOffer(S.best, a, S.snap[0], S.snap[1], S.snap[2], -1, &S.ref);
}
void OldMode(unsigned m) {
    if (Mine()) S.mode = m;
    o_mode(m);
}
void OldLoadf(const float* m) {
    if (m && Mine()) {
        if (S.mode == PROJECTION) {
            coachline::Mat4 a;
            std::memcpy(a.m, m, sizeof(a.m));
            if (coachline::ValidProjection(a)) S.building = true;
        } else if (S.mode == MODELVIEW) OldModelLoaded(m);
    }
    o_loadf(m);
}
void OldIdentity() {
    if (Mine() && S.mode == PROJECTION) S.building = false;
    o_identity();
}
void OldFrustum(double l, double r, double b, double t, double n, double f) {
    if (Mine() && S.mode == PROJECTION) {
        coachline::Mat4 p;
        if (coachline::Frustum(l, r, b, t, n, f, p) && coachline::ValidProjection(p)) S.building = true;
    }
    o_frustum(l, r, b, t, n, f);
}
void OldOrtho(double l, double r, double b, double t, double n, double f) {
    if (Mine() && S.mode == PROJECTION) S.building = false;
    o_ortho(l, r, b, t, n, f);
}
void OldTranslatef(float x, float y, float z) {
    if (Mine() && S.mode == PROJECTION && S.building) (void)coachline::Translate(x, y, z);
    o_translatef(x, y, z);
}
void OldScalef(float x, float y, float z) {
    if (Mine() && S.mode == PROJECTION && S.building) (void)coachline::Scale(x, y, z);
    o_scalef(x, y, z);
}

// ---- 0.49.4's per-call logic (src/mxbcoach.cpp, this change) ---------------------------------
void NewSkipped() {
    ++S.loads;
    S.acc[coachperf::HK_MODELVIEW].skip();
}
void NewModelLoaded(const float* m) {
    if (!S.gate.offer()) {
        NewSkipped();
        return;
    }
    ++S.loads;
    const uint64_t  t0 = Qpc();
    coachline::Mat4 a;
    std::memcpy(a.m, m, sizeof(a.m));
    if (S.locked >= 0) coachline::ModelviewOffer(S.best, a, S.snap[0], S.snap[1], S.snap[2], S.locked, &S.ref);
    else coachline::ModelviewOffer(S.best, a, S.snap[0], S.snap[1], S.snap[2], -1, &S.ref);
    const uint64_t d = Qpc() - t0;
    S.acc[coachperf::HK_MODELVIEW].add(d);
    S.frame_ticks[coachperf::HK_MODELVIEW] += d;
}
void NewMode(unsigned m) {
    if (Mine()) S.mode = m;
    o_mode(m);
}
void NewLoadf(const float* m) {
    if (m && Mine()) {
        if (S.mode == MODELVIEW) NewModelLoaded(m);
        else if (S.mode == PROJECTION) {
            const uint64_t t0 = Qpc();
            S.gate.projection(m);
            if (S.gate.live) {
                coachline::Mat4 a;
                std::memcpy(a.m, m, sizeof(a.m));
                if (coachline::ValidProjection(a)) S.building = true;
            }
            S.acc[coachperf::HK_PROJ].add(Qpc() - t0);
        }
    }
    o_loadf(m);
}
void NewIdentity() {
    if (Mine() && S.mode == PROJECTION) S.building = false;
    o_identity();
}
void NewFrustum(double l, double r, double b, double t, double n, double f) {
    if (Mine() && S.mode == PROJECTION) {
        const uint64_t t0 = Qpc();
        S.gate.frustum();
        coachline::Mat4 p;
        if (S.gate.live && coachline::Frustum(l, r, b, t, n, f, p) && coachline::ValidProjection(p)) S.building = true;
        S.acc[coachperf::HK_PROJ].add(Qpc() - t0);
    }
    o_frustum(l, r, b, t, n, f);
}
void NewOrtho(double l, double r, double b, double t, double n, double f) {
    if (Mine() && S.mode == PROJECTION) {
        S.gate.ortho();
        S.building = false;
    }
    o_ortho(l, r, b, t, n, f);
}
void NewTranslatef(float x, float y, float z) {
    if (S.building && Mine() && S.mode == PROJECTION) (void)coachline::Translate(x, y, z);
    o_translatef(x, y, z);
}
void NewScalef(float x, float y, float z) {
    if (S.building && Mine() && S.mode == PROJECTION) (void)coachline::Scale(x, y, z);
    o_scalef(x, y, z);
}

// ---- The frame ------------------------------------------------------------------------------
struct Hooks {
    void (*mode)(unsigned);
    void (*loadf)(const float*);
    void (*identity)();
    void (*frustum)(double, double, double, double, double, double);
    void (*ortho)(double, double, double, double, double, double);
    void (*translatef)(float, float, float);
    void (*scalef)(float, float, float);
};
const Hooks kNone = {OrigMode, OrigLoadf, OrigIdentity, OrigFrustum, OrigOrtho, OrigTranslatef, OrigScalef};
const Hooks kOld  = {OldMode, OldLoadf, OldIdentity, OldFrustum, OldOrtho, OldTranslatef, OldScalef};
const Hooks kNew  = {NewMode, NewLoadf, NewIdentity, NewFrustum, NewOrtho, NewTranslatef, NewScalef};

std::vector<coachline::Mat4> g_world, g_hud;

/// A rigid view whose eye is at (ex, ey, ez) in GL (telemetry x, y, z), yawed by `yaw` radians.
coachline::Mat4 View(float ex, float ey, float ez, float yaw) {
    coachline::Mat4 v;
    const float     c = std::cos(yaw), s = std::sin(yaw);
    // Rotation about y (columns), then t = -R e.
    v.m[0] = c, v.m[1] = 0, v.m[2] = -s, v.m[3] = 0;
    v.m[4] = 0, v.m[5] = 1, v.m[6] = 0, v.m[7] = 0;
    v.m[8] = s, v.m[9] = 0, v.m[10] = c, v.m[11] = 0;
    v.m[12] = -(c * ex + s * ez), v.m[13] = -ey, v.m[14] = -(-s * ex + c * ez), v.m[15] = 1;
    return v;
}

void Build() {
    // The world: view * model for scenery all over the track, most of it far from the bike, and the
    // chase camera's view itself (identity model) once.
    for (int i = 0; i < kWorldLoads; ++i) {
        const float a = float(i) * 0.37f;
        g_world.push_back(View(315.1f + 200.0f * std::cos(a), 3.0f + float(i % 7), 72.0f + 150.0f * std::sin(a), a));
    }
    g_world[kWorldLoads / 2] = View(315.2f, 2.97f, 72.0f, 0.1f);
    // The HUD: a pixel translation per primitive.
    for (int i = 0; i < kHudPrims; ++i) {
        coachline::Mat4 m;
        m.m[12] = float(i % 1920), m.m[13] = float((i * 7) % 1080), m.m[14] = 0;
        g_hud.push_back(m);
    }
}

void Frame(const Hooks& h) {
    h.mode(PROJECTION);
    h.identity();
    h.frustum(-0.1, 0.1, -0.056, 0.056, 0.1, 2000.0);
    h.mode(MODELVIEW);
    for (const coachline::Mat4& m : g_world) h.loadf(m.m);
    h.mode(PROJECTION);
    h.identity();
    h.ortho(0, 1920, 1080, 0, -1, 1);
    h.mode(MODELVIEW);
    for (const coachline::Mat4& m : g_hud) {
        h.loadf(m.m);
        h.translatef(1.0f, 2.0f, 0.0f);
        h.scalef(10.0f, 10.0f, 1.0f);
    }
}

struct Result {
    double ns_frame, ns_hud_call;
};

/// `setup` runs before each frame (the swap's part: what the next frame's hooks are told).
template <class Setup>
Result Run(const Hooks& h, Setup setup) {
    for (int i = 0; i < 5; ++i) setup(), Frame(h), S.best = coachline::ModelviewPick{};  // warm up
    double total = 0;
    for (int f = 0; f < kFrames; ++f) {
        setup();
        const auto t0 = std::chrono::steady_clock::now();
        Frame(h);
        total += double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        S.best = coachline::ModelviewPick{};
    }
    const double per_frame = total / kFrames;
    return {per_frame, 0};
}

}  // namespace

int main() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    S.thread = GetCurrentThreadId();
    Build();
    const int calls = 8 + kWorldLoads + 3 * kHudPrims;
    std::printf("synthetic frame: %d world modelview loads + %d HUD primitives (3 hooked calls each) = %d hooked calls\n\n",
                kWorldLoads, kHudPrims, calls);

    const Result none = Run(kNone, [] {});
    // 0.49.0, riding with the line on, no camera found: every load searched, the HUD's included.
    const Result old_search = Run(kOld, [] { S.wanted = true, S.locked = -1; });
    // 0.49.0 with the camera held: every load offered to the one locked axes.
    const Result old_follow = Run(kOld, [] { S.wanted = true, S.locked = 0; });
    // 0.49.4, live and searching: world loads searched, the HUD pass passed through.
    const Result new_search = Run(kNew, [] { S.locked = -1, S.gate.frame(true, true); });
    const Result new_follow = Run(kNew, [] { S.locked = 0, S.gate.frame(true, true); });
    // 0.49.4 with the search resting (no camera for 3 s), or not riding: everything passes.
    const Result new_rest = Run(kNew, [] { S.locked = -1, S.gate.frame(true, false); });

    auto row = [&](const char* name, const Result& r) {
        std::printf("  %-52s %9.1f us/frame  %6.1f ns/hooked call  (+%8.1f us over no hooks)\n", name, r.ns_frame / 1000.0,
                    r.ns_frame / calls, (r.ns_frame - none.ns_frame) / 1000.0);
    };
    row("no hooks (the originals only)", none);
    row("0.49.0 riding, camera searching (48 axes per load)", old_search);
    row("0.49.0 riding, camera locked (1 axes per load)", old_follow);
    row("0.49.4 riding, searching: world only, HUD passthrough", new_search);
    row("0.49.4 riding, camera locked: world only", new_follow);
    row("0.49.4 search resting / not riding: all passthrough", new_rest);
    std::printf("\n  0.49.0 -> 0.49.4 hook overhead, searching: %.1fx less; locked: %.1fx less\n",
                (old_search.ns_frame - none.ns_frame) / (std::max)(1.0, new_search.ns_frame - none.ns_frame),
                (old_follow.ns_frame - none.ns_frame) / (std::max)(1.0, new_follow.ns_frame - none.ns_frame));
    std::printf("  (sink %f)\n", double(g_sink));
    return 0;
}
