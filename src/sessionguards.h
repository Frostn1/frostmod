// sessionguards.h - four crashes at the end of a session, and one at random, each turned into
// the path the game already has for "nothing here".
//
// All four are on thomas4f's mxbmrp3 known-crash list for beta21e (build 0x6A21833D). Each
// section below says where the fault is, why it happens as far as the code shows, and what the
// guard does instead. Every guard checks its exact bytes before it writes, stays off on a
// mismatch, logs once when it arms, and has its own key in frostmod_radar.cfg.
//
//   +0x45B0    bike list read with an index that is not a bike      bikelistguard=0 turns it off
//   +0x3B940   wake effect update after the track freed its list    wakeguard=0
//   +0x1FED9E  list box line naming an item it does not have        listboxguard=0
//   +0x2A42F0  stack pointer passed through 32 bits (job 863)       ptr64fix=0
//
// No Win32 here, so tests/sessionguards_test.cpp runs anywhere.
#pragma once
#include <cstddef>
#include <cstdint>

namespace sessionguards {

inline bool Matches(const uint8_t* p, const uint8_t* want, size_t n) {
    if (!p || !want) return false;
    for (size_t i = 0; i < n; ++i)
        if (p[i] != want[i]) return false;
    return true;
}

inline int32_t Disp32(const uint8_t* p) {
    int32_t d = 0;
    for (int i = 0; i < 4; ++i) d |= (int32_t)((uint32_t)p[i] << (8 * i));
    return d;
}

/// The RVA a rip-relative operand names: instruction at `rva`, `len` bytes, disp32 at `at`.
inline uintptr_t RipTarget(const uint8_t* insn, uintptr_t rva, size_t at, size_t len) {
    return (uintptr_t)((int64_t)(rva + len) + Disp32(insn + at));
}

/// Rate limit for the log: the first three in full, then on each power of two.
inline bool ShouldLog(uint64_t n) { return n > 0 && (n <= 3 || (n & (n - 1)) == 0); }

// =====================================================================================
// +0x45B0  -  BIKE LIST INDEX
//
// 0x4580 is `int f(int bike, int* cat)`: does bike `bike` share a name with category `*cat`?
// The bike list (bikes\*.ini, loaded at 0x2884) is an array of 0x4334-byte records at
// [0x140F4EDE8], count at [0x140F48218]; each record keeps a count at +0x20 and 32-byte names
// from +0x24. The function reads the record's count first:
//   0x45A1  movsxd rbp, ecx
//   0x45A9  imul   rbp, rbp, 0x4334
//   0x45B0  cmp    [r9 + rbp + 0x20], esi        ; <- the fault
//   0x45B5  jle    0x462E                        ; no names: return 1
// It never checks `bike`. The callers (0x43E0, 0x4730, 0x8EF3E; reached from the menu code that
// user32 and dinput8 call into) pass whatever bike index their screen holds. thomas4f's dump has
// rbp = 0xFFFFE3B7BCF4AA80, which is exactly -1807526624 * 0x4334: the index itself was garbage,
// read after three Race Deinits while the rider sat in the menus. Where that index went stale is
// upstream in a screen we have not traced.
//
// The guard: a detour on 0x4580 that returns 1 - what the function returns for a bike with no
// names - when the bike index is outside [0, count), the list is not allocated, or the category
// index is outside its own list. A real bike goes straight to the game's code.
// =====================================================================================
namespace bikelist {

constexpr uintptr_t kRvaFn    = 0x4580;
constexpr uintptr_t kRvaFault = 0x45B0;
constexpr size_t    kRecBytes = 0x4334;   // one bike
constexpr size_t    kCatBytes = 0x234;    // one category
constexpr int       kEmpty    = 1;        // 0x462E: mov eax, 1 - "no names in common"

// 0x4580 .. 0x45BE: the prologue MinHook moves, the two table loads, the record read that
// faults and its jle to `return 1`, and the category table load.
constexpr size_t  kFnLen = 0x3E;
constexpr uint8_t kFn[kFnLen] = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
    0x48, 0x89, 0x7c, 0x24, 0x20, 0x41, 0x54, 0x48, 0x83, 0xec, 0x20,
    0x4c, 0x8b, 0x0d, 0x47, 0xa8, 0xf4, 0x00,          // 0x459A mov r9, [bike list]
    0x48, 0x63, 0xe9,                                  // 0x45A1 movsxd rbp, ecx
    0x33, 0xf6,                                        //        xor esi, esi
    0x48, 0x8b, 0xfa,                                  //        mov rdi, rdx (cat*)
    0x48, 0x69, 0xed, 0x34, 0x43, 0x00, 0x00,          // 0x45A9 imul rbp, rbp, 0x4334
    0x41, 0x39, 0x74, 0x29, 0x20,                      // 0x45B0 cmp [r9+rbp+0x20], esi
    0x7e, 0x77,                                        // 0x45B5 jle 0x462E (return 1)
    0x4c, 0x8b, 0x05, 0x7a, 0x95, 0xf3, 0x00,          // 0x45B7 mov r8, [category list]
};
constexpr size_t kBikeTableAt = 0x1A, kCatTableAt = 0x37, kLoadLen = 7, kLoadDisp = 3;
constexpr size_t kFaultInFn = kRvaFault - kRvaFn;      // 0x30

// Where the counts live, proved by an instruction that reads the count next to one that reads
// the same table (the loader's lookup loop, and 0x43E0's category loop).
constexpr uintptr_t kRvaBikeCount = 0x2D51;   // mov eax, [bike count]
constexpr uint8_t   kBikeCount[6] = {0x8b, 0x05, 0xc1, 0x54, 0xf4, 0x00};
constexpr uintptr_t kRvaBikeTable = 0x2D72;   // add rcx, [bike list]
constexpr uint8_t   kBikeTable[7] = {0x48, 0x03, 0x0d, 0x6f, 0xc0, 0xf4, 0x00};
constexpr uintptr_t kRvaCatCount  = 0x44E1;   // cmp [category count], ebx
constexpr uint8_t   kCatCount[6]  = {0x39, 0x1d, 0xb1, 0x97, 0xf3, 0x00};
constexpr uintptr_t kRvaCatTable  = 0x44FF;   // add rcx, [category list]
constexpr uint8_t   kCatTable[7]  = {0x48, 0x03, 0x0d, 0x32, 0x96, 0xf3, 0x00};

// The data RVAs those decode to in beta21e (the install decodes; the test checks these).
constexpr uintptr_t kRvaBikeListPtr  = 0xF4EDE8;
constexpr uintptr_t kRvaBikeCountVar = 0xF48218;
constexpr uintptr_t kRvaCatListPtr   = 0xF3DB38;
constexpr uintptr_t kRvaCatCountVar  = 0xF3DC98;

/// Whether the game's own code may run: both indices name an entry of an allocated list.
inline bool Admit(int32_t bike, int32_t bikeCount, bool bikeList,
                  const int32_t* cat, int32_t catCount, bool catList) {
    if (!bikeList || bike < 0 || bike >= bikeCount) return false;
    if (!cat || !catList || *cat < 0 || *cat >= catCount) return false;
    return true;
}

}  // namespace bikelist

