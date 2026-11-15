// Invariants over the rear-shock NaN guard in shockguard.h.
//
// The call we repoint, the function it reaches, the curve and bike offsets in it, and the stub
// we put in front of it are proved against the bytes the install insists on, rather than
// restated. A wrong byte in the stub runs on every physics step.
//
// On Windows the stub is also run for real, in front of a copy of the game's own function
// bytes (with its two float constants placed where its rip-relative loads look for them),
// against a fake bike: a NaN must come back as curve[0] and be counted, a finite x must give
// exactly what the unguarded function gives, and the unguarded function must fault on the NaN
// the way OB's game did. Everything else is pure and runs anywhere.

#include "../src/shockguard.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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

using namespace shockguard;

static constexpr uintptr_t kBase = 0x140000000;

static int32_t Disp(const uint8_t* p) {
    int32_t d = 0;
    for (int i = 0; i < 4; ++i) d |= (int32_t)((uint32_t)p[i] << (8 * i));
    return d;
}
static uint64_t Imm64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
// The rip-relative target of an instruction `len` bytes long at `rva`, disp32 at byte `at`.
static uintptr_t RipTarget(const uint8_t* insn, uintptr_t rva, size_t at, size_t len) {
    return (uintptr_t)((int64_t)(rva + len) + Disp(insn + at));
}

// Where 0x1A2970's two constants live (comiss x, [1.0f]; mulss x, [99.0f]).
static constexpr size_t kOneAt = 0x14, kNinetyNineAt = 0x1D;
static constexpr uintptr_t kRvaOne = 0x353808, kRvaNinetyNine = 0x329370;

static void TestSite() {
    CHECK(kCallInWin == 31 && kCallInWin + kCallLen == kWinLen, "the call is the window's last 5 bytes");
    CHECK(CallTarget(kBase + kRvaCall, kWin + kCallInWin) == kBase + kRvaCurveFn,
          "the call at +0x1A2E7F must reach 0x1A2970");
    // The arguments: rcx = rbx (the bike), xmm1 = (float)(1.0 - length / stroke).
    CHECK(kWin[0] == 0xf2 && kWin[1] == 0x45 && kWin[3] == 0x5e && kWin[4] == 0xe9, "divsd xmm13, xmm9");
    CHECK(kWin[14] == 0x48 && kWin[15] == 0x8b && kWin[16] == 0xcb, "mov rcx, rbx");
    CHECK(kWin[27] == 0xf2 && kWin[29] == 0x5a && kWin[30] == 0xc8, "cvtsd2ss xmm1, xmm0");
    // The two loads the length and stroke come from.
    CHECK(Disp(kLenLoad + 5) == (int32_t)kBikeShockLen && kLenLoad[4] == 0xab, "xmm13 = [rbx+0x5CC8]");
    CHECK(Disp(kStrokeLoad + 5) == (int32_t)kBikeStroke && kStrokeLoad[4] == 0x8b, "xmm9 = [rbx+0x5180]");
    // A call rel32 we would write must decode back to the stub.
    int32_t rel = 0;
    const uintptr_t stub = kBase + kRvaCall - 0x10000000;
    CHECK(CallRel(kBase + kRvaCall, stub, &rel), "stub in reach");
    uint8_t call[kCallLen] = {0xE8};
    std::memcpy(call + 1, &rel, 4);
    CHECK(CallTarget(kBase + kRvaCall, call) == stub, "rewritten call decodes to the stub");
    CHECK(!CallRel(kBase + kRvaCall, kBase + kRvaCall + 0x100000000ull, &rel), "out of reach refused");
    // The rel32 alone is rewritten, with one aligned store, in this build.
    CHECK(((kRvaCall + 1) & 3) == 0, "the call's rel32 is 4-byte aligned (atomic store)");
}

