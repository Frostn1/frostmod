// offsets.h - MX Bikes function RVAs recovered by static RE (base 0x140000000).
//
// These are RVAs (offset from the module base). At runtime:
//     absolute = (uintptr_t)GetModuleHandleA("mxbikes.exe") + RVA
//
// The shipping exe is Steam-DRM (SteamStub) wrapped, but SteamStub decrypts
// .text in place at the same virtual addresses, so these RVAs match the running
// process. If a game update shifts them, use the AOB signature below instead.
#pragma once
#include <cstddef>   // size_t — the session-settings sizes below; frostmod.cpp gets it from
                     // windows.h, but offsets_test.cpp includes this header on its own.
#include <cstdint>

/// One step of the surgical content reload: replay of a single content-list rebuild
/// lifted out of the game's boot init. Two shapes, mirroring what the game itself does:
///
///   dir=0  SC  self-contained. The loader clears its own list globals and scans both
///              the game dir and the mods dir. Called as `fn(0, 0)` — it ignores args.
///   dir=1  DIR the *caller* zeroes three list globals inline, then calls the loader
///              once per directory: `fn(&"")` for the game dir, then `fn(&mods)`.
///
/// Which shape a given loader uses is a property of the build, not of the category —
/// MX Bikes inlines the per-dir dance into boot init for some categories, GP Bikes wraps
/// it inside the loader for all of them (so GP's table is entirely SC).
///
/// `what` names the content category the step rebuilds. It is logged immediately BEFORE
/// the step runs, and `Log()` reopens and closes the file per line, so when a step takes
/// the game down its name is the last thing in the log. That is the only way a crash
/// inside a replayed loader can be attributed: every step is SEH-guarded, so an ordinary
/// access violation is swallowed and the reload finishes — the failures that actually
/// kill the process (heap corruption's fail-fast, a fault on another thread) leave
/// nothing behind but the last line written. Optional: null means "unlabelled".
struct RLStep { uint8_t dir; uintptr_t z1, z2, z3, rva; const char* what; };

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
constexpr uintptr_t RVA_TRACK_COUNT   = 0xf43298;  // int32 track count (dword_140f43298)
// qword_14109de98 -> RVA 0x109de98. It's a QWORD *pointer* to the heap track array
// (count*1220 is far too big to be inline), so deref it before indexing. (Was
// mistranscribed as 0x1109de98 - an extra digit - which read as unmapped memory.)
constexpr uintptr_t RVA_TRACK_LIST    = 0x109de98; // qword: pointer to track array
constexpr int       TRACK_STRIDE      = 1220;      // 0x4C4 bytes per track entry
// Track entry field offsets, confirmed from the F9 [tracks] dump (all 85 entries):
//   +0x00 folder/id, +0x20 DISPLAY name, +0x60 short name, +0xB0 preview image.
// +0x33C is the "resolver name" fcn.1400BB510 compares against the session config's
// 2nd name (to be confirmed at runtime; may differ from +0x20 display).
constexpr int TRK_FOLDER = 0x00, TRK_NAME = 0x20, TRK_SHORT = 0x60, TRK_IMAGE = 0xB0;
constexpr int TRK_RESOLVER_NAME = 0x33C;

// ---- localhost/testing "switch to this track" (RE'd) ---------------------------
// fcn.1400BB510 loads the configured track + enters the testing session. It takes NO
// args: it reads the session name config at 0xE4B540 (+0x00 folder, +0x20 2nd name),
// name-matches it across the track array to re-derive the selected index (0x4CA3D4),
// then calls the data loader fcn.140005A20(ecx=index). So to switch tracks we WRITE
// the name config (folder + entry+0x33C) and call 0xBB510 - setting the index alone
// won't survive. HEAVY (disk I/O + heap + UI) -> game thread only, from menu context.
constexpr uintptr_t RVA_TRK_LOAD_ENTER  = 0xBB510; // fcn.1400BB510: load track + enter session
constexpr uintptr_t RVA_TRK_DATA_LOADER = 0x5A20;  // fcn.140005A20(ecx=index): data loader it calls
constexpr uintptr_t RVA_TRK_SEL_INDEX   = 0x4CA3D4;// int32 selected index (re-derived from name cfg)
constexpr uintptr_t RVA_TRK_SESSION_CFG = 0xE4B540;// name cfg: +0x00 folder, +0x20 2nd name