// =====================================================================================
// +0x3B940  -  WAKE EFFECTS AFTER THE TRACK IS GONE
//
// The water wake effect (effects\wake.tga) keeps 400 slots of 0x58 bytes at [0x140E85CD8] and
// a 400-slot vertex buffer at [0x140E8E9B8], both allocated by 0x3B1C0 when a track loads.
// Each frame 0x5A3C0 calls 0x3B8E0(dt), which zeroes the live count at [0x140EBAA28] and walks
// the slots:
//   0x3B8E4  mov rcx, [wake list]
//   0x3B91D  add rcx, 0x4C
//   0x3B940  cmp [rcx - 0x4C], r11d              ; <- the fault: slot 0's "alive" flag
// The track teardown 0x4F7E0 (from 0x71F48, 0x88701, 0xBCCB5) frees both blocks and zeroes both
// pointers. A frame that runs after it finds the list NULL: thomas4f's dump has rcx = 0x4C and
// a zeroed register file, which is the NULL list plus the 0x4C the loop adds. The mswsock
// frames in that dump are what was left on the stack by the network teardown of the same race
// end; the faulting code is the effect update.
//
// The guard: a detour on 0x3B8E0 that, when either block is NULL, stores 0 in the live count
// (the first thing the function does) and returns 0 (what it returns) without walking the slots.
// The count matters: 0x5AA50 uploads that many vertices from the (now NULL) buffer.
// If the teardown runs on another thread and frees the list between the check and the walk,
// this does not catch it; the dump shows the pointer already NULL, which this does.
// =====================================================================================
namespace wake {

constexpr uintptr_t kRvaFn    = 0x3B8E0;
constexpr uintptr_t kRvaFault = 0x3B940;
constexpr int       kSlots    = 400;
constexpr size_t    kSlotBytes = 0x58;

// 0x3B8E0 .. 0x3B8F2: sub rsp; the two pointer loads the guard reads.
constexpr size_t  kHeadLen = 18;
constexpr uint8_t kHead[kHeadLen] = {
    0x48, 0x83, 0xec, 0x58,                            // sub rsp, 0x58
    0x48, 0x8b, 0x0d, 0xed, 0xa3, 0xe4, 0x00,          // 0x3B8E4 mov rcx, [wake list]
    0x48, 0x8b, 0x15, 0xc6, 0x30, 0xe5, 0x00,          // 0x3B8EB mov rdx, [wake vertices]
};
constexpr size_t kListAt = 4, kVertsAt = 11;
// 0x3B924: mov [live count], r11d (r11d = 0), the store the guard repeats.
constexpr uintptr_t kRvaCountStore = 0x3B924;
constexpr uint8_t   kCountStore[7] = {0x44, 0x89, 0x1d, 0xfd, 0xf0, 0xe7, 0x00};
// 0x3B937 .. 0x3B94A: 400 slots, and the read that faults.
constexpr uintptr_t kRvaLoop = 0x3B937;
constexpr size_t    kLoopLen = 19;
constexpr uint8_t   kLoop[kLoopLen] = {
    0x41, 0xb9, 0x90, 0x01, 0x00, 0x00,                // mov r9d, 400
    0x0f, 0x1f, 0x00,                                  // nop
    0x44, 0x39, 0x59, 0xb4,                            // 0x3B940 cmp [rcx-0x4C], r11d
    0x0f, 0x84, 0xb2, 0x01, 0x00, 0x00,                // je 0x3BAFC (slot not alive)
};

constexpr uintptr_t kRvaListPtr  = 0xE85CD8;
constexpr uintptr_t kRvaVertsPtr = 0xE8E9B8;
constexpr uintptr_t kRvaCountVar = 0xEBAA28;

/// Whether the game's update may walk the slots: both blocks are still allocated.
inline bool Admit(const void* list, const void* verts) { return list && verts; }

}  // namespace wake