static void TestFunction() {
    // x <= 0 -> curve[0]: xorps xmm0; movaps xmm3, xmm1; comiss xmm0, xmm3; jb +9;
    // movss xmm0, [rcx+0x4FE8]; ret
    CHECK(kFnBytes[3] == 0x0f && kFnBytes[4] == 0x28 && kFnBytes[5] == 0xd9, "x arrives in xmm1");
    CHECK(kFnBytes[6] == 0x0f && kFnBytes[7] == 0x2f && kFnBytes[8] == 0xc3 && kFnBytes[9] == 0x72,
          "comiss 0, x ; jb");
    CHECK(Disp(kFnBytes + 15) == (int32_t)kBikeCurve && kFnBytes[19] == 0xc3, "x <= 0 returns [rcx+0x4FE8]");
    // x >= 1 -> curve[99]
    CHECK(kFnBytes[kOneAt] == 0x0f && kFnBytes[kOneAt + 1] == 0x2f && kFnBytes[kOneAt + 7] == 0x73,
          "comiss x, [1.0f] ; jae");
    CHECK(RipTarget(kFnBytes + kOneAt, kRvaCurveFn + kOneAt, 3, 7) == kRvaOne, "the 1.0f constant");
    CHECK(RipTarget(kFnBytes + kNinetyNineAt, kRvaCurveFn + kNinetyNineAt, 4, 8) == kRvaNinetyNine,
          "the 99.0f constant");
    CHECK(Disp(kFnBytes + 50) == (int32_t)kBikeCurveLast && kFnBytes[54] == 0xc3, "x >= 1 returns [rcx+0x5174]");
    CHECK(kBikeCurveLast == kBikeCurve + 4 * (kCurveEntries - 1), "0x5174 is curve[99]");
    // i = cvttss2si(x * 99); the read that faults is curve[i] at 0x1A29AE.
    CHECK(kFnBytes[37] == 0xf3 && kFnBytes[38] == 0x0f && kFnBytes[39] == 0x2c && kFnBytes[40] == 0xd3,
          "cvttss2si edx, xmm3");
    const size_t fault = kRvaFault - kRvaCurveFn;
    CHECK(fault == 62, "0x1A29AE is byte 62");
    CHECK(kFnBytes[fault] == 0xf3 && kFnBytes[fault + 2] == 0x10 && kFnBytes[fault + 3] == 0x94 &&
              Disp(kFnBytes + fault + 5) == (int32_t)kBikeCurve,
          "0x1A29AE: movss xmm2, [rcx + rax*4 + 0x4FE8]");
    CHECK(kFnBytes[kFnLen - 1] == 0xc3, "ends on its last ret");
    // It writes xmm0-3, eax/rax and edx and nothing else, which is what the caller trusts and
    // what the stub may clobber: every ModRM `reg` it writes is 0-3, no REX.R anywhere.
    for (size_t i = 0; i < kFnLen; ++i)
        CHECK(!(kFnBytes[i] == 0x44 || kFnBytes[i] == 0x4C), "no REX.R (xmm8+/r8+) at byte %zu", i);
}

