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

// ---- content loading (RE'd from IDA - see CHANGELOG 2026-07-05) ---------------
// KEY: MX Bikes reads content LIVE from disk every scan (no persistent pkz mount).
// A newly-dropped .pkz is already visible to a fresh scan; the game just runs the
// content scan ONCE at startup and the menus cache it. So "reload" = re-run the
// game's own content-load, NOT mount/replay anything.
//
// fcn.1400ef210 : the boot content-load + app-init routine (called once from
//   WinMain). Reads the mods folder, then for each content type clears its list
//   and rescans BOTH the game dir and the mods folder. Signature:
//     int64 __fastcall(int mode, int64, int64, int64)
//   WARNING: it also re-inits Steam/input/sound and transitions to the UI, so
//   calling it wholesale mid-game is heavy (a soft restart to the menu). Used as
//   the reload target for now; the surgical per-category loaders are safer (TODO).
constexpr uintptr_t RVA_CONTENT_INIT  = 0xef210;   // fcn.1400ef210

// fcn.140158be0 : generic VFS directory walker (out_status, dir, ext, out_buf).
//   Reads the filesystem live (findfirst + fopen each .pkz). We hook it only to
//   observe the boot scans ([capture]); it is NOT a reload target.
constexpr uintptr_t RVA_SCAN_FOLDER   = 0x158be0;

// Reference-only RVAs kept for the opt-in --probe-mount diagnostic (see frostmod.cpp).
// These were part of the abandoned "mount the new .pkz" reload approach; the probe
// still uses them to observe how the game opens archives. NOT used by the reload.
constexpr uintptr_t RVA_REGISTRY_RESET = 0x159340;  // fcn.140159340 (restrict registry)
constexpr uintptr_t RVA_MOUNT_ONE_PKZ  = 0x15a9e0;  // fcn.14015a9e0 (6-arg .pkz iterator)
constexpr uintptr_t RVA_VFS_LOOKUP     = 0x157920;  // fcn.140157920
constexpr uintptr_t RVA_REG_BASE       = 0x396760;  // RegEntry* (0x20c each)
constexpr uintptr_t RVA_REG_COUNT      = 0x396754;  // int32 count

// Per-category content loaders live inside fcn.1400ef210 as repeated
//   { clear list globals; loader(gameDir); loader(modsDir); } blocks. The TRACK
//   list is qword_14109de98 (stride 1220, count dword_140f43298). RVA of the track
//   loader (the sub that writes dword_140f43298) is still to be pinned - then a
//   surgical reload can rebuild just the track list without the full re-init.
constexpr uintptr_t RVA_TRACK_LOADER  = 0x000000;  // TODO: xref writer of 0x140f43298
constexpr uintptr_t RVA_TRACK_COUNT   = 0xf43298;  // int32 track count
constexpr uintptr_t RVA_TRACK_LIST    = 0x1109de98;// track array (stride 1220)
constexpr int       TRACK_STRIDE      = 1220;      // 0x4C4 bytes per track entry
// Track entry field offsets (folder name / display name) - TBD from the F9 [tracks]
// dump + RE. Filled in once the entry layout is confirmed at runtime.
constexpr int TRK_FOLDER = -1, TRK_NAME = -1;      // -1 = not yet pinned

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
//       ...                // +0x84  2-byte field (port/id); precedes the name
//       char name[...];    // +0x86  display name  (CONFIRMED by the read-only dump)
//       u32  maxplayers;   // +0xC8  capacity       (32/42/... - the CAP)
//       u32  players;      // +0xCC  current players (==0 => empty; the game's own
//                          //        hide-empty cmp at 0x0ABAB6 tests THIS field)
//       u32  ping;         // +0xDC  (0xFFFFFFFF => "---"; but UNRESOLVED at list-build
//                          //        time -> "---" for EVERY server here, so useless
//                          //        as a build-time filter signal. Name is the signal.)
//       u32  type_status;  // +0x100 category/status enum
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
// THE row is created by the FIRST setCellText (msg 0x11B) at 0x0ABA03 - a cell-write
// auto-extends the widget, there is no separate addRow. So to hide a row we must skip
// BEFORE 0x0ABA03. The game does exactly this with its name-search filter: strstr miss
// -> jmp 0x0ACE68 at 0x0AB9D3. We mirror it by hooking the LOOP TOP (0x0AB960) and
// jmp'ing to 0x0ACE68 for spam rows, so the cells are never written = the row never
// appears. (Hooking 0x0ABAB6/hide-empty was too LATE - the row was already committed.)
constexpr uintptr_t RVA_SB_LAN_CMD        = 0x0AB530; // clear+build+populate (LAN)
constexpr uintptr_t RVA_SB_WORLD_CMD      = 0x0AA260; // string dispatch (world)
constexpr uintptr_t RVA_SB_POPULATE_LOOP  = 0x0AB960; // per-server loop TOP - our hook site
constexpr uintptr_t RVA_SB_ROW_CREATE     = 0x0ABA03; // first setCellText (row is committed here)
constexpr uintptr_t RVA_SB_ROW_SKIP_TGT   = 0x0ACE68; // jump here to SKIP a row (row never created)
constexpr uintptr_t RVA_SB_HIDE_EMPTY_BR  = 0x0ABAB6; // game's hide-empty cmp (AFTER row create; unused now)
constexpr uintptr_t RVA_SB_BUILD_CLEAR    = 0x0AB59C; // ListBegin + zero counts + ListClear

// bytes at the loop top 0x0AB960: cmp byte [rip+disp32], r12b. We verify the 3-byte
// opcode+modrm (44 38 25); the disp32 that follows is RIP-relative and build-specific,
// so it is intentionally NOT part of the check.
constexpr unsigned char SB_POPULATE_LOOP_BYTES[] = {0x44, 0x38, 0x25};
constexpr uintptr_t RVA_SB_REFRESHLIST    = 0x0AB6A8; // ID_REFRESHLIST branch (LAN)

// SB_Entry (working copy) field offsets. Confirmed from the populate loop disasm
// AND the runtime read-only dump ([srv.hex]):
//   entry = [rsp + rdi] (stack buffer, rdi = per-row offset). NAME is at +0x86 (the
//   dump showed the name text starting there on every row; the 2 bytes at +0x84 are
//   a binary field that only *looks* like ASCII sometimes). +0xC8 is the CAPACITY
//   (max) and +0xCC the CURRENT player count (0 => empty; the game's own hide-empty
//   cmp at 0x0ABAB6 tests +0xCC) - earlier these two were swapped. +0xDC is the
//   ping, but it is unresolved at build time (== "---" for everyone) so we don't
//   filter on it.
constexpr int SBE_STRIDE = 0x1D8, SBE_NAME = 0x86, SBE_MAXPLAYERS = 0xC8,
              SBE_PLAYERS = 0xCC, SBE_PING = 0xDC, SBE_TYPE = 0x100;
constexpr uint32_t SBE_PING_UNJOINABLE = 0xFFFFFFFFu; // ping value shown as "---"

// exact bytes at RVA_SB_HIDE_EMPTY_BR: cmp [rsp+rdi+0xCC], r12d (8 bytes). The
// filter hook verifies these before splicing, and jz's target is the skip label.
constexpr unsigned char SB_HIDE_EMPTY_BYTES[] =
    {0x44, 0x39, 0xA4, 0x3C, 0xCC, 0x00, 0x00, 0x00};

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
