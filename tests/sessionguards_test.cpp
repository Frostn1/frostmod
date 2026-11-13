// Invariants over the four session guards in sessionguards.h.
//
// The bytes each install insists on are read back here for what they claim: that the fault
// instruction is where the header says, that the tables and counts decode to the cells the
// guards read, that the "nothing here" paths the guards take are the game's own. The two
// machine-code stubs are checked byte by byte, and on Windows run for real against fake
// game state: a good line or pointer comes out exactly as the game's code would produce it,
// a bad one takes the skip path and is counted.

#include "../src/sessionguards.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

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

using namespace sessionguards;

static uint64_t Imm64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

// ---- +0x45B0 ---------------------------------------------------------------------------
static void TestBikeList() {
    using namespace bikelist;
    // The fault is cmp [r9+rbp+0x20], esi, right after rbp = (int)bike * 0x4334.
    CHECK(kFaultInFn == 0x30, "0x45B0 is byte 0x30 of 0x4580");
    CHECK(kFn[0x21] == 0x48 && kFn[0x22] == 0x63 && kFn[0x23] == 0xe9, "movsxd rbp, ecx: the bike index, signed");
    CHECK(kFn[0x29] == 0x48 && kFn[0x2a] == 0x69 && kFn[0x2b] == 0xed && (size_t)Disp32(kFn + 0x2c) == kRecBytes,
          "imul rbp, rbp, 0x4334");
    CHECK(kFn[kFaultInFn] == 0x41 && kFn[kFaultInFn + 1] == 0x39 && kFn[kFaultInFn + 4] == 0x20,
          "0x45B0: cmp [r9+rbp+0x20], esi");
    // jle to 0x462E, the `mov eax, 1` the guard returns.
    CHECK(kFn[0x35] == 0x7e && kRvaFn + 0x37 + kFn[0x36] == 0x462E, "jle 0x462E");
    CHECK(kEmpty == 1, "0x462E returns 1");
    // The tables, from the function's own loads and from the loops that also read the counts.
    const uintptr_t bt = RipTarget(kFn + kBikeTableAt, kRvaFn + kBikeTableAt, kLoadDisp, kLoadLen);
    const uintptr_t ct = RipTarget(kFn + kCatTableAt, kRvaFn + kCatTableAt, kLoadDisp, kLoadLen);
    CHECK(bt == kRvaBikeListPtr, "r9 = [0xF4EDE8] (got %zx)", (size_t)bt);
    CHECK(ct == kRvaCatListPtr, "r8 = [0xF3DB38] (got %zx)", (size_t)ct);
    CHECK(RipTarget(kBikeTable, kRvaBikeTable, 3, 7) == bt, "the loader's loop reads the same bike list");
    CHECK(RipTarget(kBikeCount, kRvaBikeCount, 2, 6) == kRvaBikeCountVar, "and its count at 0xF48218");
    CHECK(RipTarget(kCatTable, kRvaCatTable, 3, 7) == ct, "0x43E0's loop reads the same category list");
    CHECK(RipTarget(kCatCount, kRvaCatCount, 2, 6) == kRvaCatCountVar, "and its count at 0xF3DC98");

    // thomas4f's dump: rbp = 0xFFFFE3B7BCF4AA80 is a whole number of records, a garbage index.
    const int64_t rbp = (int64_t)0xFFFFE3B7BCF4AA80ull;
    CHECK(rbp % (int64_t)kRecBytes == 0, "the dump's rbp is index * 0x4334");
    const int64_t idx = rbp / (int64_t)kRecBytes;
    CHECK(idx >= INT32_MIN && idx < 0, "the index was %lld", (long long)idx);
    const int32_t cat = 0;
    CHECK(!Admit((int32_t)idx, 40, true, &cat, 5, true), "that index is refused");

    CHECK(Admit(0, 40, true, &cat, 5, true) && Admit(39, 40, true, &cat, 5, true), "every real bike passes");
    CHECK(!Admit(40, 40, true, &cat, 5, true) && !Admit(-1, 40, true, &cat, 5, true), "one past either end does not");
    CHECK(!Admit(0, 0, true, &cat, 5, true), "an empty list admits nothing");
    CHECK(!Admit(0, 40, false, &cat, 5, true), "nor an unallocated one");
    const int32_t badCat = 5, negCat = -1;
    CHECK(!Admit(0, 40, true, &badCat, 5, true) && !Admit(0, 40, true, &negCat, 5, true), "a category out of range");
    CHECK(!Admit(0, 40, true, nullptr, 5, true) && !Admit(0, 40, true, &cat, 5, false), "no category pointer or list");
}

