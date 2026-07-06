// ============================================================================
//  FrostMod - on-demand mods-folder reloader for MX Bikes
//
//  What it does
//  ------------
//  MX Bikes reads the mods/content folders once at startup and mounts every
//  .pkz into an in-memory virtual filesystem. New files dropped in while the
//  game runs are ignored until a restart. FrostMod re-triggers the game's own
//  content scan (via an in-game overlay + F8, or the frostmod.exe console 'R') so
//  newly added tracks/skins/bikes register live, with no loading screen.
//
//  How the reload works (see README + offsets.h for the RE details)
//  ----------------------------------------------------------------
//  The game's folder scanner is fcn.140158be0 (RVA 0x158be0) and the registry
//  reset is fcn.140159340 (RVA 0x159340). Rather than guess their argument
//  formats, FrostMod HOOKS both and RECORDS the arguments the first time the
//  game itself calls them (at startup, or when a content menu re-scans). The
//  reload button then REPLAYS those recorded calls on the game's render thread.
//
//  IMPORTANT: for capture to happen, the game must call the scanner at least
//  once while FrostMod is loaded. If you inject after launch and have never
//  opened a content/track menu, click Reload once you have (watch frostmod.log).
//  If you inject at launch (proxy DLL), startup capture happens automatically.
//
//  Threading: the UI runs on its own thread; the actual game calls are queued
//  and executed inside the SwapBuffers hook (the render thread) so we never
//  mutate the VFS while the game is reading it.
// ============================================================================

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <deque>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <set>
#include <cstring>
#include <intrin.h>   // _ReturnAddress (find the walk's caller)
#include <GL/gl.h>    // in-game overlay (immediate-mode GL, drawn in the swap hook)

#include "MinHook.h"
#include "offsets.h"
#include "serverfilter.h"
#include "version.h"    // FROSTMOD_VERSION

// ---------------------------------------------------------------------------
// small logging helper -> <dll folder>\frostmod.log  (and OutputDebugString)
//
// We log NEXT TO the dll (frostmod.exe reads the same folder) rather than to
// %TEMP%, because a Steam-launched game can have a different %TEMP% than the
// launcher, which would hide our output. Falls back to %TEMP% if read-only.
// ---------------------------------------------------------------------------
namespace {

std::mutex g_logMutex;
char g_logPath[MAX_PATH] = {0};

// lifecycle: FrostMod loads two ways - injected (frostmod.exe) or as a PiBoSo
// plugin the game loads itself from its plugins folder. Either way we run Init
// exactly once (guarded), then install the same hooks.
std::atomic<bool> g_initStarted{false};
HMODULE g_selfModule = nullptr;
char    g_savePath[MAX_PATH] = {0};   // PiBoSo Startup() save/data path (plugin mode)

// Resolve the log path once, from the dll's own module handle (its folder is the
// same folder as frostmod.exe). Called from DllMain before anything else logs.
void InitLogPath(HMODULE self) {
    char p[MAX_PATH];
    if (self && GetModuleFileNameA(self, p, sizeof(p))) {
        if (char* slash = strrchr(p, '\\')) *(slash + 1) = 0;
        char cand[MAX_PATH];
        strcpy_s(cand, p);
        strcat_s(cand, "frostmod.log");
        if (FILE* f; fopen_s(&f, cand, "a") == 0 && f) { fclose(f); strcpy_s(g_logPath, cand); return; }
    }
    char t[MAX_PATH];
    if (GetTempPathA(sizeof(t), t)) { strcat_s(t, "frostmod.log"); strcpy_s(g_logPath, t); }
}

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lk(g_logMutex);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    // fall back to %TEMP% if InitLogPath hasn't run yet for some reason
    char temp[MAX_PATH];
    const char* path = g_logPath[0] ? g_logPath
                     : (GetTempPathA(sizeof(temp), temp) ? (strcat_s(temp, "frostmod.log"), temp) : nullptr);
    if (path) {
        if (FILE* f; fopen_s(&f, path, "a") == 0 && f) {
            SYSTEMTIME st; GetLocalTime(&st);
            fprintf(f, "[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
            fclose(f);
        }
    }
}

// adapter so serverfilter (which takes a plain void(*)(const char*)) logs here
void SfLog(const char* msg) { Log("%s", msg); }

// ---------------------------------------------------------------------------
// game-thread task queue: UI thread enqueues, render thread drains
// ---------------------------------------------------------------------------
std::mutex g_taskMutex;
std::deque<std::function<void()>> g_tasks;

void EnqueueGameThreadTask(std::function<void()> task) {
    std::lock_guard<std::mutex> lk(g_taskMutex);
    g_tasks.push_back(std::move(task));
}

void DrainGameThreadTasks() {
    for (;;) {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lk(g_taskMutex);
            if (g_tasks.empty()) return;
            task = std::move(g_tasks.front());
            g_tasks.pop_front();
        }
        task();
    }
}

// ---------------------------------------------------------------------------
// resolved game addresses + captured call arguments
// ---------------------------------------------------------------------------
uintptr_t g_base = 0;
// Build drift: (address where the scanner signature was actually found) minus its
// expected RVA. 0 when offsets.h matches this build exactly; nonzero when the game
// binary shifted (a game update, or SteamStub unpacking to a different layout).
// EVERY fixed RVA we use must add this, or it points at the wrong bytes on a build
// that differs from the one offsets.h was RE'd against (e.g. a friend's PC).
intptr_t g_sigDelta = 0;

// fcn.140158be0 - folder scanner  (4 register args, no stack args)
using ScanFolder_t = int64_t(__fastcall*)(void*, void*, void*, void*);
ScanFolder_t g_origScan = nullptr;

// fcn.1400ef210 - the game's content-load / app-init routine; re-running it
// rescans all content (bikes/tracks/tyres/...) from disk. This is the reload
// target. Resolved (g_base + RVA_CONTENT_INIT) in Init; null if unavailable.
using ContentInit_t = int64_t(__fastcall*)(int, int64_t, int64_t, int64_t);
ContentInit_t g_contentInit = nullptr;

// fcn.140159340 - registry reset/rebuild  (2 register args)
using RegistryReset_t = int64_t(__fastcall*)(void*, void*);
RegistryReset_t g_origReset = nullptr;

struct CapturedCall {
    std::atomic<bool> valid{false};
    void* a0{}; void* a1{}; void* a2{}; void* a3{};
};
CapturedCall g_resetArgs;  // last args seen for the registry reset (probe only)

// Every DISTINCT (dir, ext) scan the game made at startup, so a reload can replay
// just the mods (.pkz) scan or all content scans. NOTE: a0 (status) and a3 (out
// buf) were the game's *stack* buffers at capture time; a1 (dir) and a2 (ext)
// point into the module's data and stay valid. Replay strategies account for that.
struct ScanCall { void* a0{}; void* a1{}; void* a2{}; void* a3{}; std::string dir, ext; };
std::mutex g_scansMutex;
std::vector<ScanCall> g_scans;

