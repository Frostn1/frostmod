// Invariants over lookupguard.h: the clamp table, the three stubs, and (on Windows) the clamp
// and animation stubs run for real on an executable page against fake game state.

#include "../src/lookupguard.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

using namespace lookupguard;

/// The reg field of `mov r32, [rsp+d]` / `movsxd r64, [rsp+d]` (with its REX), or -1.
static int LoadReg(const uint8_t* b, size_t n) {
    size_t i = 0;
    uint8_t rex = 0;
    if (n && (b[0] & 0xF0) == 0x40) rex = b[i++];
    if (i + 3 > n) return -1;
    const uint8_t op = b[i];
    if (op != 0x8B && op != 0x63) return -1;
    const uint8_t modrm = b[i + 1], sib = b[i + 2];
    if ((modrm & 7) != 4 || sib != 0x24) return -1;        // [rsp + disp]
    if ((modrm >> 6) != 1 && (modrm >> 6) != 2) return -1;  // disp8 / disp32
    return ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0);
}

static void TestTable() {
    CHECK(kClampCount == 45, "45 clamp sites (%zu)", kClampCount);
    uint32_t prev = 0;
    bool sawRoc = false;
    for (size_t i = 0; i < kClampCount; ++i) {
        const ClampSite& c = kClamps[i];
        CHECK(c.rva > prev, "sorted, no duplicates at %zu", i);
        prev = c.rva;
        CHECK(c.len >= 5 && c.len <= 8, "site %X: 5..8 bytes", c.rva);
        CHECK(c.replay >= 1 && c.replay <= c.len, "site %X: replay inside", c.rva);
        CHECK(c.reg < 16 && c.reg != 4, "site %X: a real register", c.rva);
        CHECK(c.count > 0x37C000 && c.count < 0x10A4000, "site %X: count in .data", c.rva);
        if (c.rva == 0x62E66) sawRoc = true;
        if (!c.clampFirst) {
            CHECK(LoadReg(c.bytes, c.replay) == c.reg, "site %X: replay loads the index register", c.rva);
            if (c.replay < c.len) {
                // mov + `jmp short`: the stub returns to the jump's target.
                CHECK(c.len == c.replay + 2 && c.bytes[c.replay] == 0xEB, "site %X: then jmp short", c.rva);
                CHECK(c.back == c.rva + c.len + (int8_t)c.bytes[c.replay + 1], "site %X: back = jmp target", c.rva);
            } else {
                CHECK(c.back == c.rva + c.len, "site %X: back after the site", c.rva);
            }
            if (c.rva != 0x41B42)   // the bike-list entry is a consumer: no loop of its own
            CHECK(c.cmpRva && c.cmpRva < c.rva && c.rva - c.cmpRva < 0x40 && (c.cmpLen == 6 || c.cmpLen == 7),
                  "site %X: the loop's cmp just before", c.rva);
        } else {
            CHECK(c.back == c.rva + c.len, "consumer %X: back after", c.rva);
        }
    }
    CHECK(sawRoc, "the .roc track exit +0x62E66 is in the table");
    // The two consumers.
    CHECK(kClamps[1].rva == 0x2075D && kClamps[1].clampFirst && kClamps[1].reg == 9 && kClamps[1].bytes[0] == 0x4d &&
          kClamps[1].bytes[1] == 0x69 && kClamps[1].count == 0x109DECC, "trainer loader 2: clamp r9d, then imul r9, r9, 0x608");
    CHECK(kClamps[2].rva == 0x41B42 && kClamps[2].reg == 11 && LoadReg(kClamps[2].bytes, 8) == 11,
          "bike-list entry: movsxd r11, [rsp+0x838]");
}

