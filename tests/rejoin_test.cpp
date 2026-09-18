// Invariants over the entity-teardown patch in rejoin.h.
//
// The thunk is 36 bytes of hand-written machine code that runs on every disconnect, so its
// encoding is proved here rather than trusted. Pure constants and arithmetic: builds and
// runs anywhere.

#include "../src/rejoin.h"

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

using namespace rejoin;

static constexpr uintptr_t kBase   = 0x140000000;
static constexpr uintptr_t kMemset = 0x1402AB340;   // the import thunk the stock call reaches

int main() {
    // --- the window we guard ----------------------------------------------------------
    CHECK(kCallOffset + kCallLen + 3 == kWindowLen, "window ends three bytes past the call");
    CHECK(kWindowBytes[kCallOffset] == 0xe8, "the byte we repoint is a call rel32");
    CHECK(kRvaCall == 0x2A565E, "call RVA 0x%llx", (unsigned long long)kRvaCall);

    // The window really is the memset of a 0x2E0 roster record: size in r8d, stride in the
    // imul. If either stops being 0x2E0 this is not the handler we think it is.
    CHECK(kWindowBytes[13] == 0xe0 && kWindowBytes[14] == 0x02, "imul stride is 0x2e0");
    CHECK(kWindowBytes[21] == 0xe0 && kWindowBytes[22] == 0x02, "memset length is 0x2e0");

    // --- the stock call goes where we say ---------------------------------------------
    CHECK(CurrentTarget(kBase + kRvaCall, kWindowBytes + kCallOffset) == kMemset,
          "stock call should reach memset, got 0x%llx",
          (unsigned long long)CurrentTarget(kBase + kRvaCall, kWindowBytes + kCallOffset));
    CHECK(CurrentTarget(kBase + kRvaCall, nullptr) == 0, "null call decodes to 0");

    // --- WindowMatches is a real guard -------------------------------------------------
    CHECK(WindowMatches(kWindowBytes), "the stock window matches itself");
    CHECK(!WindowMatches(nullptr), "null does not match");
    for (size_t i = 0; i < kWindowLen; ++i) {
        uint8_t bad[kWindowLen];
        std::memcpy(bad, kWindowBytes, kWindowLen);
        bad[i] ^= 0xff;
        CHECK(!WindowMatches(bad), "a flipped byte at %zu should not match", i);
    }

    // --- PlanDisp round-trips ----------------------------------------------------------
    const uintptr_t site = kBase + kRvaCall;
    int32_t disp = 0;
    CHECK(PlanDisp(site, kMemset, &disp), "memset is in reach of the call");
    {
        uint8_t call[kCallLen] = {0xe8, 0, 0, 0, 0};
        for (int i = 0; i < 4; ++i) call[kCallDispOff + i] = (uint8_t)((uint32_t)disp >> (8 * i));
        CHECK(CurrentTarget(site, call) == kMemset, "planned rel32 decodes back to the target");
    }
    CHECK(!PlanDisp(site, site + 0x90000000ull, &disp), "a target out of rel32 reach must fail");
    CHECK(!PlanDisp(site, 0, &disp), "a null target must fail");
    CHECK(!PlanDisp(0, kMemset, &disp), "a null site must fail");

    // --- the thunk ---------------------------------------------------------------------
    uint8_t th[kThunkLen] = {0};
    const uintptr_t rem = kBase + kRvaEntityRemove;
    CHECK(BuildThunk(th, kMemset, rem), "thunk builds");
    CHECK(!BuildThunk(nullptr, kMemset, rem), "null buffer refused");
    CHECK(!BuildThunk(th, 0, rem), "null memset refused");
    CHECK(!BuildThunk(th, kMemset, 0), "null entity_remove refused");

    // sub rsp,0x28 ... add rsp,0x28 ; ret - the stack must come back balanced, or the
    // handler returns into whatever the thunk left behind.
    CHECK(th[0] == 0x48 && th[1] == 0x83 && th[2] == 0xec && th[3] == 0x28, "sub rsp, 0x28");
    CHECK(th[31] == 0x48 && th[32] == 0x83 && th[33] == 0xc4 && th[34] == 0x28, "add rsp, 0x28");
    CHECK(th[35] == 0xc3, "ret");
    CHECK(th[kThunkMemsetImm - 2] == 0x48 && th[kThunkMemsetImm - 1] == 0xb8, "movabs rax, memset");
    CHECK(th[14] == 0xff && th[15] == 0xd0, "call rax");
    CHECK(th[16] == 0x44 && th[17] == 0x89 && th[18] == 0xe1, "mov ecx, r12d (the id)");
    CHECK(th[kThunkRemoveImm - 2] == 0x48 && th[kThunkRemoveImm - 1] == 0xb8, "movabs rax, remove");
    CHECK(th[29] == 0xff && th[30] == 0xd0, "call rax");

    uint64_t a = 0, bq = 0;
    for (int i = 0; i < 8; ++i) {
        a  |= (uint64_t)th[kThunkMemsetImm + i] << (8 * i);
        bq |= (uint64_t)th[kThunkRemoveImm + i] << (8 * i);
    }
    CHECK(a == (uint64_t)kMemset, "memset immediate little-endian");
    CHECK(bq == (uint64_t)rem, "entity_remove immediate little-endian");

    // The shadow space the two calls need is the whole reason for the 0x28.
    CHECK(0x28 >= 32 + 8, "0x28 covers 32 bytes of shadow space and keeps 16-byte alignment");

    if (g_failures) { std::printf("\n%d check(s) failed\n", g_failures); return 1; }
    std::printf("rejoin: all checks passed\n");
    return 0;
}