// ---- +0x3B940 --------------------------------------------------------------------------
static void TestWake() {
    using namespace wake;
    CHECK(RipTarget(kHead + kListAt, kRvaFn + kListAt, 3, 7) == kRvaListPtr, "rcx = [0xE85CD8]");
    CHECK(RipTarget(kHead + kVertsAt, kRvaFn + kVertsAt, 3, 7) == kRvaVertsPtr, "rdx = [0xE8E9B8]");
    CHECK(RipTarget(kCountStore, kRvaCountStore, 3, 7) == kRvaCountVar, "the count it zeroes is 0xEBAA28");
    CHECK(kCountStore[0] == 0x44 && kCountStore[1] == 0x89 && kCountStore[2] == 0x1d, "mov [rip+d], r11d (0)");
    // 400 slots; the fault is slot 0's flag, at list + 0x4C - 0x4C.
    CHECK(Disp32(kLoop + 2) == kSlots && (size_t)kSlots * kSlotBytes == 0x8980, "400 slots of 0x58 = 0x8980 (the alloc)");
    CHECK(kRvaLoop + 9 == kRvaFault && kLoop[9] == 0x44 && kLoop[10] == 0x39 && kLoop[12] == 0xb4,
          "0x3B940: cmp [rcx-0x4C], r11d");
    // thomas4f's dump: rcx = 0x4C. That is a NULL list plus the loop's add rcx, 0x4C.
    CHECK(0 + 0x4C - 0x4C == 0, "rcx 0x4C reads address 0");
    int a = 0, b = 0;
    CHECK(Admit(&a, &b), "both blocks: the game runs");
    CHECK(!Admit(nullptr, &b) && !Admit(&a, nullptr) && !Admit(nullptr, nullptr), "either freed: it does not");
}

