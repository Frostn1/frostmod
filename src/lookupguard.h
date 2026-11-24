// lookupguard.h - lookups that come back empty, and what the game then does with "nothing".
//
// Three families of crash, all on beta21f (build 0x6B032CE7), all from the reports:
//
// 1. A NAME LOOKUP THAT FINDS NOTHING. The game finds a track, tyre, bike or series by name with
//    the same loop everywhere: walk the list, compare names, stop on a match. When nothing
//    matches, the exit loads the index from a stack slot that the not-found path never wrote,
//    and the next instructions multiply it by the entry size and use the entry (often as a %s
//    for sprintf: msvcr90.dll+0x36EDE). The biggest one is the menu after an online event
//    (+0x62E66, the last track, when it is not installed: 35 reports from 31 riders on beta21e
//    under +0x62F32). The tyre fallback (tyrefallback.h) already covers the bike list and the
//    trainer loader; this covers the rest.
//
//    WHAT THIS DOES, per site: the exit's own load runs as it is, then the index is compared
//    with the count the loop itself just walked (the same global). In range, nothing changes.
//    Out of range, it becomes 0, the first entry, and the site's counter goes up. A value that
//    was already a valid index is never touched, so the clamp cannot change a working path.
//    The sites were picked by tracing each exit forward (tools: the beta21f image): the index
//    must reach `imul <it>, <entry size>` for that same list before anything tests it for -1
//    or a sign, nothing may read the flags the clamp leaves, and no branch may land inside the
//    patched bytes. 43 exits pass; two more are consumers of an index that comes in junk:
//      +0x2075D  the second trainer loader's tyre (beta21e +0x208B4, 6 reports)
//      +0x41B42  the bike-list entry's tyre argument (beta21e +0x41C88)
//
// 2. A NaN ANIMATION PHASE (+0x2686C0). 0x22016E samples a keyframed animation channel at
//    phase t ([rbp+0x84]): key = (int)((keys - 1) * t). A NaN t turns into INT_MIN, which is
//    used as a key index, and +0x2686C0 (a dot product) reads 0x1C * INT_MIN bytes away. The
//    report's trail has the NaN height queries at the frame rate (two per ~16 ms), not the
//    physics rate: the NaN is in the rider's render-side animation, not in the bike bodies, so
//    nanrecover (which checks those bodies) saw nothing to reset. A NaN phase now reads as 0
//    (and is written back as 0 so it does not stay), and the key is held to [0, keys - 1].
//
// 3. AN OBJECT THAT IS NOT THERE.
//    +0x2544A3  getter 0x15BCD0 returns NULL by design (no id, no list, no match); the race-
//               start loop reads [rax+0x110] without a test. A NULL now takes the loop's own
//               "next item" path (+0x2547AC), the one it takes for an item it skips.
//    +0x290371  an interface pointer [rbx] read and called through with no test. A NULL now
//               takes the function's own failure return (+0x2903E2, eax = 1), the one it takes
//               when the call fails.
//
// Every site is checked byte for byte before anything is written, and the patches are reverted
// at unload. lookupguard=0 turns off families 1 and 3, animguard=0 family 2.
//
// No Win32 here, so tests/lookupguard_test.cpp runs anywhere.
#pragma once
#include <cstddef>
#include <cstdint>