// ---- in-garage BIKE switcher (RE'd; analog of the track switcher) --------------
// In-game bike list: a QWORD *pointer* to a heap array of bike entries (deref before
// indexing). count is int32. Entry +0x00 = folder/ID (the field the loader
// _stricmp-matches); +0x4C0 = the string used to build  bikes\<name>\<name>.cfg .
// The class ([data] cat, e.g. "Classic MX1 OEM") lives in the entry's data block;
// its exact offset is TBD — Stage A (--bikecap) dumps an entry to find it.
constexpr uintptr_t RVA_BIKE_LIST   = 0xF4EDE8;  // qword: pointer to bike array
constexpr uintptr_t RVA_BIKE_COUNT  = 0xF48218;  // int32 bike count
constexpr int       BIKE_STRIDE     = 0x4334;    // 17204 bytes per bike entry
constexpr int       BIKE_FOLDER     = 0x000;     // folder/ID (matched by _stricmp)
constexpr int       BIKE_CFGNAME    = 0x4C0;     // string in bikes\<name>\<name>.cfg

// fcn.1400E4550 : the bike/session APPLY loader.  __fastcall(void* rcx, void* rdx).
//   Re-derives the selected index by name-matching entry+0x00 against a name held in
//   the descriptor (rdx), builds "%sbikes\%s\%s.cfg", loads the machine. Mirrors the
//   track loader's re-derive-by-name shape, but selection travels through the passed
//   descriptor (no fixed session-cfg global like tracks' 0xE4B540). Callers 0xE5632/0xE57A0.
constexpr uintptr_t RVA_BIKE_APPLY  = 0xE4550;
// AOB for the apply prologue (offset-drift resilience; the E8 rel32 is wildcarded):
//   40 53 56 57 41 57 B8 C8 20 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 8B 05
constexpr char SIG_BIKE_APPLY[] =
    "\x40\x53\x56\x57\x41\x57\xB8\xC8\x20\x00\x00\xE8\x00\x00\x00\x00\x48\x2B\xE0\x48\x8B\x05";
constexpr char SIG_BIKE_APPLY_MASK[] = "xxxxxxxxxxxx????xxxxxx";

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
constexpr uintptr_t RVA_SB_CONNECT_TARGET = 0xE53DE0; // connect OUTPUT/state (see below)