// ---- +0x1FED9E -------------------------------------------------------------------------
static void TestListBox() {
    using namespace listbox;
    CHECK(kSiteInWin == 24 && kWin[kSiteInWin] == 0x48 && Disp32(kWin + kSiteInWin + 3) == kLines,
          "0x1FED8C: mov rax, [rbx+0x500]");
    CHECK(kRvaFault - kRvaWin == 42 && kWin[42] == 0x44 && kWin[43] == 0x8b && kWin[45] == 0x90,
          "0x1FED9E: mov r8d, [rax+rdx*4]");
    CHECK(kRvaWin + kWinLen == kRvaBack, "the window ends where the stub returns");
    CHECK(Disp32(kWin + kSiteInWin + 14) == kOffs, "rax = [rbx+0x4F0]");
    // The jl that skips a line above the scroll position goes where the stub skips to.
    CHECK(kWin[kSkipJlAt] == 0x0f && kWin[kSkipJlAt + 1] == 0x8c &&
          kRvaWin + kSkipJlAt + 6 + Disp32(kWin + kSkipJlAt + 2) == kRvaSkip, "jl 0x1FEE30");
    CHECK(kRvaTail + 2 == kRvaSkip && kTail[2] == 0xff && kTail[3] == 0xc7, "0x1FEE30: inc edi (next line)");
    CHECK(kTail[0] == 0xff && kTail[1] == 0xc6, "a drawn line counts a row (inc esi); a skipped one does not");
    // The add: offs[count] = used, so a real offset is below `used` and a real item below count.
    CHECK(Disp32(kAdd + 3) == kItemCount && Disp32(kAdd + 9) == kTextUsed && Disp32(kAdd + 16) == kOffs,
          "the add reads count, used and offs");
    CHECK(kAdd[30] == 0x89 && kAdd[31] == 0x04 && kAdd[32] == 0x91, "mov [rcx+rdx*4], eax: offs[count] = used");

    // The stub.
    uint8_t s[kPageBytes];
    std::memset(s, 0xCC, sizeof(s));
    const uintptr_t at = 0x7FF612340000ull;
    const uint64_t back = 0x140000000ull + kRvaBack, skip = 0x140000000ull + kRvaSkip;
    const size_t n = BuildStub(s, at, back, skip);
    CHECK(n == kStubLen && n <= kCountOff, "stub is %zu bytes, before the counters", n);
    CHECK(kCountOff % 4 == 0 && kItemOff % 4 == 0 && kTextOff % 4 == 0 && kObjOff % 8 == 0 &&
              kObjOff + 8 <= kPageBytes, "cells aligned, in the page");
    // Its two reads are the game's, byte for byte.
    CHECK(std::memcmp(s + 3, kWin + kSiteInWin, 11) == 0, "mov rax,[rbx+0x500]; movsxd rdx,[rax+rbp]");
    CHECK(std::memcmp(s + 22, kWin + kSiteInWin + 11, 11) == 0, "mov rax,[rbx+0x4F0]; mov r8d,[rax+rdx*4]");
    CHECK(s[14] == 0x3b && s[15] == 0x93 && Disp32(s + 16) == kItemCount, "cmp edx, [rbx+0x4C8]");
    CHECK(s[33] == 0x44 && s[34] == 0x3b && s[35] == 0x83 && Disp32(s + 36) == kTextUsed, "cmp r8d, [rbx+0x4C4]");
    CHECK(s[kJae1At] == 0x73 && kJae1At + 2 + s[kJae1At + 1] == kBadAt, "jae (unsigned: a negative item too)");
    CHECK(s[kJae2At] == 0x73 && kJae2At + 2 + s[kJae2At + 1] == kBadAt, "jae on the offset");
    CHECK(s[kBackJmpAt] == 0xff && s[kBackJmpAt + 1] == 0x25 && Disp32(s + kBackJmpAt + 2) == 0 &&
              Imm64(s + kBackJmpAt + 6) == back, "good line: jmp 0x1FEDA2");
    CHECK(kBackJmpAt + 14 == kBadAt, "bad follows");
    CHECK(s[kBadAt] == 0xf0 && s[kBadAt + 1] == 0xff && s[kBadAt + 2] == 0x05 &&
              at + kBadAt + 7 + Disp32(s + kBadAt + 3) == at + kCountOff, "lock inc [count]");
    CHECK(at + kBadAt + 13 + Disp32(s + kBadAt + 9) == at + kItemOff, "mov [item], edx");
    CHECK(at + kBadAt + 20 + Disp32(s + kBadAt + 16) == at + kTextOff, "mov [text], r8d");
    CHECK(at + kBadAt + 27 + Disp32(s + kBadAt + 23) == at + kObjOff, "mov [obj], rbx");
    CHECK(s[kSkipJmpAt] == 0xff && s[kSkipJmpAt + 1] == 0x25 && Imm64(s + kSkipJmpAt + 6) == skip &&
              kSkipJmpAt + 14 == kStubLen, "bad line: jmp 0x1FEE30");

    // The decision.
    const int32_t offs[3] = {0, 10, 25};
    const int32_t line[6] = {0, 1, 2, 3, -1, 0x3AEC0280};
    CHECK(Decide(line, offs, 0, 3, 40) == 0 && Decide(line, offs, 2, 3, 40) == 25, "real lines draw their item");
    CHECK(Decide(line, offs, 3, 3, 40) == -1, "an item one past the end is skipped");
    CHECK(Decide(line, offs, 4, 3, 40) == -1, "a negative item is skipped");
    CHECK(Decide(line, offs, 5, 3, 40) == -1, "thomas4f's 0x3AEC0280 is skipped");
    CHECK(Decide(line, offs, 2, 3, 25) == -1, "an offset at or past the used text is skipped");
}

