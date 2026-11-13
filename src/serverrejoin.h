// serverrejoin.h - FrostServer: a rider who leaves is taken off everybody's screen.
//
// THE BUG, as riders see it. Someone leaves the server and joins again, and from then on two of
// them are on the track: the new one, and a frozen copy where they left. Every client keeps the
// copy until it leaves the server itself.
//
// WHY (mxbikes/SYMBOLS.md, docs/protocol). A client draws a rider from its entity record. The
// only message that removes one is CONNECTION_INFO (0x19): its handler frees the record. The
// server sends CONNECTION_INFO from exactly one function, server_entity_remove at 0x298A20,
// which also frees the server's own record, and that function has one caller in the image:
// the job dispatcher, at 0x126ACE. It is not on the disconnect path. Every departure goes through
// server_client_left at 0x298E20, which records the reason, raises server_event(7), and
// broadcasts DISCONNECTION_INFO (0x2D) - which clients answer by clearing their roster line
// and nothing else. So the rider's entity outlives the rider, on the server and on every
// client, and a rejoin under a new id makes a second one.
//
// THE FIX. A detour on server_client_left. After the game's own function has run, every server
// entity whose owner is the leaving client is removed with the game's own server_entity_remove,
// which broadcasts CONNECTION_INFO for it and frees the server's record - the same pair of
// effects as the game's own removal job. Native mxbserver does the same thing on disconnect
// (crates/mxbserver/src/native.rs, "DISCONNECTION_INFO ... CONNECTION_INFO").
//
// Server entity table: 50 records of 0x151A8 from 0x140A270EC: in-use flag at -4, entity id at
// +0, owning client id at +4 (on_entering_info 0x2A05C3 matches both). The client record's id is
// its first dword (server_client_left's own `mov edx, [rcx]`).
//
// No Win32 here, so tests/serverrejoin_test.cpp runs anywhere.
#pragma once
#include <cstddef>
#include <cstdint>

namespace serverrejoin {

constexpr uintptr_t kRvaClientLeft   = 0x298E20;   // int left(client*, reason, a, b)
constexpr uintptr_t kRvaEntityRemove = 0x298A20;   // int remove(entity id); 0 = removed
constexpr uintptr_t kRvaEntities     = 0xA270EC;   // record 0's entity id; flag at -4
constexpr size_t    kEntityStride    = 0x151A8;
constexpr int       kEntities        = 50;
constexpr size_t    kFlagAt = (size_t)-4, kIdAt = 0, kOwnerAt = 4;

// 0x298E20: prologue, cookie, the three stores of the reason, and `mov edx, [rcx]` (client id).
constexpr size_t  kLeftLen = 0x3D;
constexpr uint8_t kLeft[kLeftLen] = {
    0x48, 0x89, 0x5c, 0x24, 0x18, 0x57, 0x48, 0x81, 0xec, 0xd0, 0x05, 0x00, 0x00, 0x48, 0x8b, 0x05,
    0x6c, 0x9a, 0x0f, 0x00, 0x48, 0x33, 0xc4, 0x48, 0x89, 0x84, 0x24, 0xc0, 0x05, 0x00, 0x00, 0x44,
    0x89, 0x89, 0x50, 0x12, 0x00, 0x00, 0x44, 0x89, 0x81, 0x4c, 0x12, 0x00, 0x00, 0x89, 0x91, 0x48,
    0x12, 0x00, 0x00, 0x45, 0x33, 0xc9, 0x8b, 0xfa, 0x44, 0x8b, 0xc2, 0x8b, 0x11,
};
// 0x298A20: prologue, cookie, the server-running test; and at 0x298A79 the walk of the entity
// table by id (lea of the table and its end, flag test at -4, id compare, stride add).
constexpr size_t  kRemoveLen = 0x22;
constexpr uint8_t kRemove[kRemoveLen] = {
    0x48, 0x81, 0xec, 0xd8, 0x05, 0x00, 0x00, 0x48, 0x8b, 0x05, 0x72, 0x9e, 0x0f, 0x00, 0x48, 0x33,
    0xc4, 0x48, 0x89, 0x84, 0x24, 0xc0, 0x05, 0x00, 0x00, 0x83, 0x3d, 0x88, 0x58, 0x78, 0x00, 0x00,
    0x75, 0x1d,
};
constexpr uintptr_t kRvaRemoveWalk = 0x298A79;
constexpr size_t    kRemoveWalkLen = 0x2E;
constexpr uint8_t   kRemoveWalk[kRemoveWalkLen] = {
    0x48, 0x8d, 0x05, 0x6c, 0xe6, 0x78, 0x00,         // lea rax, [entities]
    0x48, 0x8d, 0x15, 0x35, 0xd9, 0xba, 0x00,         // lea rdx, [end]
    0x8b, 0xde, 0x48, 0x8b, 0xfe, 0x0f, 0x1f, 0x40, 0x00,
    0x39, 0x70, 0xfc, 0x74, 0x04,                     // cmp [rax-4], esi (in use?)
    0x39, 0x08, 0x74, 0x45,                           // cmp [rax], ecx   (this id?)
    0x48, 0x05, 0xa8, 0x51, 0x01, 0x00,               // add rax, 0x151A8
    0xff, 0xc3, 0x48, 0xff, 0xc7, 0x48, 0x3b, 0xc2,
};

/// The entity table the remove function walks, decoded from its own lea (instruction at
/// kRvaRemoveWalk, 7 bytes, disp32 at 3).
inline uintptr_t DecodeTable(const uint8_t* walk) {
    int32_t d = 0;
    for (int i = 0; i < 4; ++i) d |= (int32_t)((uint32_t)walk[3 + i] << (8 * i));
    return (uintptr_t)((int64_t)(kRvaRemoveWalk + 7) + d);
}

/// The entity ids owned by `client`, from a table read through `at(index, &flag, &id, &owner)`.
/// At most `max` written to `out`; returns how many.
template <class At>
inline int OwnedBy(int32_t client, At at, int32_t* out, int max) {
    int n = 0;
    for (int i = 0; i < kEntities && n < max; ++i) {
        int32_t flag = 0, id = 0, owner = 0;
        if (!at(i, &flag, &id, &owner)) continue;
        if (flag != 0 && owner == client) out[n++] = id;
    }
    return n;
}

}  // namespace serverrejoin