// ---- direct connect (FrostMod feature: JOIN a server by IP:port) --------------
// CORRECTED MODEL (capstone xref sweep, 2026-07-12 - r2's cached project returned 0xFF
// for these regions; capstone on the raw file is the source of truth). The earlier
// "fill 0xE53DE0 and call a connect fn" plan is DEAD:
//   * 0xE53DE0 is connect OUTPUT/state, NOT a settable input. All 29 code refs are
//     RESETS (6 writer sites store -1/0 sentinels: 0x5FEA9, 0x9210A, 0x921BF, 0xF0F8A,
//     0x111FB3, 0x11A374) or `lea &struct` pointer-passes. NO site LOADS its fields to
//     drive a socket, and the 16-byte +0x00 field is written only as raw bytes -> a
//     PACKED binary address (the resolved/connected addr), not an ASCII host you set.
//   * The browser JOIN writes a DIFFERENT struct at rbx+0xE54030 (mov [rbx+0xE54030],
//     ax @ 0x0AA3A8). "msg 0x385 fills 0xE53DE0" was a struct mix-up.
//   * The connection is initiated by the engine COMMAND BUS - a runtime fn-ptr at
//     [0x140566C48] - with command 0x389. The JOIN handler (~0x0F0Exx) does:
//         lea r8,  [rsp+0x200]     ; r8 = the REAL input (likely ASCII host / host:port)
//         lea rdx, [0x140E53DE0]   ; rdx = the OUTPUT target struct
//         mov ecx, 0x389           ; cmd id
//         call [0x140566C48]       ; -> eax (0 = fail -> struct reset)
//     Dispatch sites: 0x0F0FD3 (call instr 0x0F0FE7) and 0x0EEF63 (0x0EEF6F).
//   * Net layer, downstream of the bus (via the fn-ptr, NOT statically linkable):
//     resolve_host 0x2854E0 <- 0x28422E ; sendto 0x284BD0 <- 0x283DC0.
// Driving cmd 0x389 is NOT a cold call: it needs the LIVE bus + menu state (the handler
// validates the server's track vs the local track array [0x140F43298] first) and the
// correct r8. Open unknowns before we can dispatch: (a) the r8 param layout; (b) whether
// the bus can be driven outside the menu JOIN flow.
// NEXT STEP (x64dbg): bp 0x0F0FE7 (and 0x0EEF6F); on hit dump [r8], confirm rdx ==
// 0x140E53DE0, step over, dump 16B @ 0xE53DE0 (now the packed addr). That settles the
// input format. Until then direct connect is PREVIEW-only (parses + logs, no dispatch).
constexpr uintptr_t RVA_CMD_BUS_PTR   = 0x566C48; // [0x140566C48] engine command-bus fn-ptr
constexpr int       CMD_JOIN          = 0x389;    // bus cmd id that initiates the connect
constexpr uintptr_t RVA_JOIN_DISPATCH = 0xF0FE7;  // the `call [bus]` inside the JOIN handler
constexpr uint16_t  MXB_DEFAULT_PORT  = 54200;    // 0xD3B8; used when :port is omitted

// ---- session settings / overjump crash (capstone sweep of beta21e, 2026-08-31) ----
// beta21e's changelog adds "new: overjump crash option", and it does not stick: players
// uncheck it, join, and the crash is back. The setting is ONE dword inside the session
// settings block the command bus is handed at session start:
//
//     cmd 0x310: ... arg13 = &settings, arg14 = 0x21C (its size)
//     settings[+0x190] != 0  =>  overjump crash DISABLED   (stored INVERTED)
//
// How +0x190 was pinned, all in mxbikes.exe beta21e (base 0x140000000):
//   0x02546B  the dedicated-server config loader reads `[hardcore] overjump_crash`
//             (bus cmd 0x27 = read int, default 1 = crash ON) into a local,
//   0x02549D  stores `value == 0` into the block at [rsp+0x2D0],
//   0x026FCC  hands the block ([rsp+0x140], so the field is +0x190) to cmd 0x310.
// The client and single-player paths dispatch the same command at 0x0921B7 and 0x111F5E,
// both with the same (ptr, 0x21C) pair in arg13/arg14.
//
// beta21d parses that key with byte-identical code - same section, same default, same
// inverted store - so the ini side did NOT change in 21e, only the client-side option
// did. That is why a server's `overjump_crash = 0` is not enough on its own, and why the
// probe reads the block on the CLIENT.
//
// Both ids are build-specific: 21d numbers the same config read 0x25, not 0x27. Sanity
// check a dispatch on the 0x21C size arg, never on the id alone.
constexpr int    CMD_SESSION_START     = 0x310;  // bus cmd handed the settings block
constexpr int    CMD_CFG_READ_INT      = 0x27;   // bus cmd: read an int out of the ini
constexpr size_t SESSION_CFG_SIZE      = 0x21C;  // arg14 at every 0x310 dispatch
constexpr size_t OFF_OVERJUMP_DISABLED = 0x190;  // dword; non-zero = crash disabled

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

