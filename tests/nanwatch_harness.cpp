// The live trap's workshop additions (src/nantrap.cpp), on Windows:
//   - the spawn 0*inf, rebuilt here byte for byte (divss xmm2, [r12]; call the vector scale whose
//     mulss xmm0, [rdx] is 0 * inf), is resumed without a report and gives the stock NaN;
//   - with nantrapov=1 the first overflow traps, is reported as "overflow", resumes with the
//     stock inf (no second trap on the same instruction), and MXCSR comes back exactly.

#include "../src/nantrap.h"

#include <windows.h>
#include <xmmintrin.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
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

// rcx = out[3], rdx = point[3], r8 = &divisor. The game's 0x19F68E..0x19F69B and 0x267CF0.
using SpawnFn = void (*)(float* out, const float* point, const float* divisor);
static SpawnFn BuildSpawn() {
    static const uint8_t caller[] = {
        0x41, 0x54,                          // push r12
        0x48, 0x83, 0xEC, 0x20,              // sub rsp, 0x20
        0x4D, 0x8B, 0xE0,                    // mov r12, r8
        0xB8, 0x00, 0x00, 0x80, 0x3F,        // mov eax, 1.0f
        0x66, 0x0F, 0x6E, 0xD0,              // movd xmm2, eax
        0xF3, 0x41, 0x0F, 0x5E, 0x14, 0x24,  // divss xmm2, [r12]   (r / 0 = inf)
        0xE8, 0, 0, 0, 0,                    // call scale
        0x48, 0x83, 0xC4, 0x20,              // add rsp, 0x20
        0x41, 0x5C,                          // pop r12
        0xC3,                                // ret
    };
    static const uint8_t scale[] = {
        0x0F, 0x28, 0xC2,                    // movaps xmm0, xmm2
        0x0F, 0x28, 0xCA,                    // movaps xmm1, xmm2
        0xF3, 0x0F, 0x59, 0x02,              // mulss xmm0, [rdx]   (0 * inf)
        0xF3, 0x0F, 0x11, 0x01,              // movss [rcx], xmm0
        0xF3, 0x0F, 0x59, 0x4A, 0x04,        // mulss xmm1, [rdx+4]
        0xF3, 0x0F, 0x11, 0x49, 0x04,        // movss [rcx+4], xmm1
        0xF3, 0x0F, 0x59, 0x52, 0x08,        // mulss xmm2, [rdx+8]
        0xF3, 0x0F, 0x11, 0x51, 0x08,        // movss [rcx+8], xmm2
        0xC3,                                // ret
    };
    auto* mem = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return nullptr;
    std::memcpy(mem, caller, sizeof(caller));
    const size_t scaleAt = 64;
    std::memcpy(mem + scaleAt, scale, sizeof(scale));
    const size_t callAt = 24;   // the E8
    const int32_t rel = (int32_t)(scaleAt - (callAt + 5));
    std::memcpy(mem + callAt + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), mem, 4096);
    return (SpawnFn)mem;
}

__declspec(noinline) static float Overflow(volatile float* a, volatile float* b) { return *a * *b; }

int main() {
    char dir[MAX_PATH];
    GetTempPathA(sizeof(dir), dir);
    std::string d = std::string(dir) + "nanwatch_harness";
    CreateDirectoryA(d.c_str(), nullptr);
    frostmod::crash::Install(Log, d.c_str(), "harness");

    SpawnFn spawn = BuildSpawn();
    CHECK(spawn != nullptr, "code page");
    const float point[3] = {0, 0, 0};
    const float zero = 0.0f;
    float stockOut[3] = {1, 1, 1};
    const unsigned before0 = _mm_getcsr();
    spawn(stockOut, point, &zero);
    _mm_setcsr(before0);
    CHECK(std::isnan(stockOut[0]), "stock spawn scale makes a NaN (0 * inf)");

    nantrap::Config cfg;
    cfg.on = true;
    cfg.max = 8;
    cfg.overflow = true;
    nantrap::Install(Log, cfg);
    CHECK(nantrap::Armed(), "armed");

    // The spawn 0*inf: resumed, not reported, the same NaN as stock.
    const unsigned game = 0x1F80u;
    _mm_setcsr(game);
    float armedOut[3] = {1, 1, 1};
    {
        const uint32_t saved = nantrap::Enter();
        spawn(armedOut, point, &zero);
        nantrap::Leave(saved);
    }
    CHECK(_mm_getcsr() == game, "MXCSR restored after the spawn skip (%08X)", _mm_getcsr());
    _mm_setcsr(before0);
    uint32_t a, b;
    std::memcpy(&a, &armedOut[0], 4);
    std::memcpy(&b, &stockOut[0], 4);
    CHECK(a == b, "same NaN as stock (%08X vs %08X)", a, b);
    CHECK(nantrap::Reports() == 0, "no report for the spawn 0*inf (got %u)", nantrap::Reports());
    CHECK(nantrap::SpawnSkips() >= 1, "counted as a spawn skip (%u)", nantrap::SpawnSkips());
    CHECK(nantrap::Armed(), "still armed");

    // Overflow: FLT_MAX * 4 traps once, is reported as overflow and resumes with +inf.
    volatile float big = FLT_MAX, four = 4.0f;
    _mm_setcsr(game);
    float r;
    unsigned after;
    {
        // As the sim-step hook does it: restore, look, then write the report (the report's
        // float formatting sets sticky flags of its own; the game's step never sees them).
        const uint32_t saved = nantrap::Enter();
        r = Overflow(&big, &four);
        nantrap::Restore(saved);
        after = _mm_getcsr();
        nantrap::Flush();
    }
    _mm_setcsr(before0);
    CHECK(std::isinf(r) && r > 0, "resumed with +inf (got %g)", r);
    CHECK(after == game, "MXCSR restored exactly (%08X)", after);
    CHECK(nantrap::Reports() == 1, "one report (got %u)", nantrap::Reports());
    const nantrap::Record& rec = nantrap::Last();
    CHECK(rec.code == nantrap::kStatusOverflow || rec.code == nantrap::kStatusMultiple, "code %08X", rec.code);
    CHECK(g_log.find("[nantrap] FIRST overflow FP op #1") != std::string::npos, "logged as overflow:\n%s",
          g_log.c_str());

    // Without nantrapov the same multiply runs unarmed for overflow: no trap, stock inf.
    CHECK((nantrap::ArmedMxcsr(game) & nantrap::kOM) != 0, "overflow stays masked by default");

    if (g_failures) {
        std::printf("%d failure(s)\nlog:\n%s", g_failures, g_log.c_str());
        return 1;
    }
    std::printf("nanwatch_harness: all passed (%u spawn skip(s), overflow at %s)\n", nantrap::SpawnSkips(),
                rec.site);
    return 0;
}