static void TestStub() {
    uint8_t s[kPageBytes];
    std::memset(s, 0xCC, sizeof(s));
    const uintptr_t at = 0x7FF612340000ull;
    const uint64_t fn = kBase + kRvaCurveFn;
    const size_t n = BuildStub(s, at, fn);
    CHECK(n == kStubLen && n <= kCountOff, "stub is %zu bytes and stops before the counter", n);
    CHECK(kCountOff % 4 == 0 && kBikeOff % 8 == 0 && kBikeOff + 8 <= kPageBytes, "cells aligned, in the page");
    // ucomiss xmm1, xmm1 ; jnp game
    CHECK(s[0] == 0x0F && s[1] == 0x2E && s[2] == 0xC9, "ucomiss xmm1, xmm1 (quiet: no #IA on a QNaN)");
    CHECK(s[kJnpAt] == 0x7B && kJnpAt + 2 + s[kJnpAt + 1] == kGameAt, "jnp lands on the jump to the game");
    // xorps xmm1, xmm1
    CHECK(s[5] == 0x0F && s[6] == 0x57 && s[7] == 0xC9, "xorps xmm1, xmm1");
    // lock inc dword [count]
    CHECK(s[kIncAt] == 0xF0 && s[kIncAt + 1] == 0xFF && s[kIncAt + 2] == 0x05, "lock inc dword [rip+d]");
    CHECK(at + kIncEnd + Disp(s + kIncAt + 3) == at + kCountOff, "inc hits the counter");
    // mov [bike], rcx
    CHECK(s[kStoreAt] == 0x48 && s[kStoreAt + 1] == 0x89 && s[kStoreAt + 2] == 0x0D, "mov [rip+d], rcx");
    CHECK(at + kStoreEnd + Disp(s + kStoreAt + 3) == at + kBikeOff, "store hits the bike cell");
    // mov rax, fn ; jmp rax
    CHECK(s[kGameAt] == 0x48 && s[kGameAt + 1] == 0xB8 && Imm64(s + kGameAt + 2) == fn, "mov rax, 0x1A2970");
    CHECK(s[kGameAt + 10] == 0xFF && s[kGameAt + 11] == 0xE0 && kGameAt + 12 == kStubLen, "jmp rax");
    // Registers it writes: xmm1 (ucomiss/xorps reg field 1), rax, and memory of its own.
    // Nothing in xmm4/xmm5, which the caller keeps live across the call.
    CHECK(s[2] == 0xC9 && s[7] == 0xC9, "ModRM C9: xmm1 only");
}

static void TestModel() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(IndexFor(nan, false) == INT32_MIN, "unguarded NaN falls through both checks to INT_MIN");
    CHECK(IndexFor(nan, true) == -1, "guarded NaN takes the x <= 0 branch");
    CHECK(IndexFor(inf, false) == 100 && IndexFor(-inf, false) == -1, "infinities take a branch on their own");
    CHECK(IndexFor(inf, true) == 100 && IndexFor(-inf, true) == -1, "and the guard leaves them alone");
    for (float x : {-1.0f, 0.0f, 0.001f, 0.25f, 0.5f, 0.999f, 1.0f, 2.0f})
        CHECK(IndexFor(x, true) == IndexFor(x, false), "finite x %g is untouched", (double)x);
    CHECK(IndexFor(0.5f, false) == 49, "0.5 reads curve[49]");

    // OB's report, 2026-10-28: AV reading 0xFFFFFFFFD5CC5068 at +0x1A29AE. With i = INT_MIN
    // that is bike + 0x4FE8 - 2^33, for a bike at 0x1D5CC0080.
    const uint64_t bike = 0x1D5CC0080ull;
    const uint64_t read = bike + kBikeCurve + (uint64_t)((int64_t)INT32_MIN * 4);
    CHECK(read == 0xFFFFFFFFD5CC5068ull, "OB's fault address is curve[INT_MIN] (got %llx)",
          (unsigned long long)read);
}

static void TestRates() {
    int lines = 0;
    for (uint64_t n = 1; n <= 30000; ++n) lines += ShouldLog(n) ? 1 : 0;   // a minute of 2 ms steps
    CHECK(lines <= 20, "a minute of fires logs %d lines", lines);
    CHECK(ShouldLog(1) && ShouldLog(2) && ShouldLog(3) && ShouldLog(4) && !ShouldLog(5), "first three, then 2^k");
    CHECK(!ShouldLog(0), "nothing to log at zero");
}

#ifdef _WIN32
// ---- the stub, run for real in front of the game's own bytes --------------------------
using CurveFn = float (*)(const void* bike, float x);

static float CallSafe(CurveFn f, const void* bike, float x, bool* faulted) {
    *faulted = false;
    __try {
        return f(bike, x);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        *faulted = true;
        return 0.0f;
    }
}