// ---- surgical reload: the content-load section of fcn.1400ef210 ---------------
// Transcribed verbatim from boot init, RVA 0xef68e..0xef8xx. Every content list is
// cleared and rescanned from disk (tracks, bikes, tyres, helmets, boots, riders, ...),
// but the input/sound/Steam re-init and the UI transition that follow it are skipped —
// so new mods of any type appear with no loading screen and no bounce to the menu.
//
// Boot init also carries `cmp/jge` bail-outs between some loaders (e.g. "no bikes found
// -> abort startup"). Those are deliberately NOT transcribed: they jump out of the
// routine, which is meaningless when replaying just this section.
constexpr RLStep kReloadSteps[] = {
    {0,0,0,0,0x2460}, {0,0,0,0,0x1CE00},                 // tracks
    {1,0xF3DC80,0x109DEC4,0xF3DC48,0x1B790},
    {0,0,0,0,0x3100}, {0,0,0,0,0x3FA0}, {0,0,0,0,0x171D0}, // bikes
    {1,0x109DE88,0xF3DC40,0xF4EDF8,0x17320},
    {1,0xF3DB9C,0xF3DC9C,0xF4EDA0,0x17950},
    {0,0,0,0,0x17F80},
    {1,0xF48620,0xF3DB64,0x109E090,0x18360},
    {1,0x10A30F4,0xF4EDDC,0xF3DC28,0x189C0},
    {1,0xF4EE00,0x109DE90,0xF48610,0x19060},
    {0,0,0,0,0x1BDD0},
    {1,0xF3DB50,0x109DEA8,0xF3DC90,0x19330},
    {1,0x109DEA4,0xF3DC8C,0xF48658,0x1AE10},
    {0,0,0,0,0x1B420}, {0,0,0,0,0x19DA0},
    {1,0xF48660,0xF4EDE0,0xF432A0,0x1A110},
    {1,0xF3DC58,0xF432B4,0xF48208,0x1A770},
    {1,0x106BB28,0x109DEB0,0xF432A8,0x1C140},
    {1,0xF432C0,0xF48608,0xF4EDD0,0x1C450},
};
constexpr int kReloadStepCount = (int)(sizeof(kReloadSteps) / sizeof(kReloadSteps[0]));

// The two operands the DIR steps pass. `RELOAD_STR` is the empty string the game hands
// its loaders for "the game dir" (cwd); `RELOAD_MODS` is the mods-folder path global,
// scanned only when non-empty — exactly the `cmp byte ptr [mods], 0 / je` the game does.
constexpr uintptr_t RVA_RELOAD_STR  = 0x3333EB;
constexpr uintptr_t RVA_RELOAD_MODS = 0xE54B44;

} // namespace mxb