namespace lookupguard {

// ---- family 1: the clamp sites ---------------------------------------------------------
struct ClampSite {
    uint32_t rva;          // first patched byte
    uint8_t  len;          // bytes patched (the jump to the stub + nops)
    uint8_t  bytes[8];     // what the game has there
    uint8_t  replay;       // how many of them the stub runs as they are (no rip, no branch)
    uint8_t  reg;          // the index register, 0..15 (eax..r15d)
    uint32_t count;        // RVA of the list's count (the int the loop compared with)
    uint32_t back;         // RVA the stub returns to
    uint8_t  clampFirst;   // 1: clamp, then replay (the replayed instruction uses the index)
    uint32_t stride;       // the list's entry size, for the log and the test
    uint32_t cmpRva;       // the loop's `cmp r32, [rip+count]` (0 for the two consumers)
    uint8_t  cmpLen;       // its length; its last four bytes are the disp to `count`
};

constexpr ClampSite kClamps[] = {
    {0x01CFC7, 6, {0x8b, 0x5c, 0x24, 0x68, 0xeb, 0x04}, 4,  3, 0x0F3DC98, 0x01CFD1, 0, 0x234, 0x01CFBF, 6},   // ebx series
    {0x02075D, 7, {0x4d, 0x69, 0xc9, 0x08, 0x06, 0x00, 0x00}, 7,  9, 0x109DECC, 0x020764, 1, 0x608, 0x000000, 0},   // r9d tyre
    {0x041B42, 8, {0x4c, 0x63, 0x9c, 0x24, 0x38, 0x08, 0x00, 0x00}, 8, 11, 0x109DECC, 0x041B4A, 0, 0x608, 0x000000, 0},   // r11d tyre
    {0x05D1A8, 7, {0x8b, 0xac, 0x24, 0xb8, 0x00, 0x00, 0x00}, 7,  5, 0x0F3DC98, 0x05D1AF, 0, 0x234, 0x05D1A0, 6},   // ebp series
    {0x05DA8C, 7, {0x8b, 0xbc, 0x24, 0xb0, 0x00, 0x00, 0x00}, 7,  7, 0x0F3DC64, 0x05DA93, 0, 0x268, 0x05DA84, 6},   // edi list
    {0x05DC58, 8, {0x44, 0x8b, 0xa4, 0x24, 0xb4, 0x00, 0x00, 0x00}, 8, 12, 0x109DECC, 0x05DC60, 0, 0x608, 0x05DC50, 6},   // r12d tyre
    {0x05DCAC, 7, {0x8b, 0xbc, 0x24, 0xb0, 0x00, 0x00, 0x00}, 7,  7, 0x0F3DC80, 0x05DCB3, 0, 0x12C, 0x05DCA4, 6},   // edi list
    {0x05EC2C, 8, {0x44, 0x8b, 0xb4, 0x24, 0xb0, 0x00, 0x00, 0x00}, 8, 14, 0x0F3DC98, 0x05EC34, 0, 0x234, 0x05EC23, 7},   // r14d series
    {0x05F49C, 7, {0x8b, 0xbc, 0x24, 0xb8, 0x00, 0x00, 0x00}, 7,  7, 0x0F3DC64, 0x05F4A3, 0, 0x268, 0x05F494, 6},   // edi list
    {0x05F7E8, 7, {0x8b, 0xb4, 0x24, 0xb4, 0x00, 0x00, 0x00}, 7,  6, 0x109DECC, 0x05F7EF, 0, 0x608, 0x05F7E0, 6},   // esi tyre
    {0x05F83C, 7, {0x8b, 0xbc, 0x24, 0xb8, 0x00, 0x00, 0x00}, 7,  7, 0x0F3DC80, 0x05F843, 0, 0x12C, 0x05F834, 6},   // edi list
    {0x060CB8, 6, {0x8b, 0x5c, 0x24, 0x40, 0xeb, 0x04}, 4,  3, 0x0F48218, 0x060CC2, 0, 0x4334, 0x060CB0, 6},   // ebx bike
    {0x062E66, 7, {0x8b, 0xbc, 0x24, 0xc4, 0x00, 0x00, 0x00}, 7,  7, 0x0F43298, 0x062E6D, 0, 0x4C4, 0x062E5E, 6},   // edi track
    {0x063A9C, 7, {0x8b, 0x84, 0x24, 0xc0, 0x00, 0x00, 0x00}, 7,  0, 0x0F3DC80, 0x063AA3, 0, 0x12C, 0x063A94, 6},   // eax list
    {0x064A00, 7, {0x8b, 0xbc, 0x24, 0xf0, 0x00, 0x00, 0x00}, 7,  7, 0x0F3DC80, 0x064A07, 0, 0x12C, 0x0649F8, 6},   // edi list
    {0x065898, 7, {0x8b, 0x9c, 0x24, 0xc4, 0x00, 0x00, 0x00}, 7,  3, 0x0F48218, 0x06589F, 0, 0x4334, 0x065890, 6},   // ebx bike
    {0x065B0E, 7, {0x8b, 0xbc, 0x24, 0xc0, 0x00, 0x00, 0x00}, 7,  7, 0x109DECC, 0x065B15, 0, 0x608, 0x065B06, 6},   // edi tyre
    {0x065BC0, 7, {0x8b, 0xbc, 0x24, 0xf0, 0x00, 0x00, 0x00}, 7,  7, 0x0F3DC80, 0x065BC7, 0, 0x12C, 0x065BB8, 6},   // edi list
    {0x06B0D8, 7, {0x8b, 0xac, 0x24, 0xb0, 0x00, 0x00, 0x00}, 7,  5, 0x0F3DC98, 0x06B0DF, 0, 0x234, 0x06B0D0, 6},   // ebp series
    {0x06B16A, 8, {0x44, 0x8b, 0xac, 0x24, 0xb0, 0x00, 0x00, 0x00}, 8, 13, 0x109DECC, 0x06B172, 0, 0x608, 0x06B161, 7},   // r13d tyre
    {0x06B1EC, 7, {0x8b, 0xbc, 0x24, 0xb0, 0x00, 0x00, 0x00}, 7,  7, 0x0F3DC80, 0x06B1F3, 0, 0x12C, 0x06B1E4, 6},   // edi list
    {0x06BD37, 7, {0x8b, 0xbc, 0x24, 0xbc, 0x00, 0x00, 0x00}, 7,  7, 0x0F3DC64, 0x06BD3E, 0, 0x268, 0x06BD2F, 6},   // edi list
    {0x075623, 7, {0x8b, 0x9c, 0x24, 0xa0, 0x00, 0x00, 0x00}, 7,  3, 0x0F43298, 0x07562A, 0, 0x4C4, 0x07561B, 6},   // ebx track
    {0x07A3AE, 8, {0x44, 0x8b, 0xa4, 0x24, 0xd8, 0x00, 0x00, 0x00}, 8, 12, 0x0F3DC98, 0x07A3B6, 0, 0x234, 0x07A3A5, 7},   // r12d series
    {0x07A42F, 7, {0x8b, 0x9c, 0x24, 0xb8, 0x00, 0x00, 0x00}, 7,  3, 0x109DECC, 0x07A436, 0, 0x608, 0x07A427, 6},   // ebx tyre
    {0x07A46E, 7, {0x8b, 0x9c, 0x24, 0xb8, 0x00, 0x00, 0x00}, 7,  3, 0x109DECC, 0x07A475, 0, 0x608, 0x07A466, 6},   // ebx tyre
    {0x07A4BE, 8, {0x44, 0x8b, 0xac, 0x24, 0xd8, 0x00, 0x00, 0x00}, 8, 13, 0x0F3DC80, 0x07A4C6, 0, 0x12C, 0x07A4B5, 7},   // r13d list
    {0x07D337, 7, {0x8b, 0xac, 0x24, 0xb4, 0x00, 0x00, 0x00}, 7,  5, 0x0F3DC98, 0x07D33E, 0, 0x234, 0x07D32F, 6},   // ebp series
    {0x07D3BC, 7, {0x8b, 0xbc, 0x24, 0xb8, 0x00, 0x00, 0x00}, 7,  7, 0x109DECC, 0x07D3C3, 0, 0x608, 0x07D3B4, 6},   // edi tyre
    {0x07D400, 7, {0x8b, 0xbc, 0x24, 0xb8, 0x00, 0x00, 0x00}, 7,  7, 0x109DECC, 0x07D407, 0, 0x608, 0x07D3F8, 6},   // edi tyre
    {0x07D44C, 7, {0x8b, 0xb4, 0x24, 0xb4, 0x00, 0x00, 0x00}, 7,  6, 0x0F3DC80, 0x07D453, 0, 0x12C, 0x07D444, 6},   // esi list
    {0x08214C, 7, {0x8b, 0x84, 0x24, 0xa8, 0x00, 0x00, 0x00}, 7,  0, 0x0F3DC80, 0x082153, 0, 0x12C, 0x082144, 6},   // eax list
    {0x0830CC, 7, {0x8b, 0x84, 0x24, 0x84, 0x00, 0x00, 0x00}, 7,  0, 0x109DECC, 0x0830D3, 0, 0x608, 0x0830C4, 6},   // eax tyre
    {0x08DFCE, 6, {0x8b, 0x5c, 0x24, 0x40, 0xeb, 0x04}, 4,  3, 0x0F43298, 0x08DFD8, 0, 0x4C4, 0x08DFC6, 6},   // ebx track
    {0x090357, 6, {0x8b, 0x5c, 0x24, 0x58, 0xeb, 0x04}, 4,  3, 0x0F3DC60, 0x090361, 0, 0x1224, 0x09034F, 6},   // ebx list
    {0x090727, 6, {0x8b, 0x5c, 0x24, 0x58, 0xeb, 0x04}, 4,  3, 0x0F48218, 0x090731, 0, 0x4334, 0x09071F, 6},   // ebx bike
    {0x0909E7, 6, {0x8b, 0x5c, 0x24, 0x58, 0xeb, 0x04}, 4,  3, 0x0F3DC60, 0x0909F1, 0, 0x1224, 0x0909DF, 6},   // ebx list
    {0x0BC27C, 7, {0x8b, 0x84, 0x24, 0xb4, 0x00, 0x00, 0x00}, 7,  0, 0x0F3DC98, 0x0BC283, 0, 0x234, 0x0BC274, 6},   // eax series
    {0x0BC33C, 7, {0x8b, 0xbc, 0x24, 0xb0, 0x00, 0x00, 0x00}, 7,  7, 0x109DECC, 0x0BC343, 0, 0x608, 0x0BC334, 6},   // edi tyre
    {0x0BC38C, 7, {0x8b, 0xbc, 0x24, 0xb0, 0x00, 0x00, 0x00}, 7,  7, 0x109DECC, 0x0BC393, 0, 0x608, 0x0BC384, 6},   // edi tyre
    {0x0E4E3C, 7, {0x8b, 0xbc, 0x24, 0xb4, 0x00, 0x00, 0x00}, 7,  7, 0x0F3DC98, 0x0E4E43, 0, 0x234, 0x0E4E34, 6},   // edi series
    {0x0E8B7D, 8, {0x44, 0x8b, 0xa4, 0x24, 0xb8, 0x00, 0x00, 0x00}, 8, 12, 0x0F48218, 0x0E8B85, 0, 0x4334, 0x0E8B74, 7},   // r12d bike
    {0x0E8DCB, 7, {0x8b, 0xbc, 0x24, 0xd4, 0x00, 0x00, 0x00}, 7,  7, 0x0F3DC98, 0x0E8DD2, 0, 0x234, 0x0E8DC3, 6},   // edi series
    {0x112036, 7, {0x8b, 0x9c, 0x24, 0xb4, 0x00, 0x00, 0x00}, 7,  3, 0x0F43298, 0x11203D, 0, 0x4C4, 0x11202E, 6},   // ebx track
    {0x11A3E6, 7, {0x8b, 0x9c, 0x24, 0xb4, 0x00, 0x00, 0x00}, 7,  3, 0x0F43298, 0x11A3ED, 0, 0x4C4, 0x11A3DE, 6},   // ebx track
};
constexpr size_t kClampCount = sizeof(kClamps) / sizeof(kClamps[0]);

inline const char* ListName(uint32_t stride) {
    switch (stride) {
        case 0x608:  return "tyre";
        case 0x4C4:  return "track";
        case 0x4334: return "bike";
        case 0x234:  return "series";
        default:     return "list";
    }
}

// The stub page: one 0x60-byte slot per stub, then the counters.
constexpr size_t kSlot      = 0x60;
constexpr size_t kAnimSlot  = kClampCount;        // slot of the animation stub
constexpr size_t kNull1Slot = kClampCount + 1;    // +0x2544A3
constexpr size_t kNull2Slot = kClampCount + 2;    // +0x290371
constexpr size_t kSlots     = kClampCount + 3;
constexpr size_t kCountBase = kSlots * kSlot;     // uint32 per slot
constexpr size_t kPageBytes = kCountBase + kSlots * 4;

inline uint8_t Rex(int reg) { return reg >= 8 ? 0x44 : 0; }

/// Emit-helpers over a buffer.
struct Emit {
    uint8_t* s; size_t o; uintptr_t va;
    void b(uint8_t x) { s[o++] = x; }
    void d32(int32_t v) { for (int i = 0; i < 4; ++i) b((uint8_t)((uint32_t)v >> (8 * i))); }
    void q64(uint64_t v) { for (int i = 0; i < 8; ++i) b((uint8_t)(v >> (8 * i))); }
    /// disp32 of a rip-relative operand ending 4 bytes from here, naming absolute `target`.
    void rip(uint64_t target) { d32((int32_t)((int64_t)target - (int64_t)(va + o + 4))); }
    void jmpAbs(uint64_t target) { b(0xff); b(0x25); d32(0); q64(target); }
    void lockInc(uint64_t cell) { b(0xf0); b(0xff); b(0x05); rip(cell); }
};

/// The clamp stub for `c` at `stubVa`. `base` is the game's image base, `counter` the VA of
/// this site's uint32. Touches the index register and the flags only.
inline size_t BuildClamp(uint8_t* s, uintptr_t stubVa, uint64_t base, const ClampSite& c,
                         uint64_t counter) {
    Emit e{s, 0, stubVa};
    const int r = c.reg;
    if (!c.clampFirst) for (int i = 0; i < c.replay; ++i) e.b(c.bytes[i]);
    if (Rex(r)) e.b(0x44);
    e.b(0x3b); e.b((uint8_t)(0x05 | ((r & 7) << 3))); e.rip(base + c.count);   // cmp r32, [count]
    const size_t jbAt = e.o;
    e.b(0x72); e.b(0);                                                        // jb ok
    e.lockInc(counter);
    if (r >= 8) e.b(0x45);
    e.b(0x31); e.b((uint8_t)(0xc0 | ((r & 7) << 3) | (r & 7)));               // xor r32, r32
    s[jbAt + 1] = (uint8_t)(e.o - (jbAt + 2));
    if (c.clampFirst) for (int i = 0; i < c.replay; ++i) e.b(c.bytes[i]);
    e.jmpAbs(base + c.back);
    return e.o;
}

/// `jmp rel32` to the stub, then nops, over the site. False when out of reach.
inline bool BuildSitePatch(uintptr_t siteVa, uintptr_t stubVa, size_t len, uint8_t* out) {
    const int64_t d = (int64_t)stubVa - (int64_t)(siteVa + 5);
    if (len < 5 || d < INT32_MIN || d > INT32_MAX) return false;
    out[0] = 0xE9;
    for (int i = 0; i < 4; ++i) out[1 + i] = (uint8_t)((uint32_t)(int32_t)d >> (8 * i));
    for (size_t i = 5; i < len; ++i) out[i] = 0x90;
    return true;
}

// ---- family 2: the animation phase -----------------------------------------------------
// 0x2202E1 .. 0x22030B: keys, phase, key = (int)((keys-1)*t), and the branch on key+1 < keys.
constexpr uint32_t kAnimWin = 0x2202E1;
constexpr uint8_t  kAnimWinBytes[] = {
    0x8b, 0x4d, 0x64,                          // mov ecx, [rbp+0x64]      keys
    0xf3, 0x0f, 0x10, 0xb5, 0x84, 0x00, 0x00, 0x00,   // movss xmm6, [rbp+0x84]  t
    0x8d, 0x41, 0xff,                          // lea eax, [rcx-1]
    0x66, 0x0f, 0x6e, 0xc0,                    // movd xmm0, eax
    0x0f, 0x5b, 0xc0,                          // cvtdq2ps xmm0, xmm0
    0xf3, 0x0f, 0x59, 0xc6,                    // mulss xmm0, xmm6
    0xf3, 0x0f, 0x2c, 0xd0,                    // cvttss2si edx, xmm0      <- 0x2202FA, patched
    0x44, 0x8d, 0x4a, 0x01,                    // lea r9d, [rdx+1]
    0x44, 0x3b, 0xc9,                          // cmp r9d, ecx
    0x7c, 0x52,                                // jl 0x220359
};
constexpr size_t   kAnimWinLen = sizeof(kAnimWinBytes);
constexpr uint32_t kAnimSite = 0x2202FA;
constexpr size_t   kAnimLen  = 8;              // cvttss2si + lea
constexpr uint32_t kAnimBack = 0x220302;       // cmp r9d, ecx
constexpr uint32_t kAnimFault = 0x2686C0;

/// Phase NaN: t = 0 (register and [rbp+0x84]), key 0, counted. Then key is held to
/// [0, keys-1] (keys in ecx). Touches xmm6 (only when it is NaN), edx, r9d and the flags,
/// which is what the replaced code and the cmp after it touch.
inline size_t BuildAnim(uint8_t* s, uintptr_t stubVa, uint64_t base, uint64_t counter) {
    Emit e{s, 0, stubVa};
    e.b(0x0f); e.b(0x2e); e.b(0xf6);                 // ucomiss xmm6, xmm6
    const size_t jpAt = e.o; e.b(0x7a); e.b(0);      // jp nan
    e.b(0xf3); e.b(0x0f); e.b(0x2c); e.b(0xd0);      // cvttss2si edx, xmm0
    e.b(0x85); e.b(0xd2);                            // test edx, edx
    const size_t jnsAt = e.o; e.b(0x79); e.b(0);     // jns pos
    e.b(0x31); e.b(0xd2);                            // xor edx, edx
    const size_t j1At = e.o; e.b(0xeb); e.b(0);      // jmp done
    s[jnsAt + 1] = (uint8_t)(e.o - (jnsAt + 2));     // pos:
    e.b(0x85); e.b(0xc9);                            // test ecx, ecx
    const size_t jle1 = e.o; e.b(0x7e); e.b(0);      // jle done
    e.b(0x3b); e.b(0xd1);                            // cmp edx, ecx
    const size_t jl1 = e.o; e.b(0x7c); e.b(0);       // jl done
    e.b(0x8d); e.b(0x51); e.b(0xff);                 // lea edx, [rcx-1]
    const size_t j2At = e.o; e.b(0xeb); e.b(0);      // jmp done
    s[jpAt + 1] = (uint8_t)(e.o - (jpAt + 2));       // nan:
    e.lockInc(counter);
    e.b(0x0f); e.b(0x57); e.b(0xf6);                 // xorps xmm6, xmm6
    e.b(0xc7); e.b(0x85); e.d32(0x84); e.d32(0);     // mov dword [rbp+0x84], 0
    e.b(0x31); e.b(0xd2);                            // xor edx, edx
    const size_t done = e.o;                         // done:
    s[j1At + 1] = (uint8_t)(done - (j1At + 2));
    s[jle1 + 1] = (uint8_t)(done - (jle1 + 2));
    s[jl1 + 1]  = (uint8_t)(done - (jl1 + 2));
    s[j2At + 1] = (uint8_t)(done - (j2At + 2));
    e.b(0x44); e.b(0x8d); e.b(0x4a); e.b(0x01);      // lea r9d, [rdx+1]
    e.jmpAbs(base + kAnimBack);
    return e.o;
}

/// What the stub decides, in C, for the test.
inline int AnimKey(float t, int keys, bool* nan) {
    *nan = t != t;
    if (*nan) return 0;
    const float x = (float)(keys - 1) * t;
    int k = (x >= -2147483648.0f && x < 2147483648.0f) ? (int)x : INT32_MIN;
    if (k < 0) return 0;
    if (keys > 0 && k >= keys) return keys - 1;
    return k;
}

// ---- family 3: NULL objects ------------------------------------------------------------
// +0x2544A3: `bt dword [rax+0x110], 8` then `jb 0x2547AC` (skip this item).
constexpr uint32_t kNull1Site = 0x2544A3;
constexpr uint8_t  kNull1Bytes[] = {0x0f, 0xba, 0xa0, 0x10, 0x01, 0x00, 0x00, 0x08,   // bt [rax+0x110], 8
                                    0x0f, 0x82, 0xfb, 0x02, 0x00, 0x00};               // jb 0x2547AC
constexpr size_t   kNull1Len  = sizeof(kNull1Bytes);
constexpr uint32_t kNull1Skip = 0x2547AC;    // inc ebp; cmp ebp, edi; jl (the next item)
constexpr uint32_t kNull1Back = 0x2544B1;
constexpr uint8_t  kNull1Call[] = {0xe8, 0x35, 0x78, 0xf0, 0xff};   // 0x254496 call 0x15BCD0
constexpr uint32_t kNull1CallRva = 0x254496;

inline size_t BuildNull1(uint8_t* s, uintptr_t stubVa, uint64_t base, uint64_t counter) {
    Emit e{s, 0, stubVa};
    e.b(0x48); e.b(0x85); e.b(0xc0);                 // test rax, rax
    const size_t jzAt = e.o; e.b(0x74); e.b(0);      // jz null
    for (int i = 0; i < 8; ++i) e.b(kNull1Bytes[i]); // bt [rax+0x110], 8
    const size_t jbAt = e.o; e.b(0x72); e.b(0);      // jb skip
    e.jmpAbs(base + kNull1Back);
    s[jzAt + 1] = (uint8_t)(e.o - (jzAt + 2));       // null:
    e.lockInc(counter);
    s[jbAt + 1] = (uint8_t)(e.o - (jbAt + 2));       // skip:
    e.jmpAbs(base + kNull1Skip);
    return e.o;
}

// +0x290371: rcx = [rbx]; `mov rax, [rcx]` and `call [rax+0x58]`.
constexpr uint32_t kNull2Site = 0x290371;
constexpr uint8_t  kNull2Bytes[] = {0x48, 0x8b, 0x01,   // mov rax, [rcx]
                                    0xff, 0x50, 0x58};  // call [rax+0x58]
constexpr size_t   kNull2Len  = sizeof(kNull2Bytes);
constexpr uint32_t kNull2Fail = 0x2903E2;               // mov eax, 1: the failure return
constexpr uint32_t kNull2Back = 0x290377;
constexpr uint8_t  kNull2Load[] = {0x48, 0x8b, 0x0b};   // 0x290367 mov rcx, [rbx]
constexpr uint32_t kNull2LoadRva = 0x290367;

inline size_t BuildNull2(uint8_t* s, uintptr_t stubVa, uint64_t base, uint64_t counter) {
    Emit e{s, 0, stubVa};
    e.b(0x48); e.b(0x85); e.b(0xc9);                 // test rcx, rcx
    const size_t jzAt = e.o; e.b(0x74); e.b(0);      // jz null
    e.b(0x48); e.b(0x8b); e.b(0x01);                 // mov rax, [rcx]
    e.b(0xff); e.b(0x50); e.b(0x58);                 // call [rax+0x58]   (returns here)
    e.jmpAbs(base + kNull2Back);
    s[jzAt + 1] = (uint8_t)(e.o - (jzAt + 2));       // null:
    e.lockInc(counter);
    e.jmpAbs(base + kNull2Fail);
    return e.o;
}

}  // namespace lookupguard
