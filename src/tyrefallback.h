// tyrefallback.h - a bike that names a tyre the game did not load no longer crashes the menu.
//
// THE CRASH, as a player sees it. Open the bike list and the game goes to desktop, every
// time. The stack is msvcr90+0x36EDE (sprintf reading a bad %s) under the bike-list build.
// Reproduced on a licensed copy: a folder mods\tyres\p_mx that is empty, or holds only a
// hidden desktop.ini, shadows the stock p_mx in tyres.pkz. The tyre list then has no p_mx.
//
// WHY. While building the bike list the game looks up each bike's tyre by name in the tyre
// table (two loops: the bike cfg's tyre, or the tyre the player picked). When the name is
// not in the table, all three "not found" exits load the index from an uninitialised stack
// slot ([rsp+0xB8], nothing in the function writes it). That junk index is passed on, scaled
// by the 0x608 entry size, and the resulting address goes to sprintf as a string.
//
// WHAT THIS DOES. Each of the three not-found exits (`mov ebp, [rsp+0xB8]`, seven bytes) is
// replaced with a jump to a small stub of ours that picks a valid index (the first loaded
// tyre), logs one line naming the bike and the missing tyre, and continues where the game's
// own code would have. A tyre that IS found takes the game's path untouched. With no tyres
// loaded at all there is no valid index to give, so the game's own value is kept.
//
// The whole 133-byte span holding both loops is compared before a byte is written: a game
// update that moves or re-encodes any of it turns this off, with a log line, rather than
// patching the middle of something else. MX Bikes only.
//
// No Win32 here, so tests/tyrefallback_test.cpp runs anywhere.
#pragma once
#include <cstddef>
#include <cstdint>