// ---- +0x2A42F0 -------------------------------------------------------------------------
static void TestPtr64() {
    using namespace ptr64;
    CHECK(kSite[8] == 0x8b && kSite[9] == 0x08, "0x127111: mov ecx, [rax] - 32 bits, no REX.W");
    CHECK(kSite[0] == 0x48 && kSite[1] == 0x8d && Disp32(kSite + 4) == kSlot, "lea rax, [rsp+0x2D8]");
    CHECK(kSite[kCallInSite] == 0xe8 &&
          kRvaSite + kCallInSite + 5 + Disp32(kSite + kCallInSite + 1) == kRvaCallee, "call 0x2A42F0");
    CHECK(kRvaSite + kCallInSite == kRvaBack, "the stub returns to the call");
    CHECK(kCallee[0] == 0x8b && kCallee[1] == 0x11, "0x2A42F0: mov edx, [rcx] - it is a pointer");
    // The dispatcher spills all of rdx, and [rsp+0x2D8] is that spill.
    CHECK(kDispatch[6] == 0x48 && kDispatch[7] == 0x89 && kDispatch[8] == 0x50 && kDispatch[9] == 0x10,
          "mov [entry rsp+0x10], rdx (64 bits)");
    int pushes = 0;
    for (size_t i = 18; i < 30; ++i) pushes += (kDispatch[i] >= 0x50 && kDispatch[i] <= 0x57) ? 1 : 0;
    CHECK(pushes == 8, "8 pushes (%d)", pushes);
    const int32_t frame = Disp32(kDispatch + 33);
    CHECK(frame == 0x288, "sub rsp, 0x288");
    // entry rsp = rsp + frame + 8 pushes * 8, and rdx's home is entry rsp + 0x10 (the return
    // address is at +0, rcx's home at +8).
    CHECK(frame + 8 * 8 + 0x10 == kSlot, "rdx's home is [rsp+0x2D8] in the job");

    // thomas4f's dump: rcx = 0x64BFD130 from the slot 0x000000D364BFD130.
    const uint64_t slot = 0x000000D364BFD130ull;
    CHECK(Passed(slot, false) == 0x64BFD130ull, "stock hands over the low half");
    CHECK(Passed(slot, true) == slot, "the fix hands over the pointer");
    CHECK(StockWouldTruncate(slot), "and that launch is counted");
    const uint64_t low = 0x000000003F1FE130ull;
    CHECK(Passed(low, true) == Passed(low, false) && !StockWouldTruncate(low),
          "a stack below 4 GB is passed exactly as before");

    uint8_t s[kPageBytes];
    std::memset(s, 0xCC, sizeof(s));
    const uintptr_t at = 0x7FF612350000ull;
    const uint64_t back = 0x140000000ull + kRvaBack;
    const size_t n = BuildStub(s, at, back);
    CHECK(n == kStubLen && n <= kCountOff, "stub is %zu bytes", n);
    CHECK(s[0] == 0x48 && s[1] == 0x8b && s[2] == 0x8c && s[3] == 0x24 && Disp32(s + 4) == kSlot,
          "mov rcx, [rsp+0x2D8] - the 64-bit load");
    CHECK(s[kJzAt] == 0x74 && kJzAt + 2 + s[kJzAt + 1] == kLeaAt, "jz past the count");
    CHECK(at + kJzAt + 9 + Disp32(s + kJzAt + 5) == at + kCountOff, "lock inc [count]");
    CHECK(at + kJzAt + 16 + Disp32(s + kJzAt + 12) == at + kLastOff, "mov [last], rcx");
    CHECK(std::memcmp(s + kLeaAt, kSite, 8) == 0, "the game's own lea, so rax is as it was");
    CHECK(s[kJmpAt] == 0xff && s[kJmpAt + 1] == 0x25 && Imm64(s + kJmpAt + 6) == back &&
              kJmpAt + 14 == kStubLen, "jmp to the call");
    CHECK(kCountOff % 4 == 0 && kLastOff % 8 == 0 && kLastOff + 8 <= kPageBytes, "cells aligned, in the page");
}

static void TestRates() {
    CHECK(ShouldLog(1) && ShouldLog(2) && ShouldLog(3) && ShouldLog(4) && !ShouldLog(5) && !ShouldLog(0),
          "first three, then 2^k");
}

#ifdef _WIN32
// ---- the stubs, run for real ----------------------------------------------------------
static uint8_t* ExecPage() {
    return (uint8_t*)VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
}

