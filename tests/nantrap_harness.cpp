// Proves the live trap (src/nantrap.cpp) on a real 0/0, on Windows, with the same Enter/Leave
// the sim-step hook uses:
//   - the first invalid op inside an armed window traps, and the record names this exe, the
//     divss, and the operands;
//   - the faulting instruction is resumed and gives the same NaN it gives unarmed;
//   - MXCSR after Leave is bit-for-bit the MXCSR before Enter;
//   - after the one report the trap disarms;
//   - an invalid op on a thread that is not inside an armed window is NOT swallowed;
//   - the sidecar is written where MXB App looks, with kind nan_first_fault.
// Then it times Enter/Leave, which is the per-step cost when the trap is on.

#include "../src/nantrap.h"

#include <windows.h>
#include <xmmintrin.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n  (%s)\n", #cond);                                   \
        }                                                                       \
    } while (0)

static std::string g_log;
static void Log(const char* line) {
    g_log += line;
    g_log += "\n";
}

// Stands in for the sim step: one scalar divide, not inlined, so the fault is in this frame.
__declspec(noinline) static float Step(volatile float* a, volatile float* b) { return *a / *b; }

// The hook's shape, exactly: Enter, call, Leave.
static float ArmedStep(volatile float* a, volatile float* b) {
    if (!nantrap::Armed()) return Step(a, b);
    const uint32_t saved = nantrap::Enter();
    const float r = Step(a, b);
    nantrap::Leave(saved);
    return r;
}

static DWORD WINAPI OtherThread(LPVOID out) {
    // Unmasked by hand, outside any armed window: this must reach our own __except, not be
    // resumed by the trap's handler.
    volatile float z = 0.0f;
    const unsigned saved = _mm_getcsr();
    _mm_setcsr(nantrap::ArmedMxcsr(saved));
    int caught = 0;
    __try {
        volatile float r = z / z;
        (void)r;
    } __except (GetExceptionCode() == EXCEPTION_FLT_INVALID_OPERATION ||
                        GetExceptionCode() == 0xC00002B5
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        caught = 1;
    }
    _mm_setcsr(saved);
    *(int*)out = caught;
    return 0;
}

int main() {
    char dir[MAX_PATH];
    GetTempPathA(sizeof(dir), dir);
    std::string d = std::string(dir) + "nantrap_harness";
    CreateDirectoryA(d.c_str(), nullptr);
    // Clear earlier runs' sidecars.
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((d + "\\frostmod-crash-*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do DeleteFileA((d + "\\" + fd.cFileName).c_str()); while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    frostmod::crash::Install(Log, d.c_str(), "harness");
    volatile float zero = 0.0f;

    // Unarmed: 0/0 is a quiet NaN and nothing traps. This is the stock behaviour to match.
    const unsigned before0 = _mm_getcsr();
    const float stock = Step(&zero, &zero);
    CHECK(std::isnan(stock), "stock 0/0 is NaN");
    _mm_setcsr(before0);   // drop the IE flag the stock divide set

    nantrap::Config cfg;
    cfg.on = true;
    nantrap::Install(Log, cfg);
    CHECK(nantrap::Armed(), "armed after Install");

    // A distinctive MXCSR (round toward zero, FTZ, and a stale PE flag) to prove "exactly".
    const unsigned game = 0xFF80u | 0x0020u;
    _mm_setcsr(game);
    const float first = ArmedStep(&zero, &zero);
    const unsigned after = _mm_getcsr();
    CHECK(std::isnan(first), "resumed instruction still gives NaN");
    std::uint32_t fb, sb;
    std::memcpy(&fb, (const void*)&first, 4);
    std::memcpy(&sb, (const void*)&stock, 4);
    CHECK(fb == sb, "same NaN bits as stock (%08X vs %08X)", fb, sb);
    CHECK(after == game, "MXCSR restored exactly (%08X vs %08X)", after, game);
    _mm_setcsr(before0);

    CHECK(nantrap::Reports() == 1, "one report (got %u)", nantrap::Reports());
    const nantrap::Record& r = nantrap::Last();
    CHECK(r.code == 0xC0000090u || r.code == 0xC00002B5u, "code %08X", r.code);
    CHECK(std::strstr(r.site, "nantrap_harness.exe+0x") == r.site, "site in this exe: %s", r.site);
    CHECK(std::strcmp(r.site, r.gameSite) == 0, "game site is the site: %s", r.gameSite);
    char op[64];
    nantrap::OpHint(r, op, sizeof(op));
    CHECK(std::strncmp(op, "divss", 5) == 0, "op hint: %s", op);
    CHECK(r.frameCount >= 2, "a stack (%d frames)", r.frameCount);
    CHECK(!nantrap::IsArmed(0x1F80) && nantrap::IsArmed(r.mxcsr), "trap saw the armed MXCSR %08X", r.mxcsr);
    CHECK(g_log.find("[nantrap] FIRST invalid FP op #1") != std::string::npos, "logged:\n%s", g_log.c_str());

    // Disarmed for the session now: the next 0/0 is stock and costs nothing.
    CHECK(!nantrap::Armed(), "disarmed after the report");
    const float second = ArmedStep(&zero, &zero);
    CHECK(std::isnan(second) && nantrap::Reports() == 1, "second divide is stock");
    _mm_setcsr(before0);

    // Not ours: another thread, no armed window.
    int caught = 0;
    HANDLE t = CreateThread(nullptr, 0, OtherThread, &caught, 0, nullptr);
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
    CHECK(caught == 1, "an unarmed thread's fault is passed on, not swallowed");

    // The sidecar MXB App picks up.
    h = FindFirstFileA((d + "\\frostmod-crash-*-nan1.json").c_str(), &fd);
    CHECK(h != INVALID_HANDLE_VALUE, "sidecar written in %s", d.c_str());
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        FILE* f = nullptr;
        std::string body;
        if (fopen_s(&f, (d + "\\" + fd.cFileName).c_str(), "rb") == 0 && f) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
            fclose(f);
        }
        CHECK(body.find("\"kind\": \"nan_first_fault\"") != std::string::npos, "kind in sidecar");
        CHECK(body.find("\"text\": \"nantrap: first invalid op") != std::string::npos, "trail note in sidecar");
    }

    // Overhead: Enter/Leave around an empty call, against the bare call. This is what each sim
    // step pays while the trap is armed (once disarmed the hook is one load and a branch).
    {
        const int N = 5'000'000;
        volatile float one = 1.0f;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i) (void)Step(&one, &one);
        auto t1 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i) {
            const uint32_t s = nantrap::Enter();
            (void)Step(&one, &one);
            nantrap::Leave(s);
        }
        auto t2 = std::chrono::steady_clock::now();
        const double bare = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
        const double armed = std::chrono::duration<double, std::nano>(t2 - t1).count() / N;
        std::printf("overhead: %.1f ns per step armed vs %.1f ns bare (+%.1f ns; at 1000 steps/s "
                    "that is %.4f%% of one core)\n",
                    armed, bare, armed - bare, (armed - bare) * 1000.0 / 1e9 * 100.0);
    }

    if (g_failures) {
        std::printf("%d failure(s)\nlog:\n%s", g_failures, g_log.c_str());
        return 1;
    }
    std::printf("nantrap_harness: all passed (site %s, %s)\n", r.site, op);
    return 0;
}