static void TestLive() {
    // One region holding the function at its offset 0 and its two constants where its
    // rip-relative loads land, so the bytes run unmodified.
    const size_t oneOff = kRvaOne - kRvaCurveFn, nnOff = kRvaNinetyNine - kRvaCurveFn;
    const size_t span = ((oneOff > nnOff ? oneOff : nnOff) + 0x1000) & ~(size_t)0xFFF;
    uint8_t* mem = (uint8_t*)VirtualAlloc(nullptr, span + 0x1000, MEM_RESERVE | MEM_COMMIT,
                                          PAGE_EXECUTE_READWRITE);
    CHECK(mem != nullptr, "VirtualAlloc for the function copy");
    if (!mem) return;
    std::memcpy(mem, kFnBytes, kFnLen);
    const float one = 1.0f, nn = 99.0f;
    std::memcpy(mem + oneOff, &one, 4);
    std::memcpy(mem + nnOff, &nn, 4);
    uint8_t* stub = mem + span;   // its own page, as AllocNear gives the DLL
    std::memset(stub, 0xCC, kPageBytes);
    std::memset(stub + kCountOff, 0, kPageBytes - kCountOff);
    BuildStub(stub, (uintptr_t)stub, (uint64_t)(uintptr_t)mem);
    FlushInstructionCache(GetCurrentProcess(), mem, span + 0x1000);

    // A bike with a recognisable curve: curve[i] = 1000 + i.
    static uint8_t bike[0x6000];
    for (int i = 0; i < kCurveEntries; ++i) {
        const float v = 1000.0f + (float)i;
        std::memcpy(bike + kBikeCurve + 4 * i, &v, 4);
    }
    const CurveFn game = (CurveFn)(void*)mem, guarded = (CurveFn)(void*)stub;
    const volatile uint32_t* count = (const volatile uint32_t*)(stub + kCountOff);
    bool faulted = false;

    for (float x : {-0.5f, 0.0f, 0.3f, 0.5f, 0.75f, 0.99f, 1.0f, 3.0f}) {
        const float a = CallSafe(game, bike, x, &faulted);
        const float b = CallSafe(guarded, bike, x, &faulted);
        CHECK(!faulted && a == b, "finite x %g: game %g, guarded %g", (double)x, (double)a, (double)b);
    }
    CHECK(CallSafe(game, bike, 0.5f, &faulted) == 1049.5f, "x 0.5 is 49.5 along: halfway from curve[49] to curve[50]");
    CHECK(*count == 0, "no fire on finite input");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float r = CallSafe(guarded, bike, nan, &faulted);
    CHECK(!faulted && r == 1000.0f, "guarded NaN returns curve[0] (got %g, faulted %d)", (double)r, faulted);
    CHECK(*count == 1, "the fire is counted");
    CHECK(*(void* volatile*)(stub + kBikeOff) == (void*)bike, "and the bike recorded");
    CallSafe(guarded, bike, -nan, &faulted);
    CHECK(!faulted && *count == 2, "a negative NaN too");

    // And the crash, as shipped: the same NaN into the game's own bytes reads 8 GB below the
    // curve. Only checked when that address is not mapped, which it should never be.
    MEMORY_BASIC_INFORMATION mbi{};
    const uintptr_t target = (uintptr_t)bike + kBikeCurve + (uintptr_t)((int64_t)INT32_MIN * 4);
    if (!VirtualQuery((const void*)target, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
        CallSafe(game, bike, nan, &faulted);
        CHECK(faulted, "unguarded NaN faults, as at +0x1A29AE");
    }
    VirtualFree(mem, 0, MEM_RELEASE);
}
#endif

int main() {
    TestSite();
    TestFunction();
    TestStub();
    TestModel();
    TestRates();
#ifdef _WIN32
    TestLive();
#endif
    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("shockguard: all checks passed\n");
    return 0;
}