// ============ GP Bikes ========================================================
// Same engine, same source, different build — so the analogous routines exist but every
// RVA differs. Recovered statically from an unpacked `gpbikes.exe` (SteamStub removed with
// Steamless; ImageBase 0x140000000, `.text` entropy 6.035).
//
// Only what live mod reload needs is here. The server browser / master protocol group and
// the in-game bike array are NOT ported — see `offsets_complete` on GameOffsets below,
// which keeps those features off rather than letting them fire at MX Bikes addresses.
//
// UNVERIFIED AT RUNTIME: every constant below was derived by static analysis and has not
// been confirmed under a debugger. See tasks/gp-bikes-port.md for the full derivation.
//
// THE TABLE TOOK THE GAME DOWN IN THE FIELD (2026-08-09). A reporter's v0.11.0 log shows
// the reload killing GP Bikes: `[reload] surgical content reload` with no `[reload] done`
// after it, and the process gone — once on the first reload of a session, and once on the
// fourth after three clean ones. So `reload_verified` below is false and the reload is
// REFUSED on GP Bikes unless explicitly armed (`--unsafe-reload`). It is a table to
// finish deriving, not a feature that works.
//
// What that log DOES settle (it is not all bad news — see the per-step notes):
//   * The SC assumption is right for the categories it reaches. Boot scans stack as
//     `<scanner> <- <inside loader> <- <boot-init return> <- 0x402e5(WinMain)`, and for
//     tracks/tyres/rider/bikes the game-dir and mods-dir scans share ONE boot-init return
//     with two call sites inside the loader — the loader does both directories itself.
//   * Boot content load runs on the WinMain thread. Our replay runs on whichever thread
//     calls SwapBuffers; if those differ, replaying these loaders races the game's own
//     use of the lists, which fits an intermittent kill exactly. Both threads now log
//     their id ([capture] scan tid=, [reload] ... tid=) so the next log settles it.
//   * The log reaches only 5 of the 13 categories — `LogScanCallers` stops after 16
//     stack shots — so the other 8 RVAs remain string-derived guesses. Any one of them
//     landing on the wrong function would corrupt the heap much like this.
namespace gpb {

// Boot content-load + app-init; GP twin of mxb::RVA_CONTENT_INIT.
//   Identified by fingerprinting MX's routine by the strings it references (%sglobal.ini,
//   mods, cache, %stextures\, lastprofile, %sprofiles\%s\profile.ini, texture_quality,
//   core, ...) with `mxbikes.ini` swapped for `gpbikes.ini`: this function shares 19 of
//   those 23, is one of only three referencing "gpbikes.ini", is called from exactly one
//   site (0x402e0 — "called once from WinMain"), and takes `int mode` in ecx like MX's.
//   .pdata range 0xfb650-0xfcfb7.
// Carries MX's warning too: it re-inits Steam/input/sound and transitions to the UI, so
// calling it mid-game is a soft restart to the menu.
constexpr uintptr_t RVA_CONTENT_INIT = 0xfb650;

// Generic VFS directory walker (out_status, dir, ext, out_buf); GP twin of
// mxb::RVA_SCAN_FOLDER. Pinned two independent ways that agree exactly:
//   1. mxb::SIG_SCAN_FOLDER matches here byte-for-byte — including the same 0x7f8 frame —
//      and is the ONLY match at a .pdata function start in the whole binary.
//   2. In MX it sits 4 .pdata entries before the "%s*.pkz" wrapper; GP's wrapper is
//      0x18f53b, minus 4 entries lands here.
// It also has 3 call sites (0x16020b, 0x237421, 0x2bee1d), matching MX's 3.
constexpr uintptr_t RVA_SCAN_FOLDER = 0x18f150;

// ---- surgical reload: the content-load section of GP's boot init --------------
// The contiguous run of loader calls at 0xfb95a..0xfb9b3 inside RVA_CONTENT_INIT.
//
// GP differs from MX in shape, not in substance: where MX inlines the per-directory
// dance into boot init for some categories (the DIR steps), GP wraps it inside each
// loader — every one zeroes its own list globals and scans both the game dir and the
// mods dir itself. So every GP step is SC and the table needs no z-globals, no
// RELOAD_STR and no RELOAD_MODS.
//
// How each was identified: by the path-format strings it and its callees reference,
// the same signal that names MX's (0x2460 -> "%stracks", 0x1ce00 -> "%styres", ...).
// Corroborated against a reporter's crash log, whose stack traces for the boot track
// scan carry frames 0x13a18 and 0x13a36 — two call sites inside 0x139a0, one per
// directory — with return address 0xfb95f, i.e. the `call 0x139a0` at 0xfb95a.
//
// Boot init's `cmp/jge` bail-out after the bikes loader is not transcribed, matching
// how MX's table treats the same construct.
//
// Deliberately stops before 0x315f0 ("music"/"ogg"): MX's table draws the line at
// content too, and restarting the music is not what a mods reload is for.
// The four marked "boot-corroborated" are the ones a reporter's log pins directly: each
// shows TWO scanner call sites inside the loader (game dir, then the mods dir) under a
// SINGLE boot-init return address, which is exactly the SC shape this table assumes.
// See the "what the reporter log settles" note above. The rest are string-derived only.
constexpr RLStep kReloadSteps[] = {
    {0,0,0,0,0x139A0, "tracks"},              // boot-corroborated (ret 0xfb95f, calls 0x13a18/0x13a36)
    {0,0,0,0,0x34AE0, "tyres"},               // boot-corroborated (ret 0xfb964, calls 0x34b0f/0x34b28)
    {0,0,0,0,0x34080, "rider"},               // boot-corroborated (ret 0xfb969, calls 0x340af/0x340c8)
    {0,0,0,0,0x14D90, "bikes"},               // boot-corroborated (ret 0xfb96e, calls 0x14e12/0x14e29)
    {0,0,0,0,0x31880, "paints\\data.ini"},
    {0,0,0,0,0x32090, "bike paints"},         // boot-corroborated (ret 0xfb98b, call 0x320bf)
    {0,0,0,0,0x32490, "rider\\helmets"},
    {0,0,0,0,0x32F30, "helmet paints"},
    {0,0,0,0,0x344C0, "rider\\riders"},
    {0,0,0,0,0x336A0, "rider paints"},
    {0,0,0,0,0x33A90, "rider\\animations"},
    {0,0,0,0,0x34FD0, "misc\\stands"},
    {0,0,0,0,0x25C50, "misc\\dashes"},
};
constexpr int kReloadStepCount = (int)(sizeof(kReloadSteps) / sizeof(kReloadSteps[0]));

} // namespace gpb

