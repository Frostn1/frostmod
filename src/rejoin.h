// rejoin.h - freeing a rider's entity when they disconnect.
//
// A rider who leaves a server keeps their entity record on everyone else's client: the
// disconnect message clears the 0x2E0 roster entry and nothing clears the 86,440-byte
// entity beside it. Teardown of the entity rides on a separate broadcast the game code
// has to ask for, and on a mid-session departure it does not. Rejoin and the client finds
// the surviving record by id first and writes the new session into it, so the previous
// session's model state is still in there - the two riders drawn as one.
//
// The patch repoints one call. The disconnect path ends in memset(roster_record, 0, 0x2E0);
// we send that call to a thunk that does the memset and then calls the game's own entity
// removal for the same id. Same function the game uses when it does tear an entity down,
// and a no-op when there is nothing to remove.
//
// No Win32 here, so tests/rejoin_test.cpp runs anywhere. frostmod.cpp does the memory work.
#pragma once
#include <cstdint>
#include <cstddef>

namespace rejoin {

// The tail of the DISCONNECTION_INFO handler, as RVAs from the module base. The window is
// guarded whole rather than just the call, because `call memset` is five common bytes and
// the surrounding instructions are what make this *that* memset.
constexpr uintptr_t kRvaWindow    = 0x2A5642;
constexpr size_t    kWindowLen    = 0x24;
constexpr size_t    kCallOffset   = 0x1C;      // the call within the window
constexpr size_t    kCallLen      = 5;
constexpr size_t    kCallDispOff  = 1;         // rel32 within the call
constexpr uintptr_t kRvaCall      = kRvaWindow + kCallOffset;

// Exactly what the window is in the build we read. A build guard, not decoration: these
// bytes stop matching if the handler moves or is re-emitted, and then we patch nothing.
constexpr uint8_t kWindowBytes[kWindowLen] = {
    0x48, 0x63, 0xcb,                               // movsxd rcx, ebx        (slot index)
    0x48, 0x8d, 0x3d, 0xf8, 0x39, 0x2f, 0x00,       // lea    rdi, [roster]
    0x48, 0x69, 0xc9, 0xe0, 0x02, 0x00, 0x00,       // imul   rcx, rcx, 0x2e0
    0x33, 0xd2,                                     // xor    edx, edx
    0x41, 0xb8, 0xe0, 0x02, 0x00, 0x00,             // mov    r8d, 0x2e0
    0x48, 0x03, 0xcf,                               // add    rcx, rdi
    0xe8, 0xdd, 0x5c, 0x00, 0x00,                   // call   memset          <- repointed
    0x33, 0xc0,                                     // xor    eax, eax
    0xe9,                                           // jmp    (handler exit)
};

// The game's entity removal: scans the 50-slot entity array for an in-use record with this
// id and zeroes it. Idempotent - no match, no work. Called with the id in ecx.
constexpr uintptr_t kRvaEntityRemove = 0x2A1FF0;
constexpr size_t    kEntityRemoveSigLen = 9;
constexpr uint8_t   kEntityRemoveSig[kEntityRemoveSigLen] = {
    0x48, 0x83, 0xec, 0x28, 0x33, 0xd2, 0x48, 0x8d, 0x05
};

// The thunk we build at runtime. rcx/rdx/r8 already hold the memset arguments and r12d
// still holds the disconnecting id - it is callee-saved, so it survives both calls.
//
//   sub rsp,0x28 / movabs rax,memset / call rax / mov ecx,r12d /
//   movabs rax,entity_remove / call rax / add rsp,0x28 / ret
constexpr size_t kThunkLen        = 36;
constexpr size_t kThunkMemsetImm  = 6;    // imm64 offsets within the thunk
constexpr size_t kThunkRemoveImm  = 21;

/// Write the thunk into `out` (kThunkLen bytes). Both targets are absolute VAs.
inline bool BuildThunk(uint8_t* out, uintptr_t memsetVa, uintptr_t entityRemoveVa) {
    if (!out || !memsetVa || !entityRemoveVa) return false;
    static const uint8_t tmpl[kThunkLen] = {
        0x48, 0x83, 0xec, 0x28,
        0x48, 0xb8, 0,0,0,0,0,0,0,0,
        0xff, 0xd0,
        0x44, 0x89, 0xe1,
        0x48, 0xb8, 0,0,0,0,0,0,0,0,
        0xff, 0xd0,
        0x48, 0x83, 0xc4, 0x28,
        0xc3,
    };
    for (size_t i = 0; i < kThunkLen; ++i) out[i] = tmpl[i];
    for (int i = 0; i < 8; ++i) {
        out[kThunkMemsetImm + i] = (uint8_t)((uint64_t)memsetVa       >> (8 * i));
        out[kThunkRemoveImm + i] = (uint8_t)((uint64_t)entityRemoveVa >> (8 * i));
    }
    return true;
}

/// Do these bytes look like the handler tail we mean to patch?
inline bool WindowMatches(const uint8_t* win) {
    if (!win) return false;
    for (size_t i = 0; i < kWindowLen; ++i)
        if (win[i] != kWindowBytes[i]) return false;
    return true;
}

/// Where the call at `callVa` currently goes, decoded from its own bytes.
inline uintptr_t CurrentTarget(uintptr_t callVa, const uint8_t* call) {
    if (!call || !callVa) return 0;
    int32_t disp = 0;
    for (int i = 0; i < 4; ++i)
        disp |= (int32_t)((uint32_t)call[kCallDispOff + i] << (8 * i));
    return callVa + kCallLen + (uintptr_t)(intptr_t)disp;
}

/// The rel32 that makes the call at `callVa` reach `targetVa`. False when it is out of
/// reach - a page allocated too far away must fail loudly rather than wrap.
inline bool PlanDisp(uintptr_t callVa, uintptr_t targetVa, int32_t* outDisp) {
    if (!outDisp || !callVa || !targetVa) return false;
    const int64_t delta = (int64_t)targetVa - (int64_t)(callVa + kCallLen);
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    *outDisp = (int32_t)delta;
    return true;
}

}  // namespace rejoin