static void TestStubs() {
    uint8_t s[0x100];
    const uintptr_t va = 0x7FF600010000ull;
    const uint64_t base = 0x7FF6A0000000ull;
    for (size_t i = 0; i < kClampCount; ++i) {
        std::memset(s, 0xCC, sizeof(s));
        const size_t n = BuildClamp(s, va, base, kClamps[i], va + 0x1000);
        CHECK(n <= kSlot && s[n] == 0xCC, "clamp stub %X fits (%zu)", kClamps[i].rva, n);
        uint64_t back = 0;
        std::memcpy(&back, s + n - 8, 8);
        CHECK(back == base + kClamps[i].back, "clamp stub %X jumps back", kClamps[i].rva);
    }
    std::memset(s, 0xCC, sizeof(s));
    size_t n = BuildAnim(s, va, base, va + 0x1000);
    CHECK(n <= kSlot && s[n] == 0xCC, "anim stub fits (%zu)", n);
    n = BuildNull1(s, va, base, va + 0x1000);
    CHECK(n <= kSlot, "null1 stub fits (%zu)", n);
    n = BuildNull2(s, va, base, va + 0x1000);
    CHECK(n <= kSlot, "null2 stub fits (%zu)", n);
    CHECK(kPageBytes == kSlots * (kSlot + 4), "page layout");
    // The animation window: the patched bytes are cvttss2si edx, xmm0; lea r9d, [rdx+1].
    const uint8_t* site = kAnimWinBytes + (kAnimSite - kAnimWin);
    CHECK(site[0] == 0xf3 && site[1] == 0x0f && site[2] == 0x2c && site[3] == 0xd0, "cvttss2si edx, xmm0");
    CHECK(site[4] == 0x44 && site[5] == 0x8d && site[6] == 0x4a && site[7] == 0x01, "lea r9d, [rdx+1]");
    CHECK(kAnimSite + kAnimLen == kAnimBack, "anim back after");
    CHECK(kNull1Site + 8 + 6 + (int32_t)(kNull1Bytes[10] | kNull1Bytes[11] << 8) == kNull1Skip, "jb lands on the skip");
    CHECK(kNull1CallRva + 5 + (int32_t)(kNull1Call[1] | kNull1Call[2] << 8 | kNull1Call[3] << 16 | (uint32_t)kNull1Call[4] << 24) == 0x15BCD0, "call 0x15BCD0");
    CHECK(kNull1Site + kNull1Len == kNull1Back && kNull2Site + kNull2Len == kNull2Back, "null back after");
    // C model of the animation stub.
    bool nan = false;
    CHECK(AnimKey(std::numeric_limits<float>::quiet_NaN(), 10, &nan) == 0 && nan, "NaN -> key 0");
    CHECK(AnimKey(0.5f, 11, &nan) == 5 && !nan, "t 0.5 of 11 keys -> 5");
    CHECK(AnimKey(-0.3f, 11, &nan) == 0, "negative -> 0");
    CHECK(AnimKey(1.5f, 11, &nan) == 10, "past the end -> last");
}

