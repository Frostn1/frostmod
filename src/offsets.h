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
// fcn.140158be0 : GENERIC virtual-filesystem DIRECTORY WALKER, NOT the pkz loader.
//   Runtime capture shows it is called as (out_status, dir_path, ext, out_buf)
//   with ext='/' (list subdirs), 'cfg', 'pnt', ... but NEVER 'pkz'. By the time it
//   runs the .pkz are already mounted and appear as virtual dirs it walks. So
//   replaying/calling it can never MOUNT a newly added .pkz - that was the wrong
//   function. Kept only for reference / diagnostics.
constexpr uintptr_t RVA_SCAN_FOLDER   = 0x158be0;

// fcn.140159340 : reset+rebuild the content-directory registry from a path list.
//   frees [0x140396760], zeroes count [0x140396754], reallocs, rebuilds.
//   signature: int64 __fastcall(void* path_list, void* arg2)
//   NOTE: not called at startup (never captured), so we can't replay it.
constexpr uintptr_t RVA_REGISTRY_RESET = 0x159340;

// fcn.14015a9e0 : the actual "MOUNT ONE .pkz into the VFS" function - this is what
//   we need to call for a newly-added file to make it live. Signature UNKNOWN
//   (probably (context, char* pkz_path[, flags])). frostmod.exe --probe-mount hooks
//   it to log its args at startup and reveal which arg is the path. Then a reload
//   can mount the new pkz + rebuild the registry.
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

// ============ MX Bikes networking / server browser (RE'd; base 0x140000000) ====
// The master server (master.mx-bikes.com, UDP 54200 = 0xD3B8) sends the list. The
// browser builds "working copy" entries (SB_Entry, stride 0x1D8) and a populate
// loop emits one row each. To hide spam/"ghost" servers we splice the populate
// loop: read the entry, and if serverfilter says hide, jump to the row-skip target
// (which keeps the game's counts consistent).
//
//   struct SB_Entry {                     // stride 0x1D8
//       char name[~0x80];  // +0x00  display name (col0)
//       u32  players;      // +0xC8
//       u32  maxplayers;   // +0xCC   (==0 => empty)
//       u32  ping;         // +0xD8   (0xFFFFFFFF => "---" == UNJOINABLE / ghost)
//       u32  type_status;  // +0x100  category/status enum
//   };
//   struct SB_Connect { u64 host_lo; u64 host_hi; u16 port(+0x10); u8 flag(+0x12); };
//     (filled on JOIN, msg 0x385 -> RVA_SB_CONNECT_TARGET; flag = password/lock bit)

// ---- transport (UDP, ws2_32) ----
constexpr uintptr_t RVA_NET_RECVFROM_W   = 0x284B10; // recvfrom wrapper(sock rcx, buf rdx, len r8)
constexpr uintptr_t RVA_NET_SENDTO_W     = 0x284BD0; // sendto wrapper
constexpr uintptr_t RVA_NET_RESOLVE_HOST = 0x2854E0; // host:port -> getaddrinfo
constexpr uintptr_t RVA_NET_DISPATCH     = 0x284450; // recv + per-peer dispatch
constexpr uintptr_t RVA_NET_PEER_BASE    = 0x3993B0; // peer/conn table base (stride 0x5B4)
constexpr uintptr_t RVA_NET_PEER_COUNT   = 0x3993AC; // peer count (cap 10)

// ---- master protocol (opcode/text) - the CLEAN filter point ----
// 0x2A10E0 handles master opcodes; its HOSTED case writes the server-list reply
// as TEXT into the blob at 0x9E3AE0. Hooking 0x2A10E0 (clean prologue + AOB) and
// editing that blob before the browser parses it lets us drop spam servers with
// no code cave. Blob is records via read_str (NUL/\n) + read_u32.
constexpr uintptr_t RVA_MP_MSG_HANDLER   = 0x2A10E0; // client opcode handler; HOSTED = server-list reply
constexpr uintptr_t RVA_MP_REFRESH_DRV   = 0x2A6890; // sends GETLIST to masters, 3000ms refresh timer
constexpr uintptr_t RVA_MP_ENDPOINT_ADD  = 0x2A8330; // append MASTER endpoint (not game servers), cap 10
constexpr uintptr_t RVA_MP_ENDPOINT_BASE = 0x597560; // master endpoint array (stride 0x28, cap 10)
constexpr uintptr_t RVA_MP_ENDPOINT_CNT  = 0x3D8014; // master endpoint count
constexpr uintptr_t RVA_MP_LIST_BLOB     = 0x9E3AE0; // HOSTED payload text buffer (written by 0x2A10E0)
constexpr uintptr_t RVA_MP_STATE         = 0x9D7AA4; // 0 idle /1 requesting /2 connected /3 list-complete
constexpr uintptr_t RVA_MP_READ_INIT     = 0x2835A0; // reader: init
constexpr uintptr_t RVA_MP_READ_U32      = 0x283490; // reader: read_u32
constexpr uintptr_t RVA_MP_READ_STR      = 0x283800; // reader: read_str (inline NUL/\n-terminated)

