// Invariants over the Aussie-mode patch in aussie.h.
//
// Same reasoning as antifreeze_test.cpp: a wrong displacement here is a wild eight-byte read
// inside the remote-rider extrapolation, which runs for every rider on every net step. So the
// arithmetic is proved against the instruction's real encoding rather than trusted.
//
// Pure constants and arithmetic, so it builds and runs anywhere.

#include "../src/aussie.h"
#include "../src/antifreeze.h"

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

// The image the offsets were read from, so a VA can be written the way the notes write it.
static constexpr uintptr_t kBase = 0x140000000;

int main() {
    // --- the encoding we claim -------------------------------------------------------
    // Eight bytes of `movsd xmm3, [rip+disp32]`, displacement in the last four.
    CHECK(kSiteLen == 8, "instruction length");
    CHECK(kDispOffset + 4 == kSiteLen, "displacement is the last four bytes");
    CHECK(kSiteBytes[0] == 0xf2 && kSiteBytes[1] == 0x0f && kSiteBytes[2] == 0x10 &&
          kSiteBytes[3] == 0x1d, "opcode is movsd xmm3, [rip+disp32]");

    int32_t fromBytes = 0;
    for (int i = 0; i < 4; ++i)
        fromBytes |= (int32_t)((uint32_t)kSiteBytes[kDispOffset + i] << (8 * i));
    CHECK(fromBytes == kOrigDisp, "encoded displacement 0x%08x != kOrigDisp 0x%08x",
          (unsigned)fromBytes, (unsigned)kOrigDisp);

    // It lands on the pooled 0.5, which must NOT be overwritten in place.
    CHECK(kRvaOrigConst == 0x353870, "original target RVA 0x%llx", (unsigned long long)kRvaOrigConst);
    CHECK(CurrentTarget(kBase + kRvaSite, kSiteBytes) == kBase + kRvaOrigConst,
          "CurrentTarget should decode the stock site back to the stock constant");
    CHECK(CurrentTarget(kBase + kRvaSite, nullptr) == 0, "null site decodes to 0");

    // --- it is not the antifreeze site, and the two cannot overlap ----------------------
    CHECK(kRvaSite + kSiteLen <= antifreeze::kRvaSite || antifreeze::kRvaSite +
          antifreeze::kSiteLen <= kRvaSite, "the two patches must not share a byte");

    // --- SiteMatches is a real guard --------------------------------------------------
    CHECK(SiteMatches(kSiteBytes), "the stock bytes must match");
    CHECK(!SiteMatches(nullptr), "null must not match");
    for (size_t i = 0; i < kSiteLen; ++i) {
        uint8_t bad[kSiteLen];
        std::memcpy(bad, kSiteBytes, kSiteLen);
        bad[i] ^= 0xff;
        CHECK(!SiteMatches(bad), "a build that differs in byte %zu must be refused", i);
    }

    // --- PlanDisp round-trips ---------------------------------------------------------
    int32_t disp = 0;
    CHECK(PlanDisp(kBase + kRvaSite, kBase + kRvaOrigConst, &disp),
          "planning the stock target must succeed");
    CHECK(disp == kOrigDisp, "round-trip gave 0x%08x, want 0x%08x", (unsigned)disp, (unsigned)kOrigDisp);

    const uintptr_t ours = kBase + 0x900000;
    CHECK(PlanDisp(kBase + kRvaSite, ours, &disp), "planning our own page must succeed");
    {
        uint8_t patched[kSiteLen];
        std::memcpy(patched, kSiteBytes, kSiteLen);
        for (int i = 0; i < 4; ++i)
            patched[kDispOffset + i] = (uint8_t)((uint32_t)disp >> (8 * i));
        CHECK(CurrentTarget(kBase + kRvaSite, patched) == ours,
              "a patched site must decode back to the page we chose");
        CHECK(std::memcmp(patched, kSiteBytes, kDispOffset) == 0,
              "patching must touch only the displacement");
    }

    CHECK(!PlanDisp(kBase + kRvaSite, kBase + 0x900001, &disp), "unaligned target refused");
    CHECK(!PlanDisp(0, kBase + 0x900000, &disp), "null site refused");
    CHECK(!PlanDisp(kBase + kRvaSite, 0, &disp), "null target refused");
    CHECK(!PlanDisp(kBase + kRvaSite, kBase + 0x900000, nullptr), "null out refused");
    CHECK(!PlanDisp(kBase + kRvaSite, kBase + 0xC0000000ull, &disp),
          "a target out of rel32 reach must be refused, not wrapped");

    // --- the factor ------------------------------------------------------------------
    CHECK(kStockAccelFactor == 0.5, "stock is the textbook 1/2");
    CHECK(kAussieAccelFactor == 0.0, "Aussie mode drops the term");

    if (g_failures == 0) std::printf("aussie_test: all checks passed\n");
    else                 std::printf("aussie_test: %d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