#ifdef _WIN32
static uint8_t* ExecPage() {
    return (uint8_t*)VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

static void TestClampLive() {
    uint8_t* mem = ExecPage();
    CHECK(mem != nullptr, "VirtualAlloc");
    if (!mem) return;
    std::memset(mem, 0xCC, 0x2000);
    // thunk(int index): push rdi; sub rsp, 0x20; mov [rsp+8], ecx; -> stub; back: mov eax, edi; ...
    uint8_t* t = mem;
    size_t o = 0;
    auto b = [&](uint8_t x) { t[o++] = x; };
    uint8_t* stub = mem + 0x800;
    b(0x57); b(0x48); b(0x83); b(0xec); b(0x20);         // push rdi; sub rsp, 0x20
    b(0x89); b(0x4c); b(0x24); b(0x08);                  // mov [rsp+8], ecx
    b(0xbf); b(0x11); b(0x11); b(0x00); b(0x00);         // mov edi, 0x1111 (the "game" value before)
    b(0x48); b(0xb8);                                    // mov rax, stub
    for (int i = 0; i < 8; ++i) b((uint8_t)((uint64_t)(uintptr_t)stub >> (8 * i)));
    b(0xff); b(0xe0);                                    // jmp rax
    const size_t backAt = o;
    b(0x89); b(0xf8);                                    // mov eax, edi
    b(0x48); b(0x83); b(0xc4); b(0x20); b(0x5f); b(0xc3);   // add rsp, 0x20; pop rdi; ret
    const uint64_t base = (uint64_t)(uintptr_t)mem;
    int32_t* count = (int32_t*)(mem + 0x1000);
    uint32_t* hits = (uint32_t*)(mem + 0x1004);
    *count = 6; *hits = 0;
    ClampSite c{0x0, 7, {0x8b, 0x7c, 0x24, 0x08, 0x90, 0x90, 0x90}, 4, 7, 0x1000, (uint32_t)backAt, 0, 0x608, 0, 0};
    BuildClamp(stub, (uintptr_t)stub, base, c, base + 0x1004);
    FlushInstructionCache(GetCurrentProcess(), mem, 0x2000);
    using Thunk = int32_t (*)(int32_t);
    const Thunk run = (Thunk)(void*)t;
    CHECK(run(0) == 0 && run(5) == 5 && *hits == 0, "in range: untouched, not counted");
    CHECK(run(6) == 0 && *hits == 1, "count itself: clamped to 0, counted");
    CHECK(run(-2147483647 - 1) == 0 && *hits == 2, "INT_MIN: clamped");
    CHECK(run(0x7A3B1C) == 0 && *hits == 3, "junk: clamped");
    VirtualFree(mem, 0, MEM_RELEASE);
}

static void TestAnimLive() {
    uint8_t* mem = ExecPage();
    CHECK(mem != nullptr, "VirtualAlloc");
    if (!mem) return;
    std::memset(mem, 0xCC, 0x2000);
    // thunk(float x /*xmm0*/, float t /*xmm1*/, obj /*r8*/, keys /*r9d*/) -> edx after the stub
    uint8_t* t = mem;
    size_t o = 0;
    auto b = [&](uint8_t x) { t[o++] = x; };
    uint8_t* stub = mem + 0x800;
    b(0x55);                                             // push rbp
    b(0x48); b(0x83); b(0xec); b(0x30);                  // sub rsp, 0x30
    b(0x0f); b(0x29); b(0x74); b(0x24); b(0x20);         // movaps [rsp+0x20], xmm6
    b(0x4c); b(0x89); b(0xc5);                           // mov rbp, r8
    b(0x44); b(0x89); b(0xc9);                           // mov ecx, r9d
    b(0x0f); b(0x28); b(0xf1);                           // movaps xmm6, xmm1
    b(0x48); b(0xb8);
    for (int i = 0; i < 8; ++i) b((uint8_t)((uint64_t)(uintptr_t)stub >> (8 * i)));
    b(0xff); b(0xe0);                                    // jmp rax
    const size_t backAt = o;
    b(0x44); b(0x3b); b(0xc9);                           // cmp r9d, ecx (the game's next instruction)
    b(0x89); b(0xd0);                                    // mov eax, edx
    b(0x0f); b(0x28); b(0x74); b(0x24); b(0x20);         // movaps xmm6, [rsp+0x20]
    b(0x48); b(0x83); b(0xc4); b(0x30); b(0x5d); b(0xc3);   // add rsp, 0x30; pop rbp; ret
    const uint64_t base = (uint64_t)(uintptr_t)(mem + backAt) - kAnimBack;
    uint32_t* hits = (uint32_t*)(mem + 0x1000);
    *hits = 0;
    BuildAnim(stub, (uintptr_t)stub, base, (uint64_t)(uintptr_t)hits);
    FlushInstructionCache(GetCurrentProcess(), mem, 0x2000);
    using Thunk = int32_t (*)(float, float, void*, int32_t);
    const Thunk run = (Thunk)(void*)t;
    alignas(16) static uint8_t obj[0x100];
    std::memset(obj, 0, sizeof(obj));
    const float nanf_ = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(obj + 0x84, &nanf_, 4);
    CHECK(run(10 * 0.5f, 0.5f, obj, 11) == 5 && *hits == 0, "t 0.5: key 5");
    CHECK(run(10 * 1.5f, 1.5f, obj, 11) == 10 && *hits == 0, "t 1.5: last key");
    CHECK(run(-3.0f, -0.3f, obj, 11) == 0 && *hits == 0, "t < 0: key 0");
    CHECK(run(nanf_, nanf_, obj, 11) == 0 && *hits == 1, "NaN: key 0, counted");
    float back = 1;
    std::memcpy(&back, obj + 0x84, 4);
    CHECK(back == 0.0f, "and the phase written back as 0");
    VirtualFree(mem, 0, MEM_RELEASE);
}
#endif

int main() {
    TestTable();
    TestStubs();
#ifdef _WIN32
    TestClampLive();
    TestAnimLive();
#endif
    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("lookupguard: all checks passed\n");
    return 0;
}