// ---- server browser UI state (.data) ----
constexpr uintptr_t RVA_SB_CONNECTED_FLAG = 0x4C8F20; // master/list ready (0/1)
constexpr uintptr_t RVA_SB_FILTER_FLAGS   = 0x4C8F44; // bit0 hide-empty, bit1 hide-full
constexpr uintptr_t RVA_SB_DISPLAY_COUNT  = 0x4C8F48; // filtered/displayed row count
constexpr uintptr_t RVA_SB_INFO_INDEX     = 0x4C8F54; // SERVERINFO index (-1)
constexpr uintptr_t RVA_SB_RAW_COUNT      = 0x4C8F58; // raw server count (loop bound)
constexpr uintptr_t RVA_SB_NAME_FILTER    = 0x4C8F60; // uppercased name-filter string
constexpr uintptr_t RVA_SB_SELECTED_INDEX = 0x4C8FC8; // selected row
constexpr uintptr_t RVA_SB_CONNECT_TARGET = 0xE53DE0; // JOIN connect struct

// ---- hook / patch points ----
constexpr uintptr_t RVA_SB_LAN_CMD        = 0x0AB530; // clear+build+populate (LAN)
constexpr uintptr_t RVA_SB_WORLD_CMD      = 0x0AA260; // string dispatch (world)
constexpr uintptr_t RVA_SB_POPULATE_LOOP  = 0x0AB960; // per-server emit loop (inside LAN_CMD)
constexpr uintptr_t RVA_SB_ROW_SKIP_TGT   = 0x0ACE68; // jump here to SKIP a row (counts stay sane)
constexpr uintptr_t RVA_SB_HIDE_EMPTY_BR  = 0x0ABAB6; // game's own skip branch (cmp maxplayers)
constexpr uintptr_t RVA_SB_BUILD_CLEAR    = 0x0AB59C; // ListBegin + zero counts + ListClear
constexpr uintptr_t RVA_SB_REFRESHLIST    = 0x0AB6A8; // ID_REFRESHLIST branch (LAN)

// SB_Entry (working copy) field offsets
constexpr int SBE_STRIDE = 0x1D8, SBE_NAME = 0x00, SBE_PLAYERS = 0xC8,
              SBE_MAXPLAYERS = 0xCC, SBE_PING = 0xD8, SBE_TYPE = 0x100;
constexpr uint32_t SBE_PING_UNJOINABLE = 0xFFFFFFFFu; // ping value shown as "---"

// AOB signatures (32-byte prologues; ?? = RIP/call-rel disp, wildcarded)
constexpr char SIG_SB_LAN_CMD[]  =
    "\x40\x53\x55\x56\x57\x41\x54\x41\x55\xB8\xB8\x17\x00\x00\xE8\x00\x00\x00\x00\x48\x2B\xE0\x48\x8B\x05\x00\x00\x00\x00\x48\x33\xC4";
constexpr char SIG_SB_LAN_CMD_MASK[]  = "xxxxxxxxxxxxxxx????xxxxxx????xxx";
constexpr char SIG_SB_WORLD_CMD[] =
    "\x40\x53\x55\x56\x57\x48\x81\xEC\xF8\x05\x00\x00\x48\x8B\x05\x00\x00\x00\x00\x48\x33\xC4\x48\x89\x84\x24\xD0\x05\x00\x00\x49\x8B";
constexpr char SIG_SB_WORLD_CMD_MASK[] = "xxxxxxxxxxxxxxx????xxxxxxxxxxxxxx";

// master opcode handler (writes the server-list blob) - the clean hook target
constexpr char SIG_MP_MSG_HANDLER[] =
    "\x40\x53\x56\x57\x48\x81\xEC\x20\x07\x00\x00\x48\x8B\x05\x00\x00\x00\x00\x48\x33\xC4\x48\x89\x84\x24\x10\x07\x00";
constexpr char SIG_MP_MSG_HANDLER_MASK[] = "xxxxxxxxxxxxxx????xxxxxxxxxxx";

} // namespace mxb