static void TestListBoxLive() {
    using namespace listbox;
    uint8_t* mem = ExecPage();
    CHECK(mem != nullptr, "VirtualAlloc");
    if (!mem) return;
    std::memset(mem, 0xCC, 0x1000);
    // A thunk that stands in for the draw loop: rbx = obj, rbp = 4 * line, jump to the stub.
    // "back" returns r8d (the offset the game would use), "skip" returns -1.
    uint8_t* stub = mem + 0x800;
    uint8_t* t = mem;
    size_t o = 0;
    auto b = [&](uint8_t x) { t[o++] = x; };
    b(0x53); b(0x55);                                // push rbx; push rbp
    b(0x48); b(0x89); b(0xcb);                       // mov rbx, rcx
    b(0x48); b(0x89); b(0xd5);                       // mov rbp, rdx
    b(0x48); b(0xb8);                                // mov rax, stub
    for (int i = 0; i < 8; ++i) b((uint8_t)((uint64_t)(uintptr_t)stub >> (8 * i)));
    b(0xff); b(0xe0);                                // jmp rax
    const size_t backAt = o;
    b(0x44); b(0x89); b(0xc0);                       // mov eax, r8d
    b(0x5d); b(0x5b); b(0xc3);                       // pop rbp; pop rbx; ret
    const size_t skipAt = o;
    b(0xb8); b(0xff); b(0xff); b(0xff); b(0xff);     // mov eax, -1
    b(0x5d); b(0x5b); b(0xc3);
    std::memset(stub + kCountOff, 0, kPageBytes - kCountOff);
    BuildStub(stub, (uintptr_t)stub, (uint64_t)(uintptr_t)(t + backAt), (uint64_t)(uintptr_t)(t + skipAt));
    FlushInstructionCache(GetCurrentProcess(), mem, 0x1000);

    using Thunk = int32_t (*)(void* obj, int64_t rbp);
    const Thunk run = (Thunk)(void*)t;
    alignas(8) static uint8_t obj[0x600];
    static int32_t offs[3] = {0, 12, 30};
    static int32_t line[5] = {2, 0, 1, 7, 0x3AEC0280};
    std::memset(obj, 0, sizeof(obj));
    const int32_t count = 3, used = 44;
    std::memcpy(obj + kItemCount, &count, 4);
    std::memcpy(obj + kTextUsed, &used, 4);
    int32_t* po = offs; int32_t* pl = line;
    std::memcpy(obj + kOffs, &po, 8);
    std::memcpy(obj + kLines, &pl, 8);
    const volatile uint32_t* skipped = (const volatile uint32_t*)(stub + kCountOff);

    CHECK(run(obj, 0) == 30 && run(obj, 4) == 0 && run(obj, 8) == 12, "real lines read their item's offset");
    CHECK(*skipped == 0, "nothing counted");
    CHECK(run(obj, 12) == -1 && *skipped == 1, "item 7 of 3: skipped, counted");
    CHECK(*(volatile uint32_t*)(stub + kItemOff) == 7, "and the item recorded");
    CHECK(*(void* volatile*)(stub + kObjOff) == (void*)obj, "and the list box");
    CHECK(run(obj, 16) == -1 && *skipped == 2, "0x3AEC0280: skipped, never read");
    offs[0] = 44;   // at the end of the used text: not an item's start
    CHECK(run(obj, 4) == -1 && *skipped == 3, "an offset past the text is skipped");
    CHECK(*(volatile uint32_t*)(stub + kTextOff) == 44, "with its offset recorded");
    VirtualFree(mem, 0, MEM_RELEASE);
}

static void TestPtr64Live() {
    using namespace ptr64;
    uint8_t* mem = ExecPage();
    CHECK(mem != nullptr, "VirtualAlloc");
    if (!mem) return;
    std::memset(mem, 0xCC, 0x1000);
    uint8_t* stub = mem + 0x800;
    uint8_t* t = mem;
    size_t o = 0;
    auto b = [&](uint8_t x) { t[o++] = x; };
    // A frame like the job's: the argument in [rsp+0x2D8], rcx and rax clobbered first.
    b(0x48); b(0x81); b(0xec); b(0xe8); b(0x02); b(0x00); b(0x00);              // sub rsp, 0x2E8
    b(0x48); b(0x89); b(0x8c); b(0x24); b(0xd8); b(0x02); b(0x00); b(0x00);     // mov [rsp+0x2D8], rcx
    b(0x48); b(0xc7); b(0xc1); b(0x5f); b(0x03); b(0x00); b(0x00);              // mov rcx, 0x35F (the job)
    b(0x48); b(0xb8);                                                           // mov rax, stub
    for (int i = 0; i < 8; ++i) b((uint8_t)((uint64_t)(uintptr_t)stub >> (8 * i)));
    b(0xff); b(0xe0);                                                           // jmp rax
    const size_t backAt = o;
    // Return rcx (what the callee gets) if rax is the lea the game left, a marker if not.
    b(0x48); b(0x8d); b(0x94); b(0x24); b(0xd8); b(0x02); b(0x00); b(0x00);     // lea rdx, [rsp+0x2D8]
    b(0x48); b(0x39); b(0xd0);                                                  // cmp rax, rdx
    b(0x74); b(0x0c);                                                           // je mov rax, rcx
    b(0x48); b(0xb8); for (int i = 0; i < 8; ++i) b(0xee);                      // mov rax, 0xEEEE... (marker)
    b(0xeb); b(0x03);                                                           // jmp +3
    b(0x48); b(0x89); b(0xc8);                                                  // mov rax, rcx
    b(0x48); b(0x81); b(0xc4); b(0xe8); b(0x02); b(0x00); b(0x00);              // add rsp, 0x2E8
    b(0xc3);
    std::memset(stub + kCountOff, 0, kPageBytes - kCountOff);
    BuildStub(stub, (uintptr_t)stub, (uint64_t)(uintptr_t)(t + backAt));
    FlushInstructionCache(GetCurrentProcess(), mem, 0x1000);

    using Thunk = uint64_t (*)(uint64_t slot);
    const Thunk run = (Thunk)(void*)t;
    const volatile uint32_t* high = (const volatile uint32_t*)(stub + kCountOff);
    CHECK(run(0x3F1FE130ull) == 0x3F1FE130ull && *high == 0, "below 4 GB: the same value, not counted");
    CHECK(run(0x000000D364BFD130ull) == 0x000000D364BFD130ull, "above: the whole pointer (rax as the lea left it)");
    CHECK(*high == 1 && *(volatile uint64_t*)(stub + kLastOff) == 0x000000D364BFD130ull, "counted and recorded");
    VirtualFree(mem, 0, MEM_RELEASE);
}
#endif