// =====================================================================================
// +0x1FED9E  -  LIST BOX LINE NAMING AN ITEM IT DOES NOT HAVE
//
// The game's list box keeps, at [obj+0x88]:
//   +0x4C4  bytes of text used       +0x4E0  text buffer: per item, a dword length + the string
//   +0x4C8  item count               +0x4F0  int offs[item]  (where item i's text starts)
//   +0x4F8  line count               +0x500  int line[l]     (which item line l shows)
// Every writer keeps line[l] < item count and offs[i] < text used (add 0x1FD010, clear 0x1FD2B0,
// reset 0x1FD51E, filter 0x1FD690). The draw at 0x1FEC90 trusts both:
//   0x1FED8C  mov rax, [rbx+0x500]
//   0x1FED93  movsxd rdx, [rax+rbp]              ; item = line[l]
//   0x1FED97  mov rax, [rbx+0x4F0]
//   0x1FED9E  mov r8d, [rax+rdx*4]               ; <- the fault: offs[item]
// thomas4f's dump has item = 0x3AEC0280 (985 million) at the end of an online race, so line[]
// held something no writer puts there. One sample; what wrote it is not established. The add
// grows both arrays with realloc, so a list filled on one thread while another draws it would
// read a freed block - consistent with the dump, not proven by it.
//
// The guard: the 22 bytes at 0x1FED8C jump (MinHook, threads frozen while it writes) to a stub
// that does the same two reads with two checks - item < item count, offs[item] < text used -
// and continues at 0x1FEDA2 when both hold. Otherwise it counts the line and goes to 0x1FEE30,
// the game's own "this line is above the scroll position" path: that line is not drawn this
// frame and the loop moves on. The frame tick logs the count.
// =====================================================================================
namespace listbox {

constexpr uintptr_t kRvaDraw  = 0x1FEC90;
constexpr uintptr_t kRvaSite  = 0x1FED8C;   // hooked here
constexpr uintptr_t kRvaBack  = 0x1FEDA2;   // the instruction after the two reads
constexpr uintptr_t kRvaSkip  = 0x1FEE30;   // inc edi: the next line, nothing drawn
constexpr uintptr_t kRvaFault = 0x1FED9E;

constexpr int32_t kTextUsed  = 0x4C4;
constexpr int32_t kItemCount = 0x4C8;
constexpr int32_t kOffs      = 0x4F0;
constexpr int32_t kLines     = 0x500;

// 0x1FED74 .. 0x1FEDA2: the loop head (rows left, scroll position) and the two reads.
constexpr uintptr_t kRvaWin = 0x1FED74;
constexpr size_t    kWinLen = 46;
constexpr uint8_t   kWin[kWinLen] = {
    0x3b, 0xb3, 0xd0, 0x03, 0x00, 0x00,                // cmp esi, [rbx+0x3D0] (rows)
    0x0f, 0x8d, 0xc2, 0x00, 0x00, 0x00,                // jge 0x1FEE42
    0x3b, 0xbb, 0xcc, 0x04, 0x00, 0x00,                // cmp edi, [rbx+0x4CC] (scroll)
    0x0f, 0x8c, 0xa4, 0x00, 0x00, 0x00,                // jl  0x1FEE30 (skip)
    0x48, 0x8b, 0x83, 0x00, 0x05, 0x00, 0x00,          // 0x1FED8C mov rax, [rbx+0x500]
    0x48, 0x63, 0x14, 0x28,                            //          movsxd rdx, [rax+rbp]
    0x48, 0x8b, 0x83, 0xf0, 0x04, 0x00, 0x00,          //          mov rax, [rbx+0x4F0]
    0x44, 0x8b, 0x04, 0x90,                            // 0x1FED9E mov r8d, [rax+rdx*4]
};
constexpr size_t kSiteInWin = kRvaSite - kRvaWin;      // 24
constexpr size_t kSkipJlAt  = 18;                       // the jl to kRvaSkip
// 0x1FEE2E .. 0x1FEE3C: the end of a drawn line (inc esi) and the skip target (inc edi ...).
constexpr uintptr_t kRvaTail = 0x1FEE2E;
constexpr uint8_t   kTail[14] = {0xff, 0xc6, 0xff, 0xc7, 0x48, 0x83, 0xc5, 0x04,
                                 0x3b, 0xbb, 0xf8, 0x04, 0x00, 0x00};
// 0x1FD100 .. 0x1FD121: the add, which proves the two bounds: offs[count] = used, and the
// arrays it writes.
constexpr uintptr_t kRvaAdd = 0x1FD100;
constexpr size_t    kAddLen = 33;
constexpr uint8_t   kAdd[kAddLen] = {
    0x48, 0x63, 0x93, 0xc8, 0x04, 0x00, 0x00,          // movsxd rdx, [rbx+0x4C8] (count)
    0x8b, 0x83, 0xc4, 0x04, 0x00, 0x00,                // mov eax, [rbx+0x4C4]    (used)
    0x48, 0x8b, 0x8b, 0xf0, 0x04, 0x00, 0x00,          // mov rcx, [rbx+0x4F0]
    0x48, 0x8b, 0x6c, 0x24, 0x38,
    0x48, 0x8b, 0x74, 0x24, 0x40,
    0x89, 0x04, 0x91,                                  // offs[count] = used
};

// The stub page: code, then what the frame tick reads.
constexpr size_t kCountOff = 0x68;   // uint32, lines skipped
constexpr size_t kItemOff  = 0x6C;   // uint32, the last bad item index
constexpr size_t kTextOff  = 0x70;   // uint32, its text offset (0 when the item was the bad part)
constexpr size_t kObjOff   = 0x78;   // uint64, the list box
constexpr size_t kPageBytes = 0x80;
constexpr size_t kStubLen  = 97;

// Offsets inside the stub, for the test.
constexpr size_t kJae1At = 20, kJae2At = 40, kBackJmpAt = 42, kBadAt = 56, kSkipJmpAt = 83;

/// The stub (runs at `stubVa`). Same reads as the game, two bounds checks, then back to
/// `backVa` or to `skipVa`. Clobbers rax, rdx, r8 and the flags, as the original does (r8 and
/// the flags are written by the game's next instructions before anything reads them).
inline size_t BuildStub(uint8_t* s, uintptr_t stubVa, uint64_t backVa, uint64_t skipVa) {
    size_t o = 0;
    auto b = [&](uint8_t x) { s[o++] = x; };
    auto d32 = [&](int32_t v) { for (int i = 0; i < 4; ++i) b((uint8_t)((uint32_t)v >> (8 * i))); };
    auto q64 = [&](uint64_t v) { for (int i = 0; i < 8; ++i) b((uint8_t)(v >> (8 * i))); };
    auto rip = [&](size_t end, size_t target) {   // disp32 for a field at `target` in the page
        d32((int32_t)((int64_t)(stubVa + target) - (int64_t)(stubVa + end)));
    };
    b(0x45); b(0x33); b(0xc0);                              // xor r8d, r8d (for the log)
    b(0x48); b(0x8b); b(0x83); d32(kLines);                 // mov rax, [rbx+0x500]
    b(0x48); b(0x63); b(0x14); b(0x28);                     // movsxd rdx, [rax+rbp]
    b(0x3b); b(0x93); d32(kItemCount);                      // cmp edx, [rbx+0x4C8]
    b(0x73); b((uint8_t)(kBadAt - (kJae1At + 2)));          // jae bad (negative is huge)
    b(0x48); b(0x8b); b(0x83); d32(kOffs);                  // mov rax, [rbx+0x4F0]
    b(0x44); b(0x8b); b(0x04); b(0x90);                     // mov r8d, [rax+rdx*4]
    b(0x44); b(0x3b); b(0x83); d32(kTextUsed);              // cmp r8d, [rbx+0x4C4]
    b(0x73); b((uint8_t)(kBadAt - (kJae2At + 2)));          // jae bad
    b(0xff); b(0x25); d32(0); q64(backVa);                  // jmp [rip] -> back
    // bad:
    b(0xf0); b(0xff); b(0x05); rip(o + 4, kCountOff);       // lock inc dword [count]
    b(0x89); b(0x15); rip(o + 4, kItemOff);                 // mov [item], edx
    b(0x44); b(0x89); b(0x05); rip(o + 4, kTextOff);        // mov [text], r8d
    b(0x48); b(0x89); b(0x1d); rip(o + 4, kObjOff);         // mov [obj], rbx
    b(0xff); b(0x25); d32(0); q64(skipVa);                  // jmp [rip] -> skip
    return o;
}

/// What the stub decides, in C: the text offset to draw, or -1 for "skip this line".
inline int64_t Decide(const int32_t* line, const int32_t* offs, int32_t l,
                      int32_t itemCount, int32_t textUsed) {
    const int32_t item = line[l];
    if ((uint32_t)item >= (uint32_t)itemCount) return -1;
    const int32_t off = offs[item];
    if ((uint32_t)off >= (uint32_t)textUsed) return -1;
    return off;
}

}  // namespace listbox