// ============ Kart Racing Pro =================================================
// Same engine again, third build. NOTHING IS DERIVED YET, and that is why this namespace
// is empty rather than populated with plausible numbers.
//
// `kart.exe` is SteamStub-wrapped exactly as `gpbikes.exe` was — `.bind` section, `.text`
// entropy 8.000 — so no RVA can be read out of the shipped file. Unlike GP Bikes there is
// no unpacked copy to work from, so the addresses come from the game itself instead: with
// no content offsets for this title FrostMod pattern-scans `.text` for the scanner and
// logs every scan's call stack (`[sig]` / `[stack]` lines). One boot of Kart Racing Pro
// yields the scanner RVA, one loader RVA per content category, and the boot-init call site
// above them — which is the whole of what `kReloadSteps` and `RVA_CONTENT_INIT` need.
//
// See tasks/kart-racing-pro-port.md for the capture recipe and how to read that log.
//
// What IS settled about this title needs no RE: `.rdata` is unencrypted and carries the
// same plugin export table as the other two, and PiBoSo publishes the ABI in krp_example.c.
// That is where GAME_KRP's plugin identity below comes from, and it is why plugin mode —
// overlay, radar, the session block — works here with the reload still unported.
namespace krp {
// (empty by design — a wrong-but-present address is worse than a missing one)
} // namespace krp

// ============ which title we're attached to ===================================
// The scanner signature is deliberately NOT part of this: GP's scanner prologue is
// byte-identical to MX's, so mxb::SIG_SCAN_FOLDER resolves both games unchanged and the
// existing signature-with-delta fallback keeps working for either.
struct GameOffsets {
    /// Short, stable id — what `frostmod.exe --game <id>` takes. Matches the ids MXB App
    /// uses in its own config, so the two agree on what a title is called.
    const char* id;
    /// Process image name, matched case-insensitively.
    const char* exe;
    /// Product name for logs. Never translated.
    const char* display;
    /// Folder under `Documents\PiBoSo` this title keeps its user content in. Matches the
    /// `user_dir` MXB App uses in `src-tauri/src/game.rs`, so both agree where mods live.
    const char* user_dir;
    /// What `GetModID()` must return inside this title, and the data version that goes with
    /// it. The game compares both against its own build and silently DROPS a plugin that
    /// disagrees, so these are the difference between plugin mode working and the .dlo
    /// being ignored. Taken from PiBoSo's published example for each title
    /// (mxb_example.c / gpb_example.c / krp_example.c).
    const char* plugin_id;
    int plugin_data_version;
    /// Boot content-load routine and the generic directory scanner. BOTH ZERO for a title
    /// whose content offsets have not been derived — see `content_derived()`.
    uintptr_t content_init;
    uintptr_t scan_folder;
    /// The surgical reload's step table for this title, or null when it has not been
    /// derived. Null means reload is REFUSED, not attempted with another title's
    /// addresses — see RequestReload. Never fall back to `mxb::kReloadSteps` here: those
    /// RVAs address a different binary, so replaying them calls arbitrary code and zeroes
    /// arbitrary memory in this one.
    const RLStep* reload_steps;
    int reload_count;
    /// Operands for the DIR steps. Both unused (0) for a table that is entirely SC.
    uintptr_t reload_str;
    uintptr_t reload_mods;
    /// Whether this title's step table has been confirmed to run without taking the game
    /// down. False = derived but not proven, so the reload is REFUSED unless the user
    /// explicitly arms it (`frostmod.exe --unsafe-reload`, for collecting a step-level
    /// log). This is a third state on purpose: a null table means "nothing derived", and
    /// a wrong-but-present table is the more dangerous case — v0.10.0 crashed GP Bikes
    /// with one, and v0.11.0 shipped a GP-specific replacement that crashes it too. A
    /// table only earns `true` by being exercised on the title itself.
    bool reload_verified;
    /// Whether every offset in this file has been derived for this title. False means the
    /// features that need the un-ported groups (server browser filter, master protocol,
    /// direct connect, the in-game bike array) must stay OFF — their MX addresses are
    /// meaningless here, and hooking or reading them would land at a wild location.
    bool offsets_complete;

