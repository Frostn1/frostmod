// Invariants over the rider-visibility patch in antifreeze.h.
//
// This is the only part of the patch that can be checked without a running game, and it is
// the part worth checking: a wrong displacement here is not a crash at load, it is a wild
// eight-byte read inside the remote-rider playback path, which runs for every rider on every
// frame. So the arithmetic is proved against the instruction's real encoding rather than
// trusted.
//
// Pure constants and arithmetic, so like offsets_test.cpp it builds and runs anywhere.

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

using namespace antifreeze;

// The image the offsets were read from, so a VA can be written the way the notes write it.
static constexpr uintptr_t kBase = 0x140000000;

int main() {
    // --- the encoding we claim -------------------------------------------------------
    // Nine bytes of `comisd xmm8, [rip+disp32]`, displacement in the last four.
    CHECK(kSiteLen == 9, "instruction length");
    CHECK(kDispOffset + 4 == kSiteLen, "displacement is the last four bytes");
    CHECK(kSiteBytes[0] == 0x66 && kSiteBytes[1] == 0x44 && kSiteBytes[2] == 0x0f &&
          kSiteBytes[3] == 0x2f && kSiteBytes[4] == 0x05,
          "opcode is comisd xmm8, [rip+disp32]");

    // The displacement stored in those bytes is the one we say it is, little-endian.
    int32_t fromBytes = 0;
    for (int i = 0; i < 4; ++i)
        fromBytes |= (int32_t)((uint32_t)kSiteBytes[kDispOffset + i] << (8 * i));
    CHECK(fromBytes == kOrigDisp, "encoded displacement 0x%08x != kOrigDisp 0x%08x",
          (unsigned)fromBytes, (unsigned)kOrigDisp);

    // And it lands on the shared 0.3 constant. This is the number that must NOT be
    // overwritten in place - 37 unrelated sites read the same eight bytes.
    CHECK(kRvaOrigConst == 0x353D90, "original target RVA 0x%llx", (unsigned long long)kRvaOrigConst);

    // --- CurrentTarget decodes what the bytes say ------------------------------------
    CHECK(CurrentTarget(kBase + kRvaSite, kSiteBytes) == kBase + kRvaOrigConst,
          "CurrentTarget should decode the stock site back to the stock constant");
    CHECK(CurrentTarget(kBase + kRvaSite, nullptr) == 0, "null site decodes to 0");

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
    // Point the real site at the real constant and we must recover the shipped displacement.
    int32_t disp = 0;
    CHECK(PlanDisp(kBase + kRvaSite, kBase + kRvaOrigConst, &disp),
          "planning the stock target must succeed");
    CHECK(disp == kOrigDisp, "round-trip gave 0x%08x, want 0x%08x", (unsigned)disp, (unsigned)kOrigDisp);

    // A patched displacement, decoded back through CurrentTarget, must name our page.
    const uintptr_t ours = kBase + 0x900000;   // 8-byte aligned, comfortably in range
    CHECK(PlanDisp(kBase + kRvaSite, ours, &disp), "planning our own page must succeed");
    {
        uint8_t patched[kSiteLen];
        std::memcpy(patched, kSiteBytes, kSiteLen);
        for (int i = 0; i < 4; ++i)
            patched[kDispOffset + i] = (uint8_t)((uint32_t)disp >> (8 * i));
        CHECK(CurrentTarget(kBase + kRvaSite, patched) == ours,
              "a patched site must decode back to the page we chose");
        // It still has to look like the same instruction, or a re-patch would be refused.
        CHECK(std::memcmp(patched, kSiteBytes, kDispOffset) == 0,
              "patching must touch only the displacement");
    }

    // A target below the site encodes as a negative displacement, which is normal and fine.
    CHECK(PlanDisp(kBase + kRvaSite, kBase + 0x1000, &disp) && disp < 0,
          "a target before the site must encode negative");

    // --- PlanDisp refuses what it cannot encode ---------------------------------------
    CHECK(!PlanDisp(kBase + kRvaSite, kBase + 0x900001, &disp), "unaligned target refused");
    CHECK(!PlanDisp(0, kBase + 0x900000, &disp), "null site refused");
    CHECK(!PlanDisp(kBase + kRvaSite, 0, &disp), "null target refused");
    CHECK(!PlanDisp(kBase + kRvaSite, kBase + 0x900000, nullptr), "null out refused");
    // Out of rel32 reach: a page 3 GB above the site cannot be named by this instruction.
    CHECK(!PlanDisp(kBase + kRvaSite, kBase + 0xC0000000ull, &disp),
          "a target out of rel32 reach must be refused, not wrapped");

    // --- the limit, and its bounds ----------------------------------------------------
    CHECK(kMinLimitSeconds == kStockLimitSeconds,
          "the floor must be the stock value, so this can never be worse than shipping");
    CHECK(kDefaultLimitSeconds > kStockLimitSeconds, "the default must actually widen");
    CHECK(kDefaultLimitSeconds <= kMaxLimitSeconds, "the default must be inside the range");
    CHECK(ClampLimit(0.0)  == kMinLimitSeconds, "below the floor clamps up");
    CHECK(ClampLimit(99.0) == kMaxLimitSeconds, "above the ceiling clamps down");
    CHECK(ClampLimit(1.25) == 1.25,             "a value in range is kept");
    CHECK(ClampLimit(0.0 / 0.0) == kDefaultLimitSeconds, "NaN falls back to the default");

    if (g_failures == 0) std::printf("antifreeze_test: all checks passed\n");
    else                 std::printf("antifreeze_test: %d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
