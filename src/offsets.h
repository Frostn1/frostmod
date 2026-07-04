// offsets.h - MX Bikes function RVAs recovered by static RE (base 0x140000000).
//
// These are RVAs (offset from the module base). At runtime:
//     absolute = (uintptr_t)GetModuleHandleA("mxbikes.exe") + RVA
//
// The shipping exe is Steam-DRM (SteamStub) wrapped, but SteamStub decrypts
// .text in place at the same virtual addresses, so these RVAs match the running
// process. If a game update shifts them, use the AOB signature below instead.
#pragma once
#include <cstdint>

namespace mxb {

// ---- content / virtual-filesystem functions ----------------------------------
// fcn.140158be0 : scan a folder, recurse subdirs, glob *.pkz, mount each.
//   signature: int64 __fastcall(void* out_status, char* dir_path,
//                               char* extra_ext, void* out_buf_0x108)
constexpr uintptr_t RVA_SCAN_FOLDER   = 0x158be0;

// fcn.140159340 : reset+rebuild the content-directory registry from a path list.
//   frees [0x140396760], zeroes count [0x140396754], reallocs, rebuilds.
//   signature: int64 __fastcall(void* path_list, void* arg2)
constexpr uintptr_t RVA_REGISTRY_RESET = 0x159340;

// (reference only - not called directly by this mod)
constexpr uintptr_t RVA_MOUNT_ONE_PKZ = 0x15a9e0;  // fcn.14015a9e0
constexpr uintptr_t RVA_VFS_LOOKUP    = 0x157920;  // fcn.140157920

// ---- global VFS registry (for inspection / advanced strategies) --------------
constexpr uintptr_t RVA_REG_BASE      = 0x396760;  // RegEntry* (0x20c each)
constexpr uintptr_t RVA_REG_COUNT     = 0x396754;  // int32 count

// AOB signature for the scanner prologue (fallback if RVA drifts across updates).
// 40 53 56 57 41 54 41 55 41 56 48 81 EC F8 07 00 00 48 8B 05 ?? ?? ?? ??
constexpr char SIG_SCAN_FOLDER[] =
    "\x40\x53\x56\x57\x41\x54\x41\x55\x41\x56\x48\x81\xEC\xF8\x07\x00\x00\x48\x8B\x05";
constexpr char SIG_SCAN_FOLDER_MASK[] = "xxxxxxxxxxxxxxxxxxxx";

} // namespace mxb
