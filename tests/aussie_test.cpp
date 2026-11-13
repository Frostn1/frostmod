// Invariants over the Aussie-mode patch in aussie.h.
//
// The patch rewrites three instructions in the remote-body stepper and adds two small code caves.
// A wrong byte here is a wild jump inside physics, every 2 ms, for every other rider. So every
// jump and load the plan writes is decoded back and checked against where it must land.
//
// Pure constants and arithmetic, so it builds and runs anywhere.

#include "../src/aussie.h"
#include "../src/antifreeze.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>

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

using namespace aussie;

static constexpr uintptr_t kBase = 0x140000000;

// Where a rel32 operand at `at` (operand bytes start at `at`, instruction ends at `next`) points.
static uintptr_t Target(uintptr_t next, const uint8_t* operand) {
    return (uintptr_t)((int64_t)next + (int64_t)Get32(operand));
}

int main() {
    // --- the original bytes are the instructions we think they are ------------------------
    CHECK(kKBytes[0] == 0xf3 && kKBytes[1] == 0x0f && kKBytes[2] == 0x59 && kKBytes[3] == 0x35,
          "site A is mulss xmm6, [rip+disp32]");
    CHECK(Get32(kKBytes + kKDispOffset) == kKOrigDisp, "site A displacement");
    CHECK(kRvaKOrigConst == 0x353E50, "site A reads the pooled 20.0f at 0x353E50, got 0x%llx",
          (unsigned long long)kRvaKOrigConst);
    CHECK(kYLen == 9 && kYBytes[3] == 0x8c && kYBytes[4] == 0x24 && kYBytes[5] == 0xbc,
          "site B is mulss xmm1, [rsp+0xbc]");
    CHECK(kYdLen == 5 && kYdBytes[1] == 0x41 && kYdBytes[4] == 0xc2,
          "site C is mulss xmm0, xmm10");
    CHECK(kRvaY > kRvaK + kKLen && kRvaYd > kRvaY + kYLen, "sites are in order and do not overlap");
    // Not the antifreeze site.
    CHECK(antifreeze::kRvaSite > kRvaYd + kYdLen, "no overlap with antifreeze");

    // --- the numbers reproduce Beta 20 ---------------------------------------------------
    CHECK(kAussieK == 10.0f * kStockK, "Beta 20 was ten times stiffer horizontally");
    CHECK(std::fabs(kAussieK * kYSpring - kStockK) < 1e-4f,
          "vertical stays at 20 x mass, as in both Beta 20 and 21e");
    CHECK(std::fabs(kYDamper * kYDamper - kYSpring) < 1e-6f,
          "the vertical damper is critical for the vertical spring: sqrt of its scale");

    // --- the plan ------------------------------------------------------------------------
    const uintptr_t page = kBase + 0x7FFF0000;     // inside rel32 reach of every site
    Plan p;
    CHECK(MakePlan(kBase, page, &p), "a reachable page must plan");

    float f = 0;
    std::memcpy(&f, p.page + kOffK, 4);  CHECK(f == kAussieK, "k cell");
    std::memcpy(&f, p.page + kOffYS, 4); CHECK(f == kYSpring, "y spring cell");
    std::memcpy(&f, p.page + kOffYD, 4); CHECK(f == kYDamper, "y damper cell");

    // site A: same opcode, displacement now names our k cell
    CHECK(std::memcmp(p.k, kKBytes, kKDispOffset) == 0, "site A keeps its opcode");
    CHECK(Target(kBase + kRvaK + kKLen, p.k + kKDispOffset) == page + kOffK, "site A reads our k");

    // site B: jmp to cave Y, padded with nops to the end of the old instruction
    CHECK(p.y[0] == 0xe9, "site B is a jmp");
    CHECK(Target(kBase + kRvaY + 5, p.y + 1) == page + kOffCaveY, "site B jumps to cave Y");
    for (size_t i = 5; i < kYLen; ++i) CHECK(p.y[i] == 0x90, "site B byte %zu is a nop", i);

    // cave Y: the original multiply, our extra one, and back to right after site B
    const uint8_t* cy = p.page + kOffCaveY;
    CHECK(std::memcmp(cy, kYBytes, kYLen) == 0, "cave Y starts with the original multiply");
    CHECK(cy[9] == 0xf3 && cy[10] == 0x0f && cy[11] == 0x59 && cy[12] == 0x0d,
          "cave Y then does mulss xmm1, [rip+d]");
    CHECK(Target(page + kOffCaveY + 17, cy + 13) == page + kOffYS, "cave Y reads the y spring scale");
    CHECK(cy[17] == 0xe9 && Target(page + kOffCaveY + 22, cy + 18) == kBase + kRvaY + kYLen,
          "cave Y returns to the instruction after site B");

    // site C: jmp to cave Yd
    CHECK(p.yd[0] == 0xe9 && Target(kBase + kRvaYd + 5, p.yd + 1) == page + kOffCaveYd,
          "site C jumps to cave Yd");
    const uint8_t* cd = p.page + kOffCaveYd;
    CHECK(std::memcmp(cd, kYdBytes, kYdLen) == 0, "cave Yd starts with the original multiply");
    CHECK(cd[5] == 0xf3 && cd[6] == 0x0f && cd[7] == 0x59 && cd[8] == 0x05,
          "cave Yd then does mulss xmm0, [rip+d]");
    CHECK(Target(page + kOffCaveYd + 13, cd + 9) == page + kOffYD, "cave Yd reads the y damper scale");
    CHECK(cd[13] == 0xe9 && Target(page + kOffCaveYd + 18, cd + 14) == kBase + kRvaYd + kYdLen,
          "cave Yd returns to the instruction after site C");

    // the two caves and the cells do not overlap
    CHECK(kOffCaveY >= kOffYD + 4 && kOffCaveY + 22 <= kOffCaveYd && kOffCaveYd + 18 <= kPageUsed,
          "page layout");

    // --- refusals ------------------------------------------------------------------------
    CHECK(!MakePlan(kBase, kBase + 0xC0000000ull, &p), "a page out of rel32 reach is refused");
    CHECK(!MakePlan(kBase, page + 4, &p), "an unaligned page is refused");
    CHECK(!MakePlan(0, page, &p), "no base is refused");
    CHECK(!MakePlan(kBase, page, nullptr), "no output is refused");
    uint8_t bad[kKLen];
    std::memcpy(bad, kKBytes, kKLen); bad[2] ^= 0xff;
    CHECK(!Matches(bad, kKBytes, kKLen), "a changed build is not matched");
    CHECK(Matches(kKBytes, kKBytes, kKLen), "the stock build is matched");

    if (g_failures == 0) std::printf("aussie_test: all checks passed\n");
    else                 std::printf("aussie_test: %d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