static void TestRiderInfo() {
    using namespace sessionguards;
    using namespace sessionguards::riderinfo;
    // The tail jump reaches the fill, and the case's call reaches the function.
    CHECK(kFn[kTailJmpAt] == 0xe9 && RipTarget(kFn + kTailJmpAt, kRvaFn + kTailJmpAt, 1, 5) == kRvaFill,
          "0x1B44C0 tail-jumps to 0x19A7A0");
    CHECK(kCase[14] == 0xe8 && RipTarget(kCase + 14, kRvaCase + 14, 1, 5) == kRvaFn, "the case calls 0x1B44C0");
    // The miss path: test rax,rax; jne over `mov eax, 1; ...; ret`, with nothing stored.
    CHECK(kFn[0x1A] == 0x75 && kFn[0x1C] == 0xb8 && kFn[0x1D] == kMiss && kFn[0x26] == 0xc3, "miss returns 1");
    // The caller: a dispatcher call whose result is dropped, then list 1's count from the struct.
    CHECK(kCaller[0] == 0xff && kCaller[1] == 0x15 && kCaller[6] == 0x8b, "call [dispatcher]; mov eax, [count]");
    CHECK(Disp32(kCaller + 9) == (int32_t)(0x160 + kList1Count), "list 1 count is struct +0x2EFC (rsp+0x305C)");
    CHECK(kList2Count + 4 + kListRoom * 0x64 <= kOutBytes && kList1Count < kList2Count, "the lists lie inside the struct");
    CHECK(kRvaFault == 0x7A860 && kRvaFault2 == 0x7A8E0, "sites");
    CHECK(ShouldClear(1, (void*)1) && !ShouldClear(0, (void*)1) && !ShouldClear(1, nullptr), "clear only a miss");
}

static void TestMipmap() {
    using namespace sessionguards::mipmap;
    // All three take (src rcx, dst rdx, w r8d, h r9d, bpp on the stack) and return 0.
    CHECK(kColumn[2] == 0x48 && kColumn[3] == 0x8b && kColumn[4] == 0xfa, "column: rdi = dst");
    CHECK(kRow[2] == 0x48 && kRow[3] == 0x8b && kRow[4] == 0xfa, "row: rdi = dst");
    CHECK(kBox[17] == 0x41 && kBox[18] == 0x8b && kBox[19] == 0xc0, "box: eax = w");
    CHECK(kRvaFaultWrite > kRvaBox && kRvaFaultRead > kRvaBox && kRvaFaultWrite < 0x255F50, "faults inside the box filter");
    uint8_t a = 0, b = 0;
    CHECK(Admit(&a, &b) && !Admit(nullptr, &b) && !Admit(&a, nullptr), "NULL either side is refused");
}

int main() {
    TestBikeList();
    TestWake();
    TestListBox();
    TestPtr64();
    TestRiderInfo();
    TestMipmap();
    TestRates();
#ifdef _WIN32
    TestListBoxLive();
    TestPtr64Live();
#endif
    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("sessionguards: all checks passed\n");
    return 0;
}
