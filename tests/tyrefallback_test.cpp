// Invariants over the tyre-fallback patch in tyrefallback.h: the guarded span, the three
// sites inside it, and the bytes we write (site jump and stub). Pure, so it runs anywhere.

#include "../src/tyrefallback.h"

#include <cstdio>
#include <cstring>

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

using namespace tyrefallback;

static int32_t Rel32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)p[i] << (8 * i);
    return (int32_t)v;
}

int main() {
    // --- the span ---------------------------------------------------------------------
    CHECK(kRvaJoin == 0xE8D6B, "join is 0xE8D6B");
    CHECK(SpanMatches(kSpanBytes), "span matches itself");
    CHECK(!SpanMatches(nullptr), "null never matches");
    uint8_t bad[kSpanLen];
    std::memcpy(bad, kSpanBytes, kSpanLen);
    bad[kSpanLen - 1] ^= 1;
    CHECK(!SpanMatches(bad), "one changed byte refuses");

    // Every site is the seven-byte `mov ebp, [rsp+0xB8]`, inside the span, and the span ends
    // with the last one, so the join follows it directly.
    for (int i = 0; i < kSiteCount; ++i) {
        const uintptr_t off = kRvaSites[i] - kRvaSpan;
        CHECK(off + kSiteLen <= kSpanLen, "site %d inside span", i);
        CHECK(std::memcmp(kSpanBytes + off, kSiteBytes, kSiteLen) == 0, "site %d bytes", i);
    }
    CHECK(kRvaSites[kNoTyres] + kSiteLen == kRvaJoin, "last site falls into the join");
    // The two other sites are followed by `jmp short` to the join.
    for (int i = kCfgTyreMissing; i <= kPickedTyreMissing; ++i) {
        const uintptr_t j = kRvaSites[i] + kSiteLen - kRvaSpan;
        CHECK(kSpanBytes[j] == 0xEB, "site %d followed by jmp short", i);
        CHECK(kRvaSites[i] + kSiteLen + 2 + (int8_t)kSpanBytes[j + 1] == kRvaJoin,
              "site %d's jmp lands on the join", i);
    }
    // The loops' rip-relative operands decode to the globals we read.
    // cmp [rip+d], ebx at 0xE8CE6 (6 bytes) -> tyre count
    CHECK(kRvaSpan + 6 + Rel32(kSpanBytes + 2) == kRvaTyreCount, "count global");
    // add rcx, [rip+d] at 0xE8D02 (7 bytes) -> tyre table
    CHECK(0xE8D02 + 7 + Rel32(kSpanBytes + (0xE8D02 - kRvaSpan) + 3) == kRvaTyreTable,
          "table global");
    // lea rdx, [rip+d] at 0xE8D33 (7 bytes) -> picked tyre name
    CHECK(0xE8D33 + 7 + Rel32(kSpanBytes + (0xE8D33 - kRvaSpan) + 3) == kRvaPickedTyre,
          "picked-tyre global");
    // imul rcx, rcx, 0x608 in both loops
    CHECK(Rel32(kSpanBytes + (0xE8CFB - kRvaSpan) + 3) == (int32_t)kTyreStride, "stride");
    // lea rdx, [rsp+0x640] in the first loop
    CHECK(Rel32(kSpanBytes + (0xE8CF3 - kRvaSpan) + 4) == (int32_t)kCfgTyreSlot, "cfg slot");

    // --- the pick ---------------------------------------------------------------------
    CHECK(PickIndex(5, (int)0xE67A2C53) == 0, "junk replaced");
    CHECK(PickIndex(1, -1) == 0, "one tyre is enough");
    CHECK(PickIndex(0, 1234) == 1234, "no tyres: game's value kept");
    CHECK(PickIndex(-3, 7) == 7, "bad count: game's value kept");

    // --- the site patch ---------------------------------------------------------------
    const uintptr_t base = 0x140000000;
    uint8_t p[kSiteLen];
    CHECK(BuildSitePatch(base + 0xE8D1C, base + 0x100000, p), "near stub ok");
    CHECK(p[0] == 0xE9 && p[5] == 0x90 && p[6] == 0x90, "jmp + 2 nops");
    CHECK(base + 0xE8D1C + 5 + Rel32(p + 1) == base + 0x100000, "jmp lands on the stub");
    CHECK(BuildSitePatch(base + 0xE8D1C, base - 0x1000000, p), "stub below ok");
    CHECK(base + 0xE8D1C + 5 + Rel32(p + 1) == base - 0x1000000, "jmp lands below");
    CHECK(!BuildSitePatch(base, base + 0x100000000ull, p), "out of reach refused");

    // --- the stub ---------------------------------------------------------------------
    uint8_t s[128];
    std::memset(s, 0xCC, sizeof(s));
    const uint64_t fn = 0x7FF612345678ull, join = base + kRvaJoin;
    const size_t n = BuildStub(s, 2, fn, join);
    CHECK(n == kStubLen, "stub length %zu", n);
    CHECK(s[n] == 0xCC, "no overrun");
    CHECK(s[13] == 0xB9 && Rel32(s + 14) == 2, "mov ecx, site");
    uint64_t gotFn = 0, gotJoin = 0;
    std::memcpy(&gotFn, s + 39, 8);   // after 48 B8
    CHECK(s[37] == 0x48 && s[38] == 0xB8 && gotFn == fn, "mov rax, fn");
    std::memcpy(&gotJoin, s + n - 8, 8);
    CHECK(gotJoin == join, "jumps back to the join");
    CHECK(s[n - 14] == 0xFF && s[n - 13] == 0x25 && Rel32(s + n - 12) == 0, "jmp [rip+0]");
    // Pushes and pops balance: 7 regs + flags + rbx in, the same out.
    CHECK(s[12] == 0x53 && s[52] == 0x5B, "rbx saved and restored");
    CHECK(s[53] == 0x89 && s[54] == 0xC5, "mov ebp, eax");
    CHECK(s[11] == 0x9C && s[55] == 0x9D, "flags saved and restored");

    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("tyrefallback: all checks passed\n");
    return 0;
}