namespace tyrefallback {

// RVAs in mxbikes.exe (image base 0x140000000).
constexpr uintptr_t kRvaSpan     = 0xE8CE6;   // first loop's `cmp [tyreCount], ebx`
constexpr size_t    kSpanLen     = 0x85;      // up to the join at 0xE8D6B
constexpr uintptr_t kRvaJoin     = kRvaSpan + kSpanLen;   // 0xE8D6B: index in ebp from here
constexpr uintptr_t kRvaTyreCount = 0x109DECC;  // int, entries in the tyre table
constexpr uintptr_t kRvaTyreTable = 0xF48618;   // pointer to the table
constexpr size_t    kTyreStride   = 0x608;      // entry size; the name is at +0
constexpr uintptr_t kRvaPickedTyre = 0xE55560;  // the tyre the player picked ("" = bike's own)
constexpr uintptr_t kRvaBikeCount = 0xF48218;   // int, entries in the bike table
constexpr uintptr_t kRvaBikeTable = 0xF4EDE8;   // pointer to the table
constexpr size_t    kBikeStride   = 0x4334;     // entry size; the folder name is at +0
constexpr uint32_t  kJunkSlot     = 0xB8;       // the uninitialised [rsp+0xB8]
constexpr uint32_t  kCfgTyreSlot  = 0x640;      // [rsp+0x640]: the tyre name read from the cfg

// The three not-found exits. Each is `mov ebp, dword ptr [rsp+0xB8]`.
enum Site : int {
    kCfgTyreMissing = 0,     // 0xE8D1C: the bike cfg's tyre is not in the table
    kPickedTyreMissing = 1,  // 0xE8D5B: the tyre the player picked is not in the table
    kNoTyres = 2,            // 0xE8D64: the table is empty (both loops jump here)
    kSiteCount = 3,
};
constexpr uintptr_t kRvaSites[kSiteCount] = {0xE8D1C, 0xE8D5B, 0xE8D64};
constexpr size_t    kSiteLen = 7;
constexpr uint8_t   kSiteBytes[kSiteLen] = {0x8b, 0xac, 0x24, 0xb8, 0x00, 0x00, 0x00};

// The span as the build we read has it (0xE8CE6..0xE8D6A).
constexpr uint8_t kSpanBytes[kSpanLen] = {
    0x39, 0x1d, 0xe0, 0x51, 0xfb, 0x00, 0x8b, 0xeb, 0x7e, 0x74, 0x48, 0x63, 0xcd, 0x48, 0x8d, 0x94,
    0x24, 0x40, 0x06, 0x00, 0x00, 0x48, 0x69, 0xc9, 0x08, 0x06, 0x00, 0x00, 0x48, 0x03, 0x0d, 0x0f,
    0xf9, 0xe5, 0x00, 0xe8, 0xa8, 0x27, 0x1c, 0x00, 0x85, 0xc0, 0x74, 0x59, 0xff, 0xc5, 0x3b, 0x2d,
    0xb2, 0x51, 0xfb, 0x00, 0x7c, 0xd4, 0x8b, 0xac, 0x24, 0xb8, 0x00, 0x00, 0x00, 0xeb, 0x46, 0x39,
    0x1d, 0xa1, 0x51, 0xfb, 0x00, 0x8b, 0xeb, 0x7e, 0x35, 0x90, 0x48, 0x63, 0xcd, 0x48, 0x8d, 0x15,
    0x26, 0xc8, 0xd6, 0x00, 0x48, 0x69, 0xc9, 0x08, 0x06, 0x00, 0x00, 0x48, 0x03, 0x0d, 0xd0, 0xf8,
    0xe5, 0x00, 0xe8, 0x69, 0x27, 0x1c, 0x00, 0x85, 0xc0, 0x74, 0x1a, 0xff, 0xc5, 0x3b, 0x2d, 0x73,
    0x51, 0xfb, 0x00, 0x7c, 0xd5, 0x8b, 0xac, 0x24, 0xb8, 0x00, 0x00, 0x00, 0xeb, 0x07, 0x8b, 0xac,
    0x24, 0xb8, 0x00, 0x00, 0x00,
};

inline bool SpanMatches(const uint8_t* p) {
    if (!p) return false;
    for (size_t i = 0; i < kSpanLen; ++i)
        if (p[i] != kSpanBytes[i]) return false;
    return true;
}

// The index to continue with. Any loaded tyre is better than a junk one; with none loaded
// there is nothing valid to give, so the game's own value stands.
inline int PickIndex(int tyreCount, int gameValue) { return tyreCount > 0 ? 0 : gameValue; }

// `jmp rel32` to the stub, then two nops, over the seven-byte mov. False when the stub is out
// of rel32 reach.
inline bool BuildSitePatch(uintptr_t siteVa, uintptr_t stubVa, uint8_t out[kSiteLen]) {
    const int64_t d = (int64_t)stubVa - (int64_t)(siteVa + 5);
    if (d < INT32_MIN || d > INT32_MAX) return false;
    out[0] = 0xE9;
    for (int i = 0; i < 4; ++i) out[1 + i] = (uint8_t)((uint32_t)(int32_t)d >> (8 * i));
    out[5] = 0x90; out[6] = 0x90;
    return true;
}

// The stub a site jumps to. Saves the volatiles and flags, calls
//     int fn(int site, const uint8_t* gameRsp, int bikeIndex /* r12d */)
// with a 16-aligned stack and shadow space, puts the result in ebp (what the game's mov would
// have loaded), restores, and jumps to the join. rbx parks rsp across the call (non-volatile,
// so the callee keeps it) and is itself saved. Nine pushes, so the game's rsp is ours + 0x48.
constexpr size_t kStubLen = 81;
inline size_t BuildStub(uint8_t* s, int site, uint64_t fn, uint64_t joinVa) {
    size_t o = 0;
    auto b  = [&](uint8_t x) { s[o++] = x; };
    auto b4 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) b((uint8_t)(v >> (8 * i))); };
    auto b8 = [&](uint64_t v) { for (int i = 0; i < 8; ++i) b((uint8_t)(v >> (8 * i))); };
    b(0x50); b(0x51); b(0x52);                             // push rax, rcx, rdx
    b(0x41); b(0x50); b(0x41); b(0x51);                    // push r8, r9
    b(0x41); b(0x52); b(0x41); b(0x53);                    // push r10, r11
    b(0x9C);                                               // pushfq
    b(0x53);                                               // push rbx
    b(0xB9); b4((uint32_t)site);                           // mov ecx, site
    b(0x48); b(0x8D); b(0x54); b(0x24); b(0x48);           // lea rdx, [rsp+0x48]  (game rsp)
    b(0x45); b(0x89); b(0xE0);                             // mov r8d, r12d        (bike index)
    b(0x48); b(0x89); b(0xE3);                             // mov rbx, rsp
    b(0x48); b(0x83); b(0xE4); b(0xF0);                    // and rsp, -16
    b(0x48); b(0x83); b(0xEC); b(0x20);                    // sub rsp, 0x20
    b(0x48); b(0xB8); b8(fn);                              // mov rax, fn
    b(0xFF); b(0xD0);                                      // call rax
    b(0x48); b(0x89); b(0xDC);                             // mov rsp, rbx
    b(0x5B);                                               // pop rbx
    b(0x89); b(0xC5);                                      // mov ebp, eax
    b(0x9D);                                               // popfq
    b(0x41); b(0x5B); b(0x41); b(0x5A);                    // pop r11, r10
    b(0x41); b(0x59); b(0x41); b(0x58);                    // pop r9, r8
    b(0x5A); b(0x59); b(0x58);                             // pop rdx, rcx, rax
    b(0xFF); b(0x25); b4(0);                               // jmp [rip+0]
    b8(joinVa);                                            //   -> the join
    return o;
}

}  // namespace tyrefallback
