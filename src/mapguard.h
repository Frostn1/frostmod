// mapguard.h - the quad-batch renderer no longer writes through a bad glMapBuffer pointer.
//
// THE CRASH, as a player sees it. On a supercross track the game goes to desktop mid-session:
// an access violation WRITING at mxbikes.exe+0x24085C. Seen on one setup again and again
// (RTX 5080, driver 616.92, ReShade 6.8 as opengl32.dll with heavy GI shaders) and nowhere
// else so far.
//
// WHERE. The game's dynamic quad batcher keeps ten batches of up to 4000 quads (16000
// vertices). Each flush maps that batch's vertex buffer through 0x22AC50, which orphans it
// (glBufferData(GL_ARRAY_BUFFER, size, NULL, GL_STREAM_DRAW)) and tail-jumps to
// glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY). The caller at 0x240A9C adds four fixed offsets
// to whatever came back - positions, texcoords, colours, normals, 192000 / 128000 / 64000
// bytes apart - and starts writing. Nothing checks the pointer. The writes end in 0x22ACC0,
// called at 0x240D1E, which does glUnmapBuffer; the draw follows at 0x238340.
//
// WHAT THE DUMPS SAY. In all four +0x24085C dumps the four array pointers sit exactly at
// their start offsets, so the fault is the first store into a freshly mapped buffer, and the
// pointer behind them was NOT null: 0x14304790, 0x83E64820, 0x6B8E5830, 0xEF3DEFA0. All
// below 4 GB, 16-byte aligned, none page aligned - the shape of a stale or truncated heap
// pointer, not of a driver mapping. So the check below is not just `p != NULL`.
//
// WHAT THIS DOES. Two call instructions are repointed, nothing else: the map call at
// 0x240A9C and the unmap call at 0x240D1E, each to a 12-byte stub that jumps into FrostMod.
// The map side calls the game's own 0x22AC50 and looks at the result. A pointer that is
// committed and writable for the whole buffer is returned untouched - the normal path costs
// one VirtualQuery. A NULL, an unmapped or a read-only pointer is replaced by a scratch
// buffer FrostMod owns, of the size the game asked for. On the matching unmap the scratch is
// uploaded with glBufferSubData (after a real glUnmapBuffer only if GL still thinks the
// buffer is mapped, since SubData on a mapped buffer is an error) and the game's unmap is
// skipped. The batch is drawn, from our copy, instead of the game crashing.
//
// Every other caller of 0x22AC50 / 0x22ACC0 (there are a dozen) is left alone, and
// glMapBuffer itself is not hooked.
//
// WHAT IT DOES NOT FIX. A junk pointer that happens to land in committed, writable memory
// passes the check, and the game then writes 576000 bytes over whatever lives there. That
// cannot be told apart from a good mapping without trusting the driver, which is the thing
// that failed. The real cause is upstream (driver or the GL wrapper in front of it).
//
// No Win32 here, so tests/mapguard_test.cpp runs anywhere.
#pragma once
#include <cstddef>
#include <cstdint>