// =====================================================================================
// +0x2A42F0  -  A STACK POINTER PASSED THROUGH 32 BITS
//
// The game routes ~970 calls through one varargs dispatcher, 0x120CC0(job, ...). It spills rdx
// to its home slot, [rsp+0x2D8] once its frame is set up, and each job reads its arguments
// from there. Job 863 (0x35F) has one caller, 0x6C7BA, which passes `lea rdx, [rsp+0x20]` - a
// pointer to a { count, int[8] } on its stack - and the job hands it to 0x2A42F0, which reads
// it with `mov edx, [rcx]`. But the job loads it as an int:
//   0x127109  lea rax, [rsp+0x2D8]
//   0x127111  mov ecx, [rax]                     ; 8B 08 - its neighbours have 48 8B 08
//   0x127113  call 0x2A42F0
// When Windows puts the main thread's stack below 4 GB the top half is zero and nothing goes
// wrong. When it lands above (high-entropy ASLR, some launches), the read at +0x2A42F0 goes to
// the low half alone, unmapped memory. thomas4f's dump shows exactly that: rcx = 0x64BFD130,
// the real pointer 0x000000D364BFD130, rsp 0x000000D364BFCE38.
//
// The fix is the 64-bit load the job meant. 0x127109 jumps (MinHook, threads frozen) to a stub
// that does `mov rcx, [rsp+0x2D8]`, sets rax as the lea did, and goes on to the call. On a
// launch with the stack below 4 GB the value is bit-for-bit what the game passed before. A
// launch above 4 GB is counted (the pointer had a top half), and the frame tick logs it once.
// This is the only fix in this file that changes a value the game sees, and it changes it from
// a wrong pointer to the right one.
// =====================================================================================
namespace ptr64 {

constexpr uintptr_t kRvaSite   = 0x127109;   // hooked here
constexpr uintptr_t kRvaBack   = 0x127113;   // the call
constexpr uintptr_t kRvaCallee = 0x2A42F0;
constexpr uintptr_t kRvaFault  = 0x2A42F0;
constexpr int       kJob       = 863;
constexpr int32_t   kSlot      = 0x2D8;      // rdx's home, from the job's frame

// 0x127109 .. 0x127118: the job's whole body before its jmp to the dispatcher's exit.
constexpr size_t  kSiteLen = 15;
constexpr uint8_t kSite[kSiteLen] = {
    0x48, 0x8d, 0x84, 0x24, 0xd8, 0x02, 0x00, 0x00,    // lea rax, [rsp+0x2D8]
    0x8b, 0x08,                                        // mov ecx, [rax]   <- 32 bits
    0xe8, 0xd8, 0xd1, 0x17, 0x00,                      // call 0x2A42F0
};
constexpr size_t kCallInSite = 10;
// 0x2A42F0: it dereferences what it is given.
constexpr uint8_t kCallee[7] = {0x8b, 0x11, 0xb8, 0x08, 0x00, 0x00, 0x00};   // mov edx,[rcx]; mov eax,8
// 0x120CC0 .. 0x120CE5: the dispatcher's prologue. rdx goes to [entry rsp + 0x10]; 8 pushes and
// sub rsp, 0x288 later that is [rsp + 0x2D8].
constexpr uintptr_t kRvaDispatch = 0x120CC0;
constexpr size_t    kDispatchLen = 37;
constexpr uint8_t   kDispatch[kDispatchLen] = {
    0x48, 0x8b, 0xc4,                                  // mov rax, rsp
    0x89, 0x48, 0x08,                                  // mov [rax+8], ecx
    0x48, 0x89, 0x50, 0x10,                            // mov [rax+0x10], rdx  (all 64 bits)
    0x4c, 0x89, 0x40, 0x18,
    0x4c, 0x89, 0x48, 0x20,
    0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,   // 8 pushes
    0x48, 0x81, 0xec, 0x88, 0x02, 0x00, 0x00,          // sub rsp, 0x288
};
// The jump table the dispatcher indexes (`mov eax, [r9 + rax*4 + 0x1280B0]`, r9 = image base).
constexpr uintptr_t kRvaJumpTable = 0x1280B0;

// The stub page.
constexpr size_t kCountOff  = 0x40;   // uint32, calls whose pointer had a top half
constexpr size_t kLastOff   = 0x48;   // uint64, the last such pointer
constexpr size_t kPageBytes = 0x50;
constexpr size_t kStubLen   = 53;
constexpr size_t kJzAt = 15, kLeaAt = 31, kJmpAt = 39;

/// The stub (runs at `stubVa`): the 64-bit load, a count when the top half is set, the lea the
/// game did, then on to `backVa` (the call). Clobbers rax and the flags, which 0x2A42F0 sets
/// before it reads them.
inline size_t BuildStub(uint8_t* s, uintptr_t stubVa, uint64_t backVa) {
    size_t o = 0;
    auto b = [&](uint8_t x) { s[o++] = x; };
    auto d32 = [&](int32_t v) { for (int i = 0; i < 4; ++i) b((uint8_t)((uint32_t)v >> (8 * i))); };
    auto q64 = [&](uint64_t v) { for (int i = 0; i < 8; ++i) b((uint8_t)(v >> (8 * i))); };
    auto rip = [&](size_t end, size_t target) {
        d32((int32_t)((int64_t)(stubVa + target) - (int64_t)(stubVa + end)));
    };
    b(0x48); b(0x8b); b(0x8c); b(0x24); d32(kSlot);         // mov rcx, [rsp+0x2D8]
    b(0x48); b(0x89); b(0xc8);                              // mov rax, rcx
    b(0x48); b(0xc1); b(0xe8); b(0x20);                     // shr rax, 32
    b(0x74); b((uint8_t)(kLeaAt - (kJzAt + 2)));            // jz lea (below 4 GB)
    b(0xf0); b(0xff); b(0x05); rip(o + 4, kCountOff);       // lock inc dword [count]
    b(0x48); b(0x89); b(0x0d); rip(o + 4, kLastOff);        // mov [last], rcx
    b(0x48); b(0x8d); b(0x84); b(0x24); d32(kSlot);         // lea rax, [rsp+0x2D8]
    b(0xff); b(0x25); d32(0); q64(backVa);                  // jmp [rip] -> the call
    return o;
}

/// What the callee is handed: stock passes the low half, the fix the whole pointer.
inline uint64_t Passed(uint64_t slot, bool fixed) { return fixed ? slot : (uint64_t)(uint32_t)slot; }
/// Whether stock would have handed over a different (and unmapped) pointer.
inline bool StockWouldTruncate(uint64_t slot) { return (slot >> 32) != 0; }

}  // namespace ptr64

}  // namespace sessionguards