    /// Whether the two content addresses above are real for this title. A title without
    /// them is not "at RVA 0": `base + 0` is the PE header, and hooking or calling it is a
    /// wild write. Callers gate on this and fall back to a pure signature scan instead —
    /// which is also how a new title's addresses get collected in the first place.
    constexpr bool content_derived() const { return content_init != 0 && scan_folder != 0; }
};

/// Every PiBoSo title in this file reports plugin interface version 9 — mxb_example.c,
/// gpb_example.c and krp_example.c all return the same number. It is the ABI of the
/// callback table itself, which has not changed across the three; the per-title
/// `plugin_data_version` is the one that has.
inline constexpr int kPluginInterfaceVersion = 9;

inline constexpr GameOffsets GAME_MXB = {
    "mxb", "mxbikes.exe", "MX Bikes", "MX Bikes",
    "mxbikes", 8,   // plugin identity, from mxb_example.c
    mxb::RVA_CONTENT_INIT, mxb::RVA_SCAN_FOLDER,
    mxb::kReloadSteps, mxb::kReloadStepCount,
    mxb::RVA_RELOAD_STR, mxb::RVA_RELOAD_MODS,
    true,   // reload_verified: this is the table that has been in players' hands since v0.9
    true,
};

inline constexpr GameOffsets GAME_GPB = {
    "gpb", "gpbikes.exe", "GP Bikes", "GP Bikes",
    // gpb_example.c. FrostMod answered "mxbikes"/8 to every host until v0.15, so GP Bikes
    // dropped the .dlo on sight and plugin mode never worked there — injection did.
    "gpbikes", 12,
    gpb::RVA_CONTENT_INIT, gpb::RVA_SCAN_FOLDER,
    // Entirely SC, so no DIR operands: see gpb::kReloadSteps.
    gpb::kReloadSteps, gpb::kReloadStepCount, 0, 0,
    false,  // reload_verified: it crashed a reporter's game — armed only by --unsafe-reload
    false,
};

inline constexpr GameOffsets GAME_KRP = {
    "krp", "kart.exe", "Kart Racing Pro", "Kart Racing Pro",
    "krp", 6,       // plugin identity, from krp_example.c
    // No content addresses derived — see `namespace krp` above. Zero here is load-bearing:
    // `content_derived()` reads it, and the scanner is resolved by signature scan instead.
    0, 0,
    nullptr, 0, 0, 0,   // no reload table, so RequestReload refuses cleanly
    false,  // reload_verified: there is nothing to verify yet
    false,  // offsets_complete: only the plugin ABI is ported
};

inline constexpr const GameOffsets* ALL_GAMES[] = { &GAME_MXB, &GAME_GPB, &GAME_KRP };