namespace mapguard {

// ---- RVAs in mxbikes.exe (image base 0x140000000) -------------------------------------
constexpr uintptr_t kRvaMapCall    = 0x240A9C;   // call 0x22AC50 in the quad-batch flush
constexpr uintptr_t kRvaUnmapCall  = 0x240D1E;   // call 0x22ACC0 in the same function
constexpr uintptr_t kRvaMapFn      = 0x22AC50;   // void* map(vb): orphan + glMapBuffer
constexpr uintptr_t kRvaUnmapFn    = 0x22ACC0;   // int unmap(vb, first, count)
constexpr uintptr_t kRvaBindFn     = 0x2293C0;   // bind(vb): glBindBuffer through the game's cache
constexpr uintptr_t kRvaGlOk       = 0x58482C;   // int: vertex buffers usable at all
constexpr uintptr_t kRvaUseMap     = 0x57CC04;   // int: fill by glMapBuffer (else SubData from RAM)
constexpr uintptr_t kRvaGlBufferSubData = 0x398F30;  // the game's resolved GL entry points
constexpr uintptr_t kRvaGlMapBuffer     = 0x398F38;
constexpr uintptr_t kRvaGlUnmapBuffer   = 0x398F40;

// The vertex-buffer record 0x22AC50 takes (rcx = batch + 0xA8).
constexpr size_t kVbCount  = 0x00;   // int, vertices the buffer holds
constexpr size_t kVbStride = 0x04;   // int, bytes per vertex over all four arrays
constexpr size_t kVbGlName = 0x18;   // int, GL buffer name; 0 = no buffer

constexpr uint32_t kGlArrayBuffer = 0x8892;   // GL_ARRAY_BUFFER
constexpr uint32_t kGlBufferMapped = 0x88BC;  // GL_BUFFER_MAPPED

// ---- the bytes we require before writing anything ------------------------------------
// The call sites, with the instructions that set up their argument, so a build that moved
// either call refuses rather than gets a rel32 written into the middle of something else.
constexpr uintptr_t kRvaMapWin   = 0x240A89;   // lea r13,[tbl]; mov rcx,[r13+rdi*8]; add rcx,0xA8; call
constexpr size_t    kMapWinLen   = 24;
constexpr uint8_t   kMapWin[kMapWinLen] = {
    0x4c, 0x8d, 0x2d, 0x10, 0x0b, 0x34, 0x00, 0x49, 0x8b, 0x4c, 0xfd, 0x00,
    0x48, 0x81, 0xc1, 0xa8, 0x00, 0x00, 0x00, 0xe8, 0xaf, 0xa1, 0xfe, 0xff,
};
constexpr uintptr_t kRvaUnmapWin = 0x240D0C;   // mov rax,[r13+rdi*8]; xor edx,edx; mov r8d,[rax+0x78]; lea rcx,[rax+0xA8]; call
constexpr size_t    kUnmapWinLen = 23;
constexpr uint8_t   kUnmapWin[kUnmapWinLen] = {
    0x49, 0x8b, 0x44, 0xfd, 0x00, 0x33, 0xd2, 0x44, 0x8b, 0x40, 0x78, 0x48,
    0x8d, 0x88, 0xa8, 0x00, 0x00, 0x00, 0xe8, 0x9d, 0x9f, 0xfe, 0xff,
};
// And the two functions themselves, since the thunks lean on what they do: which flags pick
// the glMapBuffer path, where the size comes from, and which GL pointers they call.
constexpr size_t  kMapFnLen = 106;   // 0x22AC50 .. 0x22ACBA (through the RAM path's ret)
constexpr uint8_t kMapFnBytes[kMapFnLen] = {
    0x48, 0x83, 0xec, 0x28, 0x83, 0x3d, 0xd1, 0x9b, 0x35, 0x00, 0x00, 0x75, 0x07, 0x33, 0xc0, 0x48,
    0x83, 0xc4, 0x28, 0xc3, 0x83, 0x79, 0x18, 0x00, 0x74, 0xf3, 0x83, 0x3d, 0x93, 0x1f, 0x35, 0x00,
    0x00, 0x74, 0x3e, 0x48, 0x89, 0x5c, 0x24, 0x20, 0x8b, 0x59, 0x04, 0x0f, 0xaf, 0x19, 0xe8, 0x3d,
    0xe7, 0xff, 0xff, 0x41, 0xb9, 0xe0, 0x88, 0x00, 0x00, 0x41, 0x8d, 0x49, 0xb2, 0x48, 0x63, 0xd3,
    0x45, 0x33, 0xc0, 0xff, 0x15, 0x8f, 0xe2, 0x16, 0x00, 0xba, 0xb9, 0x88, 0x00, 0x00, 0x8d, 0x4a,
    0xd9, 0x48, 0x8b, 0x5c, 0x24, 0x20, 0x48, 0x83, 0xc4, 0x28, 0x48, 0xff, 0x25, 0x87, 0xe2, 0x16,
    0x00, 0x48, 0x8b, 0x41, 0x10, 0x48, 0x83, 0xc4, 0x28, 0xc3,
};
constexpr size_t  kUnmapFnLen = 110; // 0x22ACC0 .. 0x22AD2E (through the RAM path's SubData)
constexpr uint8_t kUnmapFnBytes[kUnmapFnLen] = {
    0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x30, 0x83, 0x3d, 0x5d, 0x9b, 0x35, 0x00, 0x00, 0x41,
    0x8b, 0xf0, 0x8b, 0xda, 0x48, 0x8b, 0xf9, 0x75, 0x0d, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x83,
    0xc4, 0x30, 0x5f, 0x5e, 0x5b, 0xc3, 0x83, 0x79, 0x18, 0x00, 0x74, 0xed, 0x83, 0x3d, 0x11, 0x1f,
    0x35, 0x00, 0x00, 0x74, 0x15, 0xb9, 0x92, 0x88, 0x00, 0x00, 0xff, 0x15, 0x40, 0xe2, 0x16, 0x00,
    0x33, 0xc0, 0x48, 0x83, 0xc4, 0x30, 0x5f, 0x5e, 0x5b, 0xc3, 0x44, 0x3b, 0x01, 0x75, 0x29, 0x8b,
    0x59, 0x04, 0x0f, 0xaf, 0xde, 0xe8, 0xa6, 0xe6, 0xff, 0xff, 0x4c, 0x8b, 0x4f, 0x10, 0x4c, 0x63,
    0xc3, 0x33, 0xd2, 0xb9, 0x92, 0x88, 0x00, 0x00, 0xff, 0x15, 0x02, 0xe2, 0x16, 0x00,
};

// Where in those windows the patched call sits (the E8), and the call length.
constexpr size_t kCallLen = 5;
constexpr size_t kMapCallInWin   = kRvaMapCall - kRvaMapWin;      // 19
constexpr size_t kUnmapCallInWin = kRvaUnmapCall - kRvaUnmapWin;  // 18

inline bool Matches(const uint8_t* p, const uint8_t* want, size_t n) {
    if (!p) return false;
    for (size_t i = 0; i < n; ++i)
        if (p[i] != want[i]) return false;
    return true;
}

/// Where the `call rel32` at `siteVa` goes, decoded from its own bytes; 0 if it is not one.
inline uintptr_t CallTarget(uintptr_t siteVa, const uint8_t* site) {
    if (!site || site[0] != 0xE8) return 0;
    int32_t d = 0;
    for (int i = 0; i < 4; ++i) d |= (int32_t)((uint32_t)site[1 + i] << (8 * i));
    return (uintptr_t)((int64_t)(siteVa + kCallLen) + d);
}

/// The five bytes of `call rel32` from `siteVa` to `targetVa`. False when out of reach.
inline bool BuildCall(uintptr_t siteVa, uintptr_t targetVa, uint8_t out[kCallLen]) {
    if (!out || !siteVa || !targetVa) return false;
    const int64_t d = (int64_t)targetVa - (int64_t)(siteVa + kCallLen);
    if (d < INT32_MIN || d > INT32_MAX) return false;
    out[0] = 0xE8;
    for (int i = 0; i < 4; ++i) out[1 + i] = (uint8_t)((uint32_t)(int32_t)d >> (8 * i));
    return true;
}

/// The stub a patched call lands on: `mov rax, fn; jmp rax`. The game's call pushed its own
/// return address, so `fn` is entered exactly as 0x22AC50 / 0x22ACC0 would have been and
/// returns straight to the game. rax is volatile and carries the return value anyway.
constexpr size_t kStubLen = 12;
inline size_t BuildStub(uint8_t* s, uint64_t fn) {
    size_t o = 0;
    s[o++] = 0x48; s[o++] = 0xB8;
    for (int i = 0; i < 8; ++i) s[o++] = (uint8_t)(fn >> (8 * i));
    s[o++] = 0xFF; s[o++] = 0xE0;
    return o;
}

/// Bytes the buffer holds, as 0x22AC50 computes it (count * stride), or 0 when the record
/// does not describe a buffer we would copy. The quad batch is 16000 * 36 = 576000; the cap
/// is far above that and far below anything a junk record could ask us to allocate.
constexpr size_t kMaxBytes = 64u << 20;
inline size_t BufferBytes(int32_t count, int32_t stride) {
    if (count <= 0 || stride <= 0) return 0;
    const uint64_t n = (uint64_t)(uint32_t)count * (uint64_t)(uint32_t)stride;
    return n > kMaxBytes ? 0 : (size_t)n;
}

// ---- is this pointer something the game can write `size` bytes to? ------------------
// What VirtualQuery says about one region. Kept as plain numbers so the rule is testable.
struct Region {
    uintptr_t base;
    uint64_t  size;
    uint32_t  state;     // MEM_COMMIT = 0x1000
    uint32_t  protect;   // PAGE_*
};
constexpr uint32_t kMemCommit = 0x1000;
constexpr uint32_t kPageGuard = 0x100;

// The low byte is the access; PAGE_NOCACHE / PAGE_WRITECOMBINE above it are fine (driver
// mappings are often write-combined). PAGE_GUARD is not: the first touch faults.
inline bool ProtectWritable(uint32_t protect) {
    if (protect & kPageGuard) return false;
    switch (protect & 0xFF) {
    case 0x04:   // PAGE_READWRITE
    case 0x08:   // PAGE_WRITECOPY
    case 0x40:   // PAGE_EXECUTE_READWRITE
    case 0x80:   // PAGE_EXECUTE_WRITECOPY
        return true;
    default:
        return false;
    }
}

enum class Verdict : int { Ok = 0, Null, NotCommitted, NotWritable, Forced };

inline const char* VerdictName(Verdict v) {
    switch (v) {
    case Verdict::Ok:           return "ok";
    case Verdict::Null:         return "NULL";
    case Verdict::NotCommitted: return "not committed memory";
    case Verdict::NotWritable:  return "not writable";
    case Verdict::Forced:       return "forced by mapguardtest";
    default:                    return "?";
    }
}

/// Walk the regions covering [p, p + size). `query(addr, Region*)` is VirtualQuery in the
/// game and a table in the test; false from it means "nothing there". Bounded: a buffer of
/// kMaxBytes spans a handful of regions, never 64. `bad` (may be null) gets the region that
/// failed, for the log.
template <class Query>
inline Verdict Classify(uintptr_t p, size_t size, Query query, Region* bad = nullptr) {
    if (!p) return Verdict::Null;
    if (size == 0) return Verdict::Ok;
    const uintptr_t end = p + size;
    if (end < p) return Verdict::NotCommitted;     // wraps the address space
    uintptr_t a = p;
    for (int i = 0; i < 64 && a < end; ++i) {
        Region r{};
        if (!query(a, &r) || r.size == 0 || a < r.base || a >= r.base + r.size) {
            if (bad) *bad = r;
            return Verdict::NotCommitted;
        }
        if (r.state != kMemCommit) { if (bad) *bad = r; return Verdict::NotCommitted; }
        if (!ProtectWritable(r.protect)) { if (bad) *bad = r; return Verdict::NotWritable; }
        a = (uintptr_t)(r.base + r.size);
    }
    return a >= end ? Verdict::Ok : Verdict::NotCommitted;
}

/// mapguardtest=N: pretend every Nth map at this site failed, so the fallback can be driven
/// in game on a machine where nothing is wrong. `n` counts maps from 1.
inline bool ForceThis(uint64_t n, int every) { return every > 0 && n > 0 && n % (uint64_t)every == 0; }

/// Rate limit for the log: the first three in full, then on each power of two (4, 8, 16 ...),
/// so a guard firing every frame for an hour is ~25 lines, not 200000.
inline bool ShouldLog(uint64_t n) { return n > 0 && (n <= 3 || (n & (n - 1)) == 0); }

// Scratch buffers kept, one per vertex-buffer record. The quad batcher has ten.
constexpr int kMaxSlots = 16;

}  // namespace mapguard