// SEH-guarded raw copy into a POD buffer (no C++ objects here, so the compiler
// allows __try/__except in this function).
static bool SafeCopyStr(const void* p, char* out, size_t cap) {
    __try {
        const char* s = reinterpret_cast<const char*>(p);
        size_t n = 0;
        while (n + 1 < cap && s[n]) { out[n] = s[n]; ++n; }
        out[n] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH-guarded int read (POD, so __try is allowed here).
static int SafeReadInt(const int* p) {
    __try { return *p; } __except (EXCEPTION_EXECUTE_HANDLER) { return -0x7FFF; }
}

// SEH-guarded byte copy up to cap; returns bytes read (stops on the first fault).
static size_t SafeReadBytes(const char* p, char* out, size_t cap) {
    size_t n = 0;
    __try { while (n < cap) { out[n] = p[n]; ++n; } }
    __except (EXCEPTION_EXECUTE_HANDLER) { /* stop at n */ }
    return n;
}

// Read a NUL-terminated string from a (possibly bogus) pointer, safely.
std::string SafeStr(void* p) {
    if (!p) return "<null>";
    char tmp[261];
    if (!SafeCopyStr(p, tmp, sizeof(tmp))) return "<unreadable>";
    return std::string(tmp);
}

// ---------------------------------------------------------------------------
// DIAGNOSTIC: log the CALL STACK of the boot content scans.
//
// The function that BUILDS the game's track/bike list calls the walker through
// the engine's API dispatcher (fcn.140120CC0, invoked via the global fn-ptr
// qword_140566C48), so static xrefs can't find it. A runtime stack walk does:
// it shows the whole chain  walker <- dispatcher <- LOADER <- boot-init. The
// frame with a small rva (< 0x1000000 -> inside mxbikes.exe, ~14MB) that sits
// just above the dispatcher IS the content-loader = the reload target. Logged
// for the first several DISTINCT scans only; harmless, no game state touched.
// ---------------------------------------------------------------------------
using RtlCaptureStackBackTrace_t = USHORT(NTAPI*)(ULONG, ULONG, PVOID*, PULONG);
std::atomic<int> g_stackShots{0};

void LogScanCallers(const std::string& dir, const std::string& ext) {
    static RtlCaptureStackBackTrace_t cap = []() -> RtlCaptureStackBackTrace_t {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        return nt ? (RtlCaptureStackBackTrace_t)GetProcAddress(nt, "RtlCaptureStackBackTrace")
                  : nullptr;
    }();
    if (!cap || g_stackShots.fetch_add(1) >= 16) return;   // first few scans only

    void* frames[24] = {0};
    USHORT n = cap(1 /*skip LogScanCallers itself*/, 24, frames, nullptr);
    std::string chain;
    char b[80];
    for (USHORT i = 0; i < n; ++i) {
        uintptr_t rva = (uintptr_t)frames[i] - g_base;
        _snprintf_s(b, sizeof(b), _TRUNCATE, "%s0x%zx%s",
                    i ? " <- " : "", (size_t)rva, (rva < 0x1000000) ? "" : "(ext)");
        chain += b;
    }
    Log("[stack] dir='%s' ext='%s': %s", dir.c_str(), ext.c_str(), chain.c_str());
}

// ---------------------------------------------------------------------------
// hooks that CAPTURE the game's own content-load calls
// ---------------------------------------------------------------------------
int64_t __fastcall hkScan(void* a0, void* a1, void* a2, void* a3) {
    // This scanner is GENERIC: (status, directory, file-extension, buf). The game
    // calls it for many folders - e.g. ui/str, and mods content with ext "pkz".
    std::string dir = SafeStr(a1);
    std::string ext = SafeStr(a2);

    // Record each DISTINCT (dir, ext) once and keep its args so a reload can replay
    // it. We still store ALL of them, but only LOG the first few - the startup scan
    // fires hundreds of times and used to bury everything else (e.g. [srvlist]).
    {
        bool isNew = false;
        size_t count = 0;
        {
            std::lock_guard<std::mutex> lk(g_scansMutex);
            bool exists = false;
            for (const auto& c : g_scans) if (c.dir == dir && c.ext == ext) { exists = true; break; }
            if (!exists && g_scans.size() < 128) {
                g_scans.push_back(ScanCall{a0, a1, a2, a3, dir, ext});
                isNew = true; count = g_scans.size();
            }
        }
        if (isNew && count <= 5)
            Log("[capture] scan dir='%s' ext='%s' (rcx=%p rdx=%p r8=%p r9=%p)",
                dir.c_str(), ext.c_str(), a0, a1, a2, a3);
        else if (isNew && count == 6)
            Log("[capture] ...(further startup scans still recorded, logging suppressed)");

        // Pin the content-loader: log who called us (up through the API dispatcher).
        if (isNew) LogScanCallers(dir, ext);
    }

    return g_origScan(a0, a1, a2, a3);
}

int64_t __fastcall hkReset(void* a0, void* a1) {
    if (!g_resetArgs.valid.load()) {
        Log("[capture] registryReset(0x159340) args: rcx=%p rdx=%p", a0, a1);
    }
    g_resetArgs.a0 = a0; g_resetArgs.a1 = a1;
    g_resetArgs.valid.store(true);
    return g_origReset(a0, a1);
}

// PROBE (opt-in via frostmod.exe --probe-mount): capture how the real pkz-mount
// function (0x15a9e0) is called, to learn which arg is the .pkz path. Observe-only
// pass-through; we log each arg as a string so a readable path reveals itself.
// Declared with 4 register args (safe for the near-certain <=4-arg mount fn).
using MountPkz_t = int64_t(__fastcall*)(void*, void*, void*, void*);
MountPkz_t g_origMount = nullptr;

int64_t __fastcall hkMount(void* a0, void* a1, void* a2, void* a3) {
    static std::atomic<int> n{0};
    int i = n.fetch_add(1);
    if (i < 48) {   // first calls only, avoid spam
        Log("[mount] #%d 0x15a9e0(rcx=%p('%s') rdx=%p('%s') r8=%p('%s') r9=%p('%s'))",
            i, a0, SafeStr(a0).c_str(), a1, SafeStr(a1).c_str(),
            a2, SafeStr(a2).c_str(), a3, SafeStr(a3).c_str());
    }
    return g_origMount(a0, a1, a2, a3);
}

// ---------------------------------------------------------------------------
// master server-list hook (the CLEAN filter target). 0x2A10E0 handles master
// opcodes; when a HOSTED (server-list) reply completes, RVA_MP_STATE goes to 3
// and the list text sits in the blob at RVA_MP_LIST_BLOB. We dump the blob once
// per completed list so we can see its exact record format - then a follow-up can
// edit the blob here (drop spam records) before the browser parses it, no cave.
// ---------------------------------------------------------------------------
using MpMsg_t = int64_t(__fastcall*)(void*, void*, void*, void*);
MpMsg_t g_origMpMsg = nullptr;

// force=false: only logs when the blob is non-empty AND changed since last dump
// (so the per-frame auto-dump doesn't spam). force=true: always logs (manual key).
void DumpServerListBlob(bool force) {
    static char buf[8193];
    size_t n = SafeReadBytes((const char*)(g_base + mxb::RVA_MP_LIST_BLOB), buf, sizeof(buf) - 1);
    size_t end = 0, zeros = 0;
    for (size_t i = 0; i < n; ++i) { if (buf[i] == 0) { if (++zeros >= 8) break; } else { zeros = 0; end = i + 1; } }
    if (end == 0) { if (force) Log("[srvlist] blob empty/unreadable"); return; }

    uint32_t sum = 2166136261u;                     // FNV-1a over the meaningful bytes
    for (size_t i = 0; i < end; ++i) sum = (sum ^ (uint8_t)buf[i]) * 16777619u;
    static uint32_t lastSum = 0;
    if (!force && sum == lastSum) return;           // unchanged -> skip
    lastSum = sum;

    Log("[srvlist] ==== server-list blob (%zu bytes) - records below ====", end);
    for (size_t i = 0; i < end; i += 800) {
        char chunk[801];
        size_t m = end - i < 800 ? end - i : 800;
        for (size_t j = 0; j < m; ++j) { char c = buf[i + j]; chunk[j] = (c == 0) ? '|' : (c == '\n' ? '/' : c); }
        chunk[m] = 0;
        Log("[srvlist] %s", chunk);   // NUL shown as '|', newline as '/'
    }
    Log("[srvlist] ==== end blob ====");
}

std::atomic<int> g_mpCalls{0};   // how many times the master handler has fired

int64_t __fastcall hkMpMsg(void* a0, void* a1, void* a2, void* a3) {
    int64_t r = g_origMpMsg(a0, a1, a2, a3);
    static int lastState = -1;
    int st = SafeReadInt((const int*)(g_base + mxb::RVA_MP_STATE));
    int c = g_mpCalls.fetch_add(1);
    // show the handler firing (first few) and every state change, so we can see
    // whether opening the browser drives state toward 3 (list-complete).
    if (c < 6 || st != lastState)
        Log("[srvlist] master handler call#%d state=%d", c, st);
    if (st == 3 && lastState != 3) DumpServerListBlob(false);   // list just completed
    lastState = st;
    return r;
}

// Peek how many meaningful (non-zero) bytes are at the head of the list blob.
size_t BlobHeadBytes() {
    char tmp[64];
    size_t n = SafeReadBytes((const char*)(g_base + mxb::RVA_MP_LIST_BLOB), tmp, sizeof(tmp));
    size_t nz = 0; for (size_t i = 0; i < n; ++i) if (tmp[i]) nz = i + 1;
    return nz;
}

// forward decls (defined later near setup) - AOB scan helpers the filter uses to
// self-locate its hook site across game builds.
bool GetExecRange(uintptr_t base, uint8_t** begin, uint8_t** end);
uint8_t* PatternScan(uint8_t* begin, uint8_t* end, const char* pat, const char* mask);

// ---------------------------------------------------------------------------
// SERVER FILTER - hide spam/"ghost" servers from the browser.
//
// The RE (offsets.h) shows the browser builds SB_Entry working copies (stride
// 0x1D8) and a populate loop (RVA_SB_POPULATE_LOOP) emits one row each, with a
// row-skip target (RVA_SB_ROW_SKIP_TGT) that keeps the game's counts consistent.
//
// STEP BACK (2026-07-05): actually skipping rows crashed the list build (the
// game's post-loop `DISPLAY_COUNT += RAW count` desyncs the index no matter how we
// compensate). So this is now a PURE READ-ONLY DUMP: the hook observes every row,
// logs its fields + best-guess name + the filter VERDICT (keep / WOULD-HIDE), and
// never mutates a thing. Crash-proof by construction, and it doubles as the tool to
// finally pin the real SB_Entry name offset (see the [srv.hex] windows).

// ---------------------------------------------------------------------------
// POD, no C++ objects -> SEH is allowed here (a function that must unwind C++
// objects can't use __try). Reads the numeric SB_Entry fields safely.
struct SBNums { int players, maxPlayers; bool unjoinable, ok; };
static SBNums SafeReadSBNums(void* entry) {
    SBNums f{};
    __try {
        f.players    = (int)*(uint32_t*)((char*)entry + mxb::SBE_PLAYERS);
        f.maxPlayers = (int)*(uint32_t*)((char*)entry + mxb::SBE_MAXPLAYERS);
        f.unjoinable = (*(uint32_t*)((char*)entry + mxb::SBE_PING) == mxb::SBE_PING_UNJOINABLE);
        f.ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { f.ok = false; }
    return f;
}

// Log a hex+ASCII window of the copied entry header so we can eyeball field layout
// (name @ +0x86, cap @ +0xC8, current @ +0xCC, ping @ +0xDC). Read-only.
static void LogHexWindow(const char* buf, size_t len) {
    for (size_t off = 0; off < len; off += 16) {
        char line[96]; int p = 0; char ascii[17]; int a = 0;
        p += sprintf_s(line + p, sizeof(line) - p, "+0x%03zX: ", off);
        for (size_t k = 0; k < 16; ++k) {
            if (off + k < len) {
                unsigned char c = (unsigned char)buf[off + k];
                p += sprintf_s(line + p, sizeof(line) - p, "%02X ", c);
                ascii[a++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
            } else p += sprintf_s(line + p, sizeof(line) - p, "   ");
        }
        ascii[a] = 0;
        Log("[srv.hex] %s|%s|", line, ascii);
    }
}

// Log every printable-ASCII run (>=3 chars) with its offset - to locate string
// fields (folder/name) inside an unknown struct at runtime.
static void LogPrintableRuns(const char* tag, const char* buf, size_t len) {
    for (size_t i = 0; i < len; ) {
        unsigned char c = (unsigned char)buf[i];
        if (c >= 0x20 && c < 0x7F) {
            char run[160]; size_t k = 0, j = i;
            while (j < len && (unsigned char)buf[j] >= 0x20 && (unsigned char)buf[j] < 0x7F
                   && k < sizeof(run) - 1) run[k++] = buf[j++];
            run[k] = 0;
            if (k >= 3) Log("%s +0x%03zX '%s'", tag, i, run);
            i = j;
        } else ++i;
    }
}

// F9 (phase 1): read the game's track array (RVA_TRACK_LIST, stride TRACK_STRIDE,
// count at RVA_TRACK_COUNT) and log each entry's printable strings + a hex window for
// the first few. This confirms we can read the list and pins the folder/name offsets,
// so the next step is an in-game keyboard-driven track switcher.
void DumpTrackList() {
    int count = SafeReadInt((const int*)(g_base + mxb::RVA_TRACK_COUNT));
    Log("[tracks] ===== track list (count=%d, stride=%d) =====", count, mxb::TRACK_STRIDE);
    if (count <= 0 || count > 100000) {
        Log("[tracks] count looks wrong - the array/count offset may not fit this build.");
        return;
    }
    const uintptr_t base = g_base + mxb::RVA_TRACK_LIST;
    const int shown = count < 60 ? count : 60;
    for (int i = 0; i < shown; ++i) {
        char raw[0x140];
        size_t n = SafeReadBytes((const char*)(base + (size_t)i * mxb::TRACK_STRIDE),
                                 raw, sizeof(raw));
        char tag[24]; sprintf_s(tag, "[tracks] #%03d", i);
        LogPrintableRuns(tag, raw, n);
        if (i < 3) LogHexWindow(raw, n);   // hex for the first few to pin field offsets
    }
    if (count > shown) Log("[tracks] (+%d more not shown)", count - shown);
    Log("[tracks] ===== end (F9) =====");
}

static int  g_sbRow = 0;           // running row index within the current populate pass
static int  g_sbHexLeft = 0;       // hex windows still to dump this pass
// When true, matching rows are actually skipped (hidden) via the game's own row-skip
// label; when false the hook is a read-only PREVIEW (logs [srv] but hides nothing).
bool g_sbHideEnabled = true;

// The asm stub calls this once per server row at the LOOP TOP (0x0AB960), BEFORE the
// row is written to the widget (the first setCellText @ 0x0ABA03). Args: index = r14
// (0-based row index within this pass), gameRsp = rsp at the hook. The per-pass record
// lives at gameRsp + index*stride (name @ +0x86, cap @ +0xC8, current @ +0xCC, ping @
// +0xDC). We read + log it and RETURN true to HIDE - the stub then jmps to 0x0ACE68,
// so the game skips straight to the loop increment and NEVER writes any cell for this
// row, i.e. the row is never created (same as the game's own name-search filter). We
// never write to the record.
extern "C" bool SB_SuppressRow(uint64_t index, void* gameRsp) {
    if (!gameRsp) return false;
    char* base = (char*)gameRsp + (size_t)index * mxb::SBE_STRIDE;   // this row's record

    if (index == 0) {                                // new populate pass (r14 restarts at 0)
        g_sbRow = 0;
        g_sbHexLeft = 1;                             // one hex window to re-confirm the record
        Log("[srv] ===== server-list pass (%s) =====",
            g_sbHideEnabled ? "hiding cheat/ad spam" : "PREVIEW - nothing hidden");
    }

    char raw[0xE0];
    size_t n = SafeReadBytes(base, raw, sizeof(raw));

    SBNums nums = SafeReadSBNums(base);              // .players=+0xCC(current) .maxPlayers=+0xC8(cap)
    int ping = SafeReadInt((const int*)(base + mxb::SBE_PING));
    bool pingUnresolved = ((uint32_t)ping == mxb::SBE_PING_UNJOINABLE);

    char name[128] = "";
    SafeCopyStr(base + mxb::SBE_NAME, name, sizeof(name));           // name @ +0x86

    frostmod::serverfilter::ServerInfo si;
    si.name       = name;
    si.players    = nums.players;                    // current
    si.maxPlayers = nums.maxPlayers;                 // capacity
    si.unjoinable = false;   // ping is unresolved at build time ("---" for all) -> don't filter on it
    std::string why = frostmod::serverfilter::ShouldHide(si);
    bool hide = !why.empty() && g_sbHideEnabled;

    char pingStr[16];
    if (pingUnresolved) strcpy_s(pingStr, "---"); else sprintf_s(pingStr, "%d", ping);
    const char* tag = hide ? "HIDE" : (why.empty() ? "keep" : "WOULD-HIDE(preview)");
    Log("[srv] #%02llu '%s' cur=%d cap=%d ping=%s | %s%s%s",
        (unsigned long long)index, si.name.c_str(), nums.players, nums.maxPlayers, pingStr,
        tag, why.empty() ? "" : ": ", why.c_str());

    if (g_sbHexLeft > 0) { LogHexWindow(raw, n); --g_sbHexLeft; }
    ++g_sbRow;
    return hide;
}

// We hook the per-server LOOP TOP (0x0AB960: `cmp byte [0x4C8F60], r12b`) with a
// MinHook detour to a hand-built stub that reads r14 (the index) + rsp, calls
// SB_SuppressRow, restores all registers/flags byte-for-byte, then:
//   * hide -> jmp 0x0ACE68  (loop increment; no cell is written -> row never created)
//   * keep -> run the original cmp via the trampoline, continue normally
// This is BEFORE the row-creating setCellText (0x0ABA03), which is the only place a
// row can be suppressed (a cell-write auto-extends the widget - there is no addRow).
void* g_sbTramp = nullptr;

bool InstallServerFilterHook() {
    // Hook the loop TOP (0x0AB960). It's a fixed RVA, but a friend's mxbikes.exe can
    // differ from the build offsets.h was RE'd against, so we SELF-LOCATE it (the mod
    // reload already does this via the scanner AOB+delta - the filter must too, or it
    // silently no-ops). Confirm each candidate by matching SB_POPULATE_LOOP_BYTES (the
    // `cmp byte [rip+..], r12b` opcode+modrm) before touching it.
    auto verify = [](uintptr_t addr) -> bool {
        unsigned char here[sizeof(mxb::SB_POPULATE_LOOP_BYTES)];
        return SafeReadBytes((const char*)addr, (char*)here, sizeof(here)) == sizeof(here)
            && memcmp(here, mxb::SB_POPULATE_LOOP_BYTES, sizeof(here)) == 0;
    };

    // candidate 1: the fixed RVA adjusted by the scanner's build-drift delta.
    uintptr_t target     = g_base + mxb::RVA_SB_POPULATE_LOOP + g_sigDelta;
    uintptr_t skipTarget = g_base + mxb::RVA_SB_ROW_SKIP_TGT  + g_sigDelta;
    const char* how = "fixed RVA+delta";

    // candidate 2: AOB-scan .text for the browser command (SIG_SB_LAN_CMD) and place
    // the hook at its known in-function offset. Survives non-uniform drift.
    if (!verify(target)) {
        uint8_t *b, *e, *lan = nullptr;
        if (GetExecRange(g_base, &b, &e))
            lan = PatternScan(b, e, mxb::SIG_SB_LAN_CMD, mxb::SIG_SB_LAN_CMD_MASK);
        if (lan) {
            target     = (uintptr_t)lan + (mxb::RVA_SB_POPULATE_LOOP - mxb::RVA_SB_LAN_CMD);
            skipTarget = (uintptr_t)lan + (mxb::RVA_SB_ROW_SKIP_TGT  - mxb::RVA_SB_LAN_CMD);
            how = "AOB(LAN_CMD)";
            Log("[filter] fixed RVA didn't match; AOB-relocated LAN_CMD to RVA 0x%zx.",
                (size_t)((uintptr_t)lan - g_base));
        }
    }

    if (!verify(target)) {
        Log("[filter] could NOT locate the browser loop-top on this build - filter NOT "
            "installed (mod reload is unaffected). Probed RVA 0x%zx, delta 0x%zx. The "
            "server-browser code differs from offsets.h; send frostmod.log to update it.",
            (size_t)(target - g_base), (size_t)g_sigDelta);
        return false;
    }

    auto stub = (unsigned char*)VirtualAlloc(nullptr, 0x100, MEM_COMMIT | MEM_RESERVE,
                                             PAGE_EXECUTE_READWRITE);
    if (!stub) { Log("[filter] stub alloc failed"); return false; }

    // register the hook first so MinHook builds the trampoline (detour = our stub);
    // we then fill the stub bytes (which reference the trampoline), then enable.
    if (MH_CreateHook((void*)target, stub, &g_sbTramp) != MH_OK) {
        Log("[filter] MH_CreateHook failed @ RVA 0x%zx", (size_t)(target - g_base));
        VirtualFree(stub, 0, MEM_RELEASE); return false;
    }

    size_t o = 0;
    auto b  = [&](unsigned char x){ stub[o++] = x; };
    auto b8 = [&](uint64_t v){ for (int i = 0; i < 8; ++i) b((unsigned char)(v >> (i * 8))); };
    // Save volatiles+flags, call SB_SuppressRow(r14 index, gameRsp) to LOG + decide,
    // restore everything, then branch on the returned hide flag. rsp is parked in RBP
    // (non-volatile -> callee must preserve it); a volatile reg would be corrupted by
    // the callback's work (Log/std::string/regex). r14 (the loop index) is nonvolatile
    // and read before the call. 9 pushes = 0x48, so game_rsp = rsp+0x48.
    b(0x50);                                             // push rax
    b(0x51);b(0x52);b(0x41);b(0x50);b(0x41);b(0x51);     // push rcx,rdx,r8,r9
    b(0x41);b(0x52);b(0x41);b(0x53);                     // push r10,r11
    b(0x9C);                                             // pushfq
    b(0x55);                                             // push rbp   (save caller's rbp)
    b(0x44);b(0x89);b(0xF1);                             // mov ecx,r14d  (arg1 = row index, zero-extended)
    b(0x48);b(0x8D);b(0x54);b(0x24);b(0x48);             // lea rdx,[rsp+0x48]  (arg2 = game_rsp)
    b(0x48);b(0x89);b(0xE5);                             // mov rbp,rsp   (park rsp in rbp; survives the call)
    b(0x48);b(0x83);b(0xE4);b(0xF0);                     // and rsp,-16   (align)
    b(0x48);b(0x83);b(0xEC);b(0x20);                     // sub rsp,0x20  (shadow)
    b(0x48);b(0xB8);b8((uint64_t)&SB_SuppressRow);       // mov rax, &SB_SuppressRow
    b(0xFF);b(0xD0);                                     // call rax  (bool hide -> al)
    b(0x48);b(0x89);b(0xEC);                             // mov rsp,rbp   (restore rsp -> saved-rbp slot)
    b(0x5D);                                             // pop rbp    (restore caller's rbp)
    b(0x84);b(0xC0);                                     // test al,al   (al = hide; rax not yet restored)
    size_t jnzPos = o;
    b(0x75);b(0x00);                                     // jnz .hide  (rel8 patched below)
    // ---- KEEP path: restore flags+regs, continue into the original loop top (trampoline) ----
    size_t keepStart = o;
    b(0x9D);                                             // popfq
    b(0x41);b(0x5B);b(0x41);b(0x5A);b(0x41);b(0x59);b(0x41);b(0x58);b(0x5A);b(0x59);b(0x58); // pop r11..rax
    b(0xFF);b(0x25);b(0x00);b(0x00);b(0x00);b(0x00);     // jmp [rip+0] -> tramp slot
    b8((uint64_t)g_sbTramp);                             // tramp slot
    // ---- HIDE path: restore flags+regs, jmp the game's own row-skip label ----
    stub[jnzPos + 1] = (unsigned char)(o - keepStart);   // patch jnz rel8 to here
    b(0x9D);                                             // popfq
    b(0x41);b(0x5B);b(0x41);b(0x5A);b(0x41);b(0x59);b(0x41);b(0x58);b(0x5A);b(0x59);b(0x58); // pop r11..rax
    b(0xFF);b(0x25);b(0x00);b(0x00);b(0x00);b(0x00);     // jmp [rip+0] -> skip slot
    b8(skipTarget);                                      // 0x0ACE68 (row-skip: row never created)

    if (MH_EnableHook((void*)target) != MH_OK) {
        Log("[filter] MH_EnableHook failed @ RVA 0x%zx", (size_t)(target - g_base));
        return false;
    }
    Log("[filter] server-filter hook LIVE @ loop-top RVA 0x%zx via %s (delta 0x%zx). "
        "Mode=%s -> jmp RVA 0x%zx (skip before row-create). [srv]: HIDE=removed, keep=shown.",
        (size_t)(target - g_base), how, (size_t)g_sigDelta,
        g_sbHideEnabled ? "HIDE" : "PREVIEW",
        (size_t)(skipTarget - g_base));
    return true;
}

// ---------------------------------------------------------------------------
// the reload action - runs ON THE GAME THREAD (called from the swap hook)
// ---------------------------------------------------------------------------
// SURGICAL reload: replicate ONLY the content-load section of the game's boot init
// (fcn.1400ef210) - every content list is cleared and rescanned from disk (tracks,
// bikes, tyres, helmets, boots, gloves, suits, ...), but the input/sound/Steam
// re-init and the UI transition that follow it are skipped. So new mods of ANY type
// appear with no loading screen and no bounce to the menu.
//
// The section (transcribed verbatim from fcn.1400ef210) uses two loader shapes:
//   SC  self-contained: clears its own list, scans game+mods dirs. Called with
//       ignored args -> we call (0,0).
//   DIR scan(dir): the game zeroes 3 list globals inline, then calls it once per
//       dir. We do the same - zero the 3, then scan &String (game dir = "") + &mods.
// RVAs are relative to g_base (module base). Each helper is SEH-guarded so a bad
// call/write on a mismatched build is caught rather than crashing the game.
using LoaderSC_t  = int64_t(__fastcall*)(int64_t, int64_t);
using LoaderDir_t = int64_t(__fastcall*)(const void*);

static void RL_SC (uintptr_t fn) { __try { ((LoaderSC_t)fn)(0, 0); }   __except (EXCEPTION_EXECUTE_HANDLER) {} }
static void RL_Z32(uintptr_t p)  { __try { *(volatile int*)p = 0; }     __except (EXCEPTION_EXECUTE_HANDLER) {} }
static void RL_Z64(uintptr_t p)  { __try { *(volatile int64_t*)p = 0; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static void RL_Dir(uintptr_t fn, const void* gameDir, const void* mods) {
    __try {
        ((LoaderDir_t)fn)(gameDir);
        if (mods && *(const char*)mods) ((LoaderDir_t)fn)(mods);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SetStatus(const char* s, unsigned ms);   // in-game overlay status (defined below)

// The reload is the same surgical content-load as before (each SC/DIR fully rebuilds
// one content list: SC = self-contained clear+scan; DIR = zero 3 list globals, then
// scan game dir + mods). But instead of running all of them in one blocking call (the
// render thread can't present a frame while it runs -> looks like a freeze), we drive
// them ONE PER FRAME from the swap hook. Between steps every list is complete (old or
// new), so the game stays consistent, and the overlay can draw an advancing progress
// bar + spinner so you can see it's working.
struct RLStep { uint8_t dir; uintptr_t z1, z2, z3, rva; };  // dir=0 -> SC(rva); dir=1 -> DIR(...)
static const RLStep kReloadSteps[] = {
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
static const int kReloadStepCount = (int)(sizeof(kReloadSteps) / sizeof(kReloadSteps[0]));

// progress state - touched on the render thread; the overlay reads the two atomics.
std::atomic<bool> g_reloadActive{false};
std::atomic<int>  g_reloadDone{0};    // steps completed (drives the progress bar)
static int  g_reloadCur = 0;          // next step to run
static bool g_reloadPrimed = false;   // have we presented one frame before starting work?

static void RunReloadStep(int i) {
    const uintptr_t b = g_base;
    const void* S = (const void*)(b + 0x3333EB);   // "String" = "" (game dir = cwd)
    const void* M = (const void*)(b + 0xE54B44);   // byte_140E54B44 = mods folder path
    const RLStep& s = kReloadSteps[i];
    if (!s.dir) RL_SC(b + s.rva);
    else { RL_Z32(b + s.z1); RL_Z32(b + s.z2); RL_Z64(b + s.z3); RL_Dir(b + s.rva, S, M); }
}

// Called once per frame on the render thread (from Tick). Runs at most ONE step, so a
// frame is presented between steps and the progress bar advances instead of freezing.
void AdvanceReload() {
    if (!g_reloadActive.load()) return;
    if (!g_reloadPrimed) { g_reloadPrimed = true; return; }   // show the 0% frame first
    if (g_reloadCur < kReloadStepCount) {
        RunReloadStep(g_reloadCur);
        g_reloadDone.store(++g_reloadCur);
    } else {
        g_reloadActive.store(false);
        Log("[reload] done - all %d content lists rebuilt from disk.", kReloadStepCount);
        SetStatus("reloaded - new mods listed", 4000);
    }
}

void RequestReload() {
    Log("[ui] reload requested");
    if (!g_contentInit) {   // g_contentInit resolved <=> offsets match this build
        Log("[reload] ABORT: offsets didn't match this build (see the [sig] lines).");
        SetStatus("reload unavailable (offsets mismatch)", 5000);
        return;
    }
    if (g_reloadActive.load()) { Log("[reload] already in progress - ignoring"); return; }
    // Re-read the server-filter blocklist too, so editing it takes effect live.
    frostmod::serverfilter::Reload();
    g_reloadCur = 0; g_reloadDone.store(0); g_reloadPrimed = false;
    g_reloadActive.store(true);   // AdvanceReload() drives it, one step per frame
    Log("[reload] surgical content reload (stepped over %d frames)...", kReloadStepCount);
    SetStatus("reloading mods...", 30000);   // long TTL; cleared when the bar completes
}

// ---------------------------------------------------------------------------
// in-game overlay - a small corner hint drawn with immediate-mode GL inside the
// wglSwapBuffers hook. Shows "FrostMod - F8: reload mods", and a transient status
// line after a reload. F7 toggles it. No mouse - F8 (or console R) does the reload.
// If the game runs a core GL profile the fixed-function calls are no-ops (overlay
// just stays hidden); everything is wrapped in push/pop so we never disturb the game.
// ---------------------------------------------------------------------------
std::atomic<bool>      g_overlayOn{true};
std::atomic<ULONGLONG> g_statusUntil{0};        // show g_statusText until this tick
std::mutex             g_statusMutex;
char                   g_statusText[128] = {0};

void SetStatus(const char* s, unsigned ms) {
    std::lock_guard<std::mutex> lk(g_statusMutex);
    strncpy_s(g_statusText, s, _TRUNCATE);
    g_statusUntil.store(GetTickCount64() + ms);
}

// GL bitmap font built from a GDI font via wglUseFontBitmaps. Needs a current GL
// context, so we build it lazily on the first overlay draw.
GLuint g_fontBase  = 0;
bool   g_fontTried = false;

void EnsureFont(HDC hdc) {
    if (g_fontTried) return;
    g_fontTried = true;
    HFONT font = CreateFontA(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    if (!font) return;
    HGDIOBJ old = SelectObject(hdc, font);
    GLuint base = glGenLists(256);
    if (base && wglUseFontBitmaps(hdc, 0, 256, base)) g_fontBase = base;
    else if (base)                                    glDeleteLists(base, 256);
    SelectObject(hdc, old);
    DeleteObject(font);
    Log("[overlay] font %s", g_fontBase ? "ready" : "unavailable (hint text hidden)");
}

void GlText(int x, int y, const char* s) {
    if (!g_fontBase || !s || !*s) return;
    glRasterPos2i(x, y);
    glListBase(g_fontBase);
    glCallLists((GLsizei)strlen(s), GL_UNSIGNED_BYTE, s);
}

static void FillRect(int x0, int y0, int x1, int y1) {
    glBegin(GL_QUADS);
      glVertex2i(x0, y0); glVertex2i(x1, y0); glVertex2i(x1, y1); glVertex2i(x0, y1);
    glEnd();
}

void DrawOverlay(HDC hdc) {
    if (!g_overlayOn.load()) return;
    EnsureFont(hdc);

    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    const int w = vp[2], h = vp[3];
    if (w <= 0 || h <= 0) return;

    static unsigned frame = 0; ++frame;              // advances every presented frame
    const bool reloading = g_reloadActive.load();
    const int  done = g_reloadDone.load(), total = kReloadStepCount;
    const float frac = (reloading && total) ? (float)done / (float)total : 0.0f;

    char line[128];
    if (reloading) {
        static const char spin[4] = {'|', '/', '-', '\\'};
        // spinner animates on each PRESENTED frame (so if a step blocks a frame, it
        // just pauses that frame - you still see it's mid-reload, not crashed).
        sprintf_s(line, "%c  Reloading mods...  %d%%", spin[(frame / 3) % 4],
                  total ? (done * 100 / total) : 0);
    } else if (GetTickCount64() < g_statusUntil.load()) {
        std::lock_guard<std::mutex> lk(g_statusMutex);
        strncpy_s(line, g_statusText, _TRUNCATE);
    } else {
        strcpy_s(line, "FrostMod v" FROSTMOD_VERSION "   -   F8: reload mods");
    }

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0, w, 0, h, -1, 1);                  // origin bottom-left
    glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);   glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // background pill, top-left corner (taller while reloading, to fit the bar)
    const int bw = 250, bh = reloading ? 38 : 24;
    const int x0 = 10, x1 = x0 + bw, y1 = h - 10, y0 = y1 - bh;
    glColor4f(0.04f, 0.05f, 0.08f, 0.72f);
    FillRect(x0, y0, x1, y1);

    glColor4f(0.47f, 0.78f, 1.0f, 1.0f);         // FrostMod light-blue
    GlText(x0 + 8, y1 - 17, line);               // text near the top of the pill

    if (reloading) {                             // progress bar along the bottom
        const int bx0 = x0 + 8, bx1 = x1 - 8, by0 = y0 + 7, by1 = by0 + 6;
        glColor4f(1.0f, 1.0f, 1.0f, 0.18f);      // track
        FillRect(bx0, by0, bx1, by1);
        glColor4f(0.47f, 0.78f, 1.0f, 0.95f);    // fill
        FillRect(bx0, by0, bx0 + (int)((bx1 - bx0) * frac), by1);
    }

    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
    glPopAttrib();
}

// ---------------------------------------------------------------------------
// SwapBuffers hooks - our per-frame tick on the render thread
// ---------------------------------------------------------------------------
using SwapBuffers_t    = BOOL(WINAPI*)(HDC);
using wglSwapBuffers_t = BOOL(WINAPI*)(HDC);
SwapBuffers_t    g_origSwapBuffers    = nullptr;
wglSwapBuffers_t g_origWglSwapBuffers = nullptr;

// Cross-process triggers from frostmod.exe (named auto-reset events, consumed on
// the render thread). "Local\..." scopes them to the logon session. Created in Init.
HANDLE g_reloadEvent = nullptr;   // R in the console -> reload
HANDLE g_dumpEvent   = nullptr;   // D in the console -> dump the server-list blob now

void DumpServerListBlob(bool force);   // fwd (defined near hkMpMsg)
// g_origMpMsg (defined above) is non-null once --dump-serverlist hooked the handler

void Tick() {
    // Heartbeat: prove the render hook is actually firing. If you never see this
    // line in frostmod.log, the game isn't calling the SwapBuffers we hooked, so
    // F8 / reload can't run - that's the thing to fix, not the reload itself.
    static bool firstFrame = true;
    if (firstFrame) { firstFrame = false; Log("[tick] render hook alive - first frame presented"); }

    // In-game hotkeys (work in fullscreen): F8 = reload, F7 = toggle the overlay,
    // F9 = dump the server list right now (handy while the browser is on screen).
    static bool prevF8 = false, prevF7 = false, prevF9 = false;
    bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8 && !prevF8) RequestReload();
    prevF8 = f8;
    bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (f7 && !prevF7) { bool on = !g_overlayOn.load(); g_overlayOn.store(on); Log("[overlay] %s", on ? "shown" : "hidden"); }
    prevF7 = f7;
    bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9 && !prevF9) { Log("[tracks] F9 pressed - dumping track list"); DumpTrackList(); }
    prevF9 = f9;

    // If --dump-serverlist is active, auto-dump the blob whenever it changes - so
    // just opening the online browser captures it, no key press / console focus.
    // Also print a periodic status line so we can see WHY it stays empty: does the
    // handler ever fire, does the master state advance, are masters configured?
    if (g_origMpMsg) {
        static ULONGLONG lastCheck = 0, lastStatus = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastCheck > 1000) { lastCheck = now; DumpServerListBlob(false); }
        if (now - lastStatus > 3000) {
            lastStatus = now;
            int st = SafeReadInt((const int*)(g_base + mxb::RVA_MP_STATE));
            int ep = SafeReadInt((const int*)(g_base + mxb::RVA_MP_ENDPOINT_CNT));
            Log("[srvlist] status: handlerCalls=%d mpState=%d masterEndpoints=%d blobHead=%zuB",
                g_mpCalls.load(), st, ep, BlobHeadBytes());
        }
    }

    // Same, driven from the frostmod.exe console (R = reload).
    if (g_reloadEvent && WaitForSingleObject(g_reloadEvent, 0) == WAIT_OBJECT_0) {
        Log("[event] reload signal received from frostmod.exe");
        RequestReload();
    }
    if (g_dumpEvent && WaitForSingleObject(g_dumpEvent, 0) == WAIT_OBJECT_0) {
        Log("[srvlist] manual dump (D)"); DumpServerListBlob(true);
    }

    DrainGameThreadTasks();
    AdvanceReload();   // run at most one reload step, so a frame presents between steps
}

BOOL WINAPI hkSwapBuffers(HDC hdc)      { Tick(); return g_origSwapBuffers(hdc); }
BOOL WINAPI hkWglSwapBuffers(HDC hdc)   { Tick(); DrawOverlay(hdc); return g_origWglSwapBuffers(hdc); }

// (The old floating Win32 "Reload Mods" window was removed - reload is now driven
//  by the in-game overlay + F8 hotkey and the frostmod.exe console 'R'.)

// ---------------------------------------------------------------------------
// signature (AOB) validation - is offsets.h actually correct for THIS build?
//
// offsets.h ships RVAs recovered from one specific mxbikes.exe. A game update
// shifts them, and blindly hooking a stale address hooks the wrong code (or
// crashes). So instead of trusting the RVA, we verify the bytes at that RVA
// against the known signature; if they don't match we scan the module's
// executable sections for the pattern and use whatever we find. All of this is
// logged, so the console tells you plainly whether offsets.h fits your build.
// ---------------------------------------------------------------------------
bool MatchAt(const uint8_t* p, const char* pat, const char* mask) {
    for (; *mask; ++mask, ++p, ++pat)
        if (*mask == 'x' && (uint8_t)*pat != *p) return false;
    return true;
}

// bounds of the module's executable bytes, from its PE section headers
bool GetExecRange(uintptr_t base, uint8_t** begin, uint8_t** end) {
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    uintptr_t lo = ~(uintptr_t)0, hi = 0;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            uintptr_t s = base + sec->VirtualAddress;
            uintptr_t e = s + sec->Misc.VirtualSize;
            if (s < lo) lo = s;
            if (e > hi) hi = e;
        }
    }
    if (hi <= lo) return false;
    *begin = (uint8_t*)lo; *end = (uint8_t*)hi;
    return true;
}

uint8_t* PatternScan(uint8_t* begin, uint8_t* end, const char* pat, const char* mask) {
    size_t len = strlen(mask);                       // mask has no embedded NULs
    if (len == 0 || (size_t)(end - begin) < len) return nullptr;
    for (uint8_t* p = begin; p <= end - len; ++p)
        if (MatchAt(p, pat, mask)) return p;
    return nullptr;
}

// Resolve the scanner by signature. Returns its address (0 if unresolvable) and
// sets *outDelta to how far it moved from the offsets.h RVA (0 == offsets fit).
uintptr_t ResolveScanner(intptr_t* outDelta) {
    *outDelta = 0;
    uint8_t* expected = (uint8_t*)(g_base + mxb::RVA_SCAN_FOLDER);
    size_t   sigLen   = strlen(mxb::SIG_SCAN_FOLDER_MASK);

    uint8_t *b, *e;
    bool haveRange = GetExecRange(g_base, &b, &e);

    // only read the raw RVA if it actually lies inside an executable section
    bool rvaInRange = haveRange && expected >= b && expected + sigLen <= e;
    if (rvaInRange && MatchAt(expected, mxb::SIG_SCAN_FOLDER, mxb::SIG_SCAN_FOLDER_MASK)) {
        Log("[sig] scanner signature VERIFIED at RVA 0x%zx - offsets.h fits this build.",
            (size_t)mxb::RVA_SCAN_FOLDER);
        return (uintptr_t)expected;
    }
    if (!haveRange) {
        Log("[sig] WARNING: can't read module sections to validate; using the raw RVA "
            "0x%zx anyway (may be wrong).", (size_t)mxb::RVA_SCAN_FOLDER);
        return (uintptr_t)expected;
    }
    Log("[sig] WARNING: bytes at scanner RVA 0x%zx do NOT match the signature - "
        "offsets.h looks stale for this mxbikes build. Scanning .text...",
        (size_t)mxb::RVA_SCAN_FOLDER);
    uint8_t* found = PatternScan(b, e, mxb::SIG_SCAN_FOLDER, mxb::SIG_SCAN_FOLDER_MASK);
    if (!found) {
        Log("[sig] ERROR: scanner signature not found in .text. The reload can't work on "
            "this build until offsets.h/SIG_SCAN_FOLDER are updated. Skipping content hooks.");
        return 0;
    }
    *outDelta = (intptr_t)((uintptr_t)found - (g_base + mxb::RVA_SCAN_FOLDER));
    Log("[sig] scanner RELOCATED: found at RVA 0x%zx (delta %+lld from offsets.h).",
        (size_t)((uintptr_t)found - g_base), (long long)*outDelta);
    return (uintptr_t)found;
}

// If we inject EARLY (to get hooked before the game's one-time startup mods
// scan), the scanner's code may not be decrypted yet - SteamStub unpacks .text
// in place during early execution. The game can't run its scan until that code
// exists, so we spin until the signature appears, then hook: that lands our hook
// after unpack but (hopefully) before the game itself calls the scanner.
uintptr_t WaitForScanner(intptr_t* outDelta, DWORD timeoutMs) {
    *outDelta = 0;
    uint8_t* expected = (uint8_t*)(g_base + mxb::RVA_SCAN_FOLDER);
    size_t   sigLen   = strlen(mxb::SIG_SCAN_FOLDER_MASK);
    DWORD    start    = GetTickCount();
    bool     announced = false;

    for (;;) {
        uint8_t *b, *e;
        bool haveRange = GetExecRange(g_base, &b, &e);
        bool inRange = haveRange && expected >= b && expected + sigLen <= e;
        if (inRange && MatchAt(expected, mxb::SIG_SCAN_FOLDER, mxb::SIG_SCAN_FOLDER_MASK)) {
            if (announced)
                Log("[sig] scanner code decrypted after %lums; hooking now (before the scan, we hope).",
                    (unsigned long)(GetTickCount() - start));
            else
                Log("[sig] scanner signature VERIFIED at RVA 0x%zx - offsets.h fits this build.",
                    (size_t)mxb::RVA_SCAN_FOLDER);
            return (uintptr_t)expected;
        }
        if (GetTickCount() - start > timeoutMs) break;
        if (!announced) {
            Log("[sig] game code not decrypted yet (SteamStub); waiting to install content hooks...");
            announced = true;
        }
        Sleep(2);
    }
    // Timed out waiting on the exact RVA - fall back to a one-shot full resolve
    // (handles a relocated/updated build, or reports that it's truly absent).
    return ResolveScanner(outDelta);
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------
bool InstallHook(void* target, void* detour, void** original, const char* name) {
    if (MH_CreateHook(target, detour, original) != MH_OK) {
        Log("[hook] FAILED to create hook for %s @ %p", name, target);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Log("[hook] FAILED to enable hook for %s @ %p", name, target);
        return false;
    }
    Log("[hook] %s hooked @ %p", name, target);
    return true;
}

DWORD WINAPI Init(LPVOID) {
    // __DATE__/__TIME__ = when THIS dll was compiled. If this timestamp isn't
    // recent, you're running a stale frostmod.dll (rebuild failed to overwrite it,
    // usually because the game had it locked). Close the game before rebuilding.
    Log("=============== FrostMod v" FROSTMOD_VERSION " loading (dll built " __DATE__ " " __TIME__ ") ===============");

    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));  // mxbikes.exe
    Log("[init] module base = %p", (void*)g_base);

    if (MH_Initialize() != MH_OK) { Log("[init] MinHook init failed"); return 1; }

    // triggers shared with frostmod.exe: R = reload, D = dump server list.
    g_reloadEvent = CreateEventA(nullptr, FALSE /*auto-reset*/, FALSE, "Local\\FrostModReload");
    g_dumpEvent   = CreateEventA(nullptr, FALSE /*auto-reset*/, FALSE, "Local\\FrostModDumpNow");
    if (!g_reloadEvent) Log("[init] note: could not create reload event (%lu)", GetLastError());
    Log("[init] reload = re-run content load (fcn.1400ef210); press R / F8 to trigger.");

    // CONTENT hooks first and ASAP - they're timing-critical: we must be hooked
    // before the game's one-time startup mods scan. Wait for the code to be
    // decrypted (SteamStub), then hook.
    intptr_t delta = 0;
    uintptr_t scanAddr = WaitForScanner(&delta, 30000 /*ms*/);
    g_sigDelta = delta;   // share the build-drift with every other fixed-RVA consumer
    if (delta)
        Log("[sig] build drift detected: delta = 0x%zx (fixed RVAs are adjusted by this).",
            (size_t)delta);
    if (scanAddr) {
        InstallHook((void*)scanAddr, &hkScan, (void**)&g_origScan, "scanFolder");

        // Resolve the content-load routine (fcn.1400ef210) - the reload target. It has
        // no signature, so apply the same delta the scanner moved by (best-effort) and
        // only accept it if it lands inside .text.
        {
            uintptr_t ci = g_base + mxb::RVA_CONTENT_INIT + delta;
            uint8_t *cb, *ce;
            if (GetExecRange(g_base, &cb, &ce) && (uint8_t*)ci >= cb && (uint8_t*)ci < ce) {
                g_contentInit = (ContentInit_t)ci;
                Log("[init] content-load routine @ RVA 0x%zx - reload ready (R / F8).",
                    (size_t)(ci - g_base));
            } else {
                Log("[init] WARNING: content-load RVA 0x%zx outside .text; reload disabled.",
                    (size_t)mxb::RVA_CONTENT_INIT);
            }
        }

        // The registry-reset function has no signature in offsets.h, so we can't
        // verify it independently - we apply the same delta the scanner moved by
        // (correct if the update shifted .text uniformly; best-effort otherwise).
        uintptr_t resetAddr = g_base + mxb::RVA_REGISTRY_RESET + delta;
        if (delta)
            Log("[sig] NOTE: applying scanner delta %+lld to registryReset @ RVA 0x%zx "
                "(unverified - it has no signature).",
                (long long)delta, (size_t)(resetAddr - g_base));
        InstallHook((void*)resetAddr, &hkReset, (void**)&g_origReset, "registryReset");

        // OPT-IN PROBE: if frostmod.exe --probe-mount left the flag next to us, hook
        // the real .pkz-mount function (0x15a9e0, apply the same delta) to log how it's
        // called - so we can learn which arg is the path and build a real reload.
        char probe[MAX_PATH] = {0};
        if (g_logPath[0]) {
            strcpy_s(probe, g_logPath);
            if (char* s = strrchr(probe, '\\')) { *(s + 1) = 0; strcat_s(probe, "frostmod_probe.flag"); }
        }
        if (probe[0] && GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
            uintptr_t mountAddr = g_base + mxb::RVA_MOUNT_ONE_PKZ + delta;
            uint8_t *b2, *e2;
            if (GetExecRange(g_base, &b2, &e2) && (uint8_t*)mountAddr >= b2 && (uint8_t*)mountAddr < e2) {
                Log("[probe] --probe-mount active: hooking pkz-mount @ RVA 0x%zx to log its args.",
                    (size_t)(mountAddr - g_base));
                InstallHook((void*)mountAddr, &hkMount, (void**)&g_origMount, "mountPkz(0x15a9e0)");
            } else {
                Log("[probe] mount RVA 0x%zx is outside .text; not hooking.",
                    (size_t)mxb::RVA_MOUNT_ONE_PKZ);
            }
        }
    } else {
        Log("[init] content hooks NOT installed - reload is unavailable on this build "
            "until offsets.h is updated. (mods listing + logs still work.)");
    }

    // RENDER hooks (drive Tick: F8, the reload-event check, and running reloads on
    // the game thread). Not timing-critical for capture, so we can wait for
    // opengl32 to load - when injecting early it isn't mapped yet. gdi32 is always
    // present. Reload only runs much later (when you press R), by which point
    // these are installed.
    if (HMODULE gdi = GetModuleHandleA("gdi32.dll"))
        if (auto p = GetProcAddress(gdi, "SwapBuffers"))
            InstallHook((void*)p, &hkSwapBuffers,
                        (void**)&g_origSwapBuffers, "gdi32!SwapBuffers");

    HMODULE gl = nullptr;
    for (int i = 0; i < 3000 && !(gl = GetModuleHandleA("opengl32.dll")); ++i) Sleep(10);
    if (gl) {
        if (auto p = GetProcAddress(gl, "wglSwapBuffers"))
            InstallHook((void*)p, &hkWglSwapBuffers,
                        (void**)&g_origWglSwapBuffers, "opengl32!wglSwapBuffers");
    } else {
        Log("[init] note: opengl32 not loaded; relying on gdi32!SwapBuffers for the tick.");
    }

    // OPT-IN (frostmod.exe --dump-serverlist): hook the master opcode handler
    // (signature-validated, clean prologue) and dump the server-list blob once per
    // completed list, to learn its record format. Safe; off by default.
    {
        char flag[MAX_PATH] = {0};
        if (g_logPath[0]) {
            strcpy_s(flag, g_logPath);
            if (char* s = strrchr(flag, '\\')) { *(s + 1) = 0; strcat_s(flag, "frostmod_dumplist.flag"); }
        }
        if (flag[0] && GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES) {
            uint8_t *b3, *e3;
            bool ok = GetExecRange(g_base, &b3, &e3);
            uint8_t* mp = (uint8_t*)(g_base + mxb::RVA_MP_MSG_HANDLER);
            size_t   sl = strlen(mxb::SIG_MP_MSG_HANDLER_MASK);
            uint8_t* target = nullptr;
            if (ok && mp >= b3 && mp + sl <= e3 &&
                MatchAt(mp, mxb::SIG_MP_MSG_HANDLER, mxb::SIG_MP_MSG_HANDLER_MASK)) {
                target = mp;
                Log("[srvlist] master handler signature VERIFIED @ RVA 0x%zx.",
                    (size_t)mxb::RVA_MP_MSG_HANDLER);
            } else if (ok) {
                target = PatternScan(b3, e3, mxb::SIG_MP_MSG_HANDLER, mxb::SIG_MP_MSG_HANDLER_MASK);
                if (target) Log("[srvlist] master handler relocated to RVA 0x%zx.",
                                (size_t)((uintptr_t)target - g_base));
            }
            if (target) InstallHook((void*)target, &hkMpMsg, (void**)&g_origMpMsg, "masterMsg(0x2A10E0)");
            else        Log("[srvlist] master handler signature not found; not hooking.");
        }
    }

    // Server-browser spam filter: load rules from <dll folder>\frostmod_serverfilter.txt
    // (created with docs on first run). The actual hook that feeds entries in is
    // wired once the server-list function is RE'd - see the SERVER FILTER block above.
    {
        std::string cfg = g_logPath;
        if (size_t s = cfg.find_last_of("\\/"); s != std::string::npos)
            cfg = cfg.substr(0, s + 1) + "frostmod_serverfilter.txt";
        else
            cfg = "frostmod_serverfilter.txt";
        frostmod::serverfilter::Init(cfg, &SfLog);

        // OPT-IN (frostmod.exe --filter-servers): install the loop-top filter that logs
        // every server row and skips (hides) the ones matching the rules BEFORE the row
        // is created. Mid-function hook, so gated behind a flag. Scope is cheat/ad ghosts
        // by default (serverfilter config v3); see SB_SuppressRow.
        char fflag[MAX_PATH] = {0};
        if (g_logPath[0]) {
            strcpy_s(fflag, g_logPath);
            if (char* s = strrchr(fflag, '\\')) { *(s + 1) = 0; strcat_s(fflag, "frostmod_filter.flag"); }
        }
        if (fflag[0] && GetFileAttributesA(fflag) != INVALID_FILE_ATTRIBUTES)
            InstallServerFilterHook();
        else
            Log("[filter] rules loaded (inert). Run frostmod.exe --filter-servers to "
                "install the read-only dump hook and preview the server list ([srv]).");
    }

    Log("[init] ready%s. If loaded as a plugin (or injected before launch) watch for a "
        "[capture] scan line with ext='pkz' - that's the mods scan we need.",
        g_savePath[0] ? " (plugin mode)" : "");
    return 0;
}

// Start Init exactly once, whether we got here via DllMain (injected) or via the
// PiBoSo Startup() export (plugin). Guarded so the two paths can't double-init.
void EnsureInit() {
    bool expected = false;
    if (g_initStarted.compare_exchange_strong(expected, true))
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
}

} // namespace

// ---------------------------------------------------------------------------
// DLL entry - runs on ANY load (injected by frostmod.exe, or LoadLibrary'd by
// the game when we're a plugin). We kick off Init here so the injector path
// works with no plugin support on the game's side.
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_selfModule = hModule;
        InitLogPath(hModule);   // resolve <dll folder>\frostmod.log before we log
        EnsureInit();
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// PiBoSo plugin interface (MX Bikes). Placing frostmod.dll in the game's
// plugins folder makes the game load it at STARTUP and call these - which gets
// our hooks installed before the one-time mods scan, no injector needed. The
// game validates the plugin via GetModID + the two version numbers (must match
// this MX Bikes build: "mxbikes", data 8, interface 9 - from mxb_example.c).
// Optional data/telemetry callbacks are intentionally omitted; the game only
// calls the exports that exist. See docs/PLUGIN.md.
// ---------------------------------------------------------------------------
extern "C" {

__declspec(dllexport) char* GetModID() {
    static char id[] = "mxbikes";
    return id;
}
__declspec(dllexport) int GetModDataVersion()   { return 8; }
__declspec(dllexport) int GetInterfaceVersion() { return 9; }

// Called once at game startup. _szSavePath is the game's save/data folder.
// Return value = requested telemetry rate (3 = 10Hz); we don't use telemetry,
// but must return a valid rate to stay loaded. We (re)ensure our hooks are up.
__declspec(dllexport) int Startup(char* _szSavePath) {
    if (_szSavePath) strncpy_s(g_savePath, sizeof(g_savePath), _szSavePath, _TRUNCATE);
    if (g_selfModule) InitLogPath(g_selfModule);
    Log("=============== FrostMod plugin Startup() savePath='%s' ===============",
        g_savePath[0] ? g_savePath : "<null>");
    EnsureInit();
    return 3;
}

__declspec(dllexport) void Shutdown() {
    Log("[plugin] Shutdown() requested by game.");
    // MinHook hooks are torn down with the process; nothing required here.
}

} // extern "C"
