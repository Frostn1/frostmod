// ============================================================================
//  FrostMod - on-demand mods-folder reloader for MX Bikes.
//
//  MX Bikes scans its content folders once at startup; .pkz files added later
//  are ignored until a restart. FrostMod re-triggers the game's own content
//  scan (in-game F8 menu, or 'R' in frostmod.exe) so new tracks/skins/bikes
//  register live, with no loading screen.
//
//  The reload replays the content-load section of the game's boot routine one
//  step per frame on the render thread (see kReloadSteps / AdvanceReload), so
//  the VFS is never mutated while the game reads it and the overlay can show a
//  progress bar. See README + offsets.h for the RE details.
// ============================================================================

#include <winsock2.h>   // must precede <windows.h> so it doesn't pull in winsock1
#include <ws2tcpip.h>   // addrinfo (master-capture: --capture-master)
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
#include <algorithm>
#include <intrin.h>   // _ReturnAddress
#include <cmath>      // radar/ESP geometry (sinf/cosf/sqrtf/atan2f)
#include <GL/gl.h>    // immediate-mode GL overlay

// The Windows SDK ships an OpenGL 1.1 gl.h, so GL 2.0+ tokens aren't declared and
// glext.h isn't available. We only need this one as a glGetString() key (the
// [esp/diag] line that tells us fixed-function vs core-profile), so define the
// value rather than take on a new header dependency.
#ifndef GL_SHADING_LANGUAGE_VERSION
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#endif

#include "MinHook.h"
#include "offsets.h"
#include "serverfilter.h"
#include "version.h"    // FROSTMOD_VERSION

// ---------------------------------------------------------------------------
// logging -> <dll folder>\frostmod.log (+ OutputDebugString). Next to the dll,
// not %TEMP%, so the exe and injected dll agree on the file even when a
// Steam-launched game has a different %TEMP%. Falls back to %TEMP% if read-only.
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
char    g_modsPath[MAX_PATH] = {0};   // mods folder (from frostmod_mods.txt the launcher writes)
char    g_inactivePath[MAX_PATH] = {0}; // <MX Bikes>\FrostMod Inactive Tracks (deactivated .pkz store)

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

    // fall back to %TEMP% if InitLogPath hasn't run yet
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

// Every DISTINCT (dir, ext) scan the game made at startup. Diagnostic capture only
// (the reload uses kReloadSteps, not these): a0 (status)/a3 (out buf) were the
// game's stack buffers at capture time; a1 (dir)/a2 (ext) point into module data.
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

    // Record each DISTINCT (dir, ext) once; store all, but only LOG the first few
    // (the startup scan fires hundreds of times and would bury everything else).
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

// ---------------------------------------------------------------------------
// DIAGNOSTIC: capture bike-model (.edf) file opens + their CALL STACK.
// Opt-in via a frostmod_edfcap.flag file next to frostmod.log (see Init()).
//
// WHY: the garage bike PREVIEW only re-reads a swapped model.edf after the user
// switches bike class away and back - the surgical reload rebuilds the content
// CATALOGS but not the live preview instance, and static tracing couldn't pin the
// scene-level mesh loader or its identity/cache behaviour. Loose bike files are
// real disk opens, so hooking kernel32!CreateFileW/A and logging every '.edf' open
// WITH a stack walk answers both open questions from the log alone:
//   (A) swap a model then RE-SELECT the SAME bike (no class change): if NO new
//       [edf] line appears, the mesh is cached by identity -> the fix must
//       replicate away-and-back; if a line appears, a single re-select suffices.
//   (B) switch class away+back: the [edf] line's stack names the mesh loader (the
//       top mxbikes.exe RVA) and the "selected-bike-changed" caller above it.
// Read-only: we call the original CreateFile unchanged and only log.
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR,  DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileW_t    g_origCreateFileW = nullptr;
CreateFileA_t    g_origCreateFileA = nullptr;
std::atomic<int> g_edfShots{0};

// True if the path contains ".edf" (case-insensitive) - no allocation.
static bool PathHasEdf(const char* p) {
    for (const char* s = p; s[0]; ++s)
        if (s[0] == '.' && (s[1]|0x20) == 'e' && (s[2]|0x20) == 'd' && (s[3]|0x20) == 'f') return true;
    return false;
}

// Stack walk formatted as game-module RVAs (frames outside mxbikes.exe -> "[ext]").
// The first RVA is the game's own file-open wrapper; walk up to the mesh loader.
static std::string EdfStackChain() {
    static RtlCaptureStackBackTrace_t cap = []() -> RtlCaptureStackBackTrace_t {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        return nt ? (RtlCaptureStackBackTrace_t)GetProcAddress(nt, "RtlCaptureStackBackTrace") : nullptr;
    }();
    if (!cap) return "<no stackwalk>";
    void* frames[32] = {0};
    USHORT n = cap(2 /*skip EdfStackChain + the detour*/, 32, frames, nullptr);
    std::string chain; char b[48];
    for (USHORT i = 0; i < n; ++i) {
        uintptr_t a = (uintptr_t)frames[i], rva = a - g_base;
        bool inGame = (a >= g_base && rva < 0x1000000);          // mxbikes.exe is ~14MB
        _snprintf_s(b, sizeof(b), _TRUNCATE, "%s%s", i ? " <- " : "", inGame ? "" : "[ext]");
        chain += b;
        if (inGame) { _snprintf_s(b, sizeof(b), _TRUNCATE, "0x%zx", (size_t)rva); chain += b; }
    }
    return chain;
}

static void EdfLog(const char* path) {
    if (!path || !PathHasEdf(path)) return;
    static thread_local bool inHook = false;   // our Log()->fopen must not recurse in
    if (inHook) return;
    int n = g_edfShots.fetch_add(1);
    if (n >= 400) return;                       // cap the log; the capture is short-lived
    inHook = true;
    Log("[edf] #%d open '%s' <- %s", n + 1, path, EdfStackChain().c_str());
    inHook = false;
}

HANDLE WINAPI hkCreateFileW(LPCWSTR name, DWORD acc, DWORD shr, LPSECURITY_ATTRIBUTES sa,
                            DWORD disp, DWORD fl, HANDLE tmpl) {
    HANDLE h = g_origCreateFileW(name, acc, shr, sa, disp, fl, tmpl);
    if (name) {
        char narrow[MAX_PATH * 2] = {0};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        EdfLog(narrow);
    }
    return h;
}

HANDLE WINAPI hkCreateFileA(LPCSTR name, DWORD acc, DWORD shr, LPSECURITY_ATTRIBUTES sa,
                            DWORD disp, DWORD fl, HANDLE tmpl) {
    HANDLE h = g_origCreateFileA(name, acc, shr, sa, disp, fl, tmpl);
    EdfLog(name);
    return h;
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
    // first few calls + every state change (does opening the browser drive state to 3?)
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

// Log a hex+ASCII window of a byte buffer under a caller-chosen tag - so we can
// eyeball field layout (e.g. name @ +0x86, cap @ +0xC8) or a raw wire packet.
static void LogHexTag(const char* tag, const char* buf, size_t len) {
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
        Log("%s %s|%s|", tag, line, ascii);
    }
}
static void LogHexWindow(const char* buf, size_t len) { LogHexTag("[srv.hex]", buf, len); }

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

// Read the game's track array (RVA_TRACK_LIST, stride TRACK_STRIDE, count at
// RVA_TRACK_COUNT) and log each entry's fields. Confirms the array read and pins the
// folder/name offsets the switcher relies on.
void DumpTrackList() {
    int count = SafeReadInt((const int*)(g_base + mxb::RVA_TRACK_COUNT));
    Log("[tracks] ===== track list (count=%d, stride=%d) =====", count, mxb::TRACK_STRIDE);
    if (count <= 0 || count > 100000) {
        Log("[tracks] count looks wrong - the array/count offset may not fit this build.");
        return;
    }
    // RVA_TRACK_LIST is a qword POINTER to the heap array - deref it, then index.
    uintptr_t base = 0;
    SafeReadBytes((const char*)(g_base + mxb::RVA_TRACK_LIST), (char*)&base, sizeof(base));
    Log("[tracks] array ptr @ RVA 0x%zx = 0x%zx", (size_t)mxb::RVA_TRACK_LIST, (size_t)base);
    if (!base) { Log("[tracks] null array pointer - offset wrong for this build."); return; }

    const int shown = count < 80 ? count : 80;
    for (int i = 0; i < shown; ++i) {
        char* e = (char*)(base + (size_t)i * mxb::TRACK_STRIDE);
        char folder[80] = "", disp[80] = "", resolver[80] = "";
        SafeCopyStr(e + mxb::TRK_FOLDER,        folder,   sizeof(folder));
        SafeCopyStr(e + mxb::TRK_NAME,          disp,     sizeof(disp));
        SafeCopyStr(e + mxb::TRK_RESOLVER_NAME, resolver, sizeof(resolver));
        // These 3 fields are what the switcher uses: folder + resolver(+0x33C) go into
        // the session name-config; disp(+0x20) is what we show. Verify +0x33C here.
        Log("[tracks] #%03d folder='%s' | disp='%s' | resolver@0x33C='%s'",
            i, folder, disp, resolver);
    }
    if (count > shown) Log("[tracks] (+%d more not shown)", count - shown);
    Log("[tracks] ===== end (F9) =====");
}

// ---------------------------------------------------------------------------
// TRACK MANAGER (F8 menu > "Track manager") - activate/deactivate track .pkz files
// on disk so the mods folder stays lean. Active = under <mods>\tracks\**; inactive =
// moved out to <MX Bikes>\FrostMod Inactive Tracks (outside the scanned tree, so the
// game never loads it). Open the list, toggle rows, Apply -> the changed .pkz are
// moved between the two trees and the game reloads once. Everything runs on the
// render thread (Tick polls input, DrawOverlay draws it), so the list state below
// needs no lock.
// ---------------------------------------------------------------------------
// Collect *.pkz under `root`, returning paths RELATIVE to root (e.g. "motocross\X.pkz").
static void FindPkzRecursive(const std::string& root, const std::string& rel,
                             std::vector<std::string>& out) {
    std::string dir = rel.empty() ? root : (root + "\\" + rel);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;            // "." / ".."
        std::string childRel = rel.empty() ? fd.cFileName : (rel + "\\" + fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            FindPkzRecursive(root, childRel, out);
        } else {
            size_t L = strlen(fd.cFileName);
            if (L > 4 && _stricmp(fd.cFileName + L - 4, ".pkz") == 0) out.push_back(childRel);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void RequestReload();                          // fwd (defined with the reload code below)
void SetStatus(const char* s, unsigned ms);    // fwd (defined with the overlay below)

// One track in the manager list. `active` = current on-disk state (true => the .pkz
// lives under mods\tracks); `staged` = the state the user wants after Apply.
struct TrackEntry { std::string rel; bool active; bool staged; };   // rel = "motocross\\X.pkz"
static std::vector<TrackEntry> g_trk;          // combined active+inactive, sorted by rel
// The cursor/scroll index into g_trkView (the filtered list), NOT g_trk directly: with a
// search query active only the matching rows are visible, and the cursor walks those.
static std::vector<int> g_trkView;             // indices into g_trk matching g_trkQuery (visible rows)
static std::string g_trkQuery;                 // lowercase search text; empty = show every track
static bool g_trkSearch = false;               // true = typing into the search box (keys build g_trkQuery)
static int  g_trkCursor = 0;                   // highlighted row (index into g_trkView)
static int  g_trkTop    = 0;                   // first visible row (scroll offset, into g_trkView)
static std::atomic<bool> g_trkOpen{false};     // manager open? (Tick + DrawOverlay both read)
static const int kTrkVisible = 16;             // rows shown at once (the list scrolls)

// Track SWITCHER (F8 > 3): pick from the game's LOADED track array and switch the
// localhost/testing map to it. Distinct from the manager above (that moves files on
// disk); this lists what's currently loaded and re-enters the session on the chosen
// track. Names cached on open; the row index == the game's array index.
static std::vector<std::string> g_swNames;     // display names for the visible list
static int  g_swCursor = 0, g_swTop = 0;
static std::atomic<bool> g_swOpen{false};

// Direct connect (F8 > 6): type an IP[:port] and JOIN that server directly (no browser).
// A one-line text box like the track search; on Enter we write SB_CONNECT_TARGET and call
// the game's JOIN-initiator on the game thread. State is touched only on the render thread
// (Tick + both draw paths), like the switcher/manager state above.
static std::atomic<bool> g_dcOpen{false};      // direct-connect box open?
static std::string       g_dcInput;            // typed "ip[:port]" (digits . : only)
static std::string       g_dcError;            // last parse error (shown red); "" = none
static bool              g_dcPrimed = false;   // first-frame key latch done? (swallows the '6')
static const size_t      kDcMax = 21;          // "255.255.255.255:65535"

// Case-insensitive substring test. `needleLower` MUST already be lowercase (g_trkQuery
// is built lowercased); each haystack char is folded on the fly so there's no locale /
// tolower() dependency and no allocation.
static bool ContainsCI(const std::string& hay, const std::string& needleLower) {
    const size_t n = hay.size(), m = needleLower.size();
    if (m == 0) return true;
    if (m > n)  return false;
    for (size_t i = 0; i + m <= n; ++i) {
        size_t j = 0;
        for (; j < m; ++j) {
            char c = hay[i + j];
            if (c >= 'A' && c <= 'Z') c += 32;      // fold to lowercase
            if (c != needleLower[j]) break;
        }
        if (j == m) return true;
    }
    return false;
}

// Rebuild g_trkView (the visible rows) from g_trk + the current search query, then
// re-clamp the cursor/scroll into the new, possibly-shorter list. Called on open and
// after every query edit. Empty query => every row is visible.
static void RebuildTrackView() {
    g_trkView.clear();
    g_trkView.reserve(g_trk.size());
    for (int i = 0; i < (int)g_trk.size(); ++i)
        if (g_trkQuery.empty() || ContainsCI(g_trk[i].rel, g_trkQuery))
            g_trkView.push_back(i);
    const int vn = (int)g_trkView.size();
    if (g_trkCursor >= vn) g_trkCursor = vn ? vn - 1 : 0;   // keep cursor in range
    if (g_trkCursor < 0)   g_trkCursor = 0;
    g_trkTop = 0;                                            // filter change -> back to top
}

// (Re)scan both trees into g_trk and open the manager. Active tracks come from
// mods\tracks, inactive from the store; deduped by rel (a rel present in both is an
// anomaly -> keep the active copy). staged starts == active (no pending change).
void OpenTrackManager() {
    if (!g_modsPath[0]) {
        Log("[trklib] cannot open - mods path unknown (run frostmod.exe so it writes frostmod_mods.txt).");
        SetStatus("track manager: mods path unknown", 4000);
        return;
    }
    std::string tracksRoot = std::string(g_modsPath) + "\\tracks";
    std::vector<std::string> active, inactive;
    FindPkzRecursive(tracksRoot, "", active);
    if (g_inactivePath[0]) FindPkzRecursive(g_inactivePath, "", inactive);

    g_trk.clear();
    for (auto& r : active) g_trk.push_back({ r, true, true });
    for (auto& r : inactive) {
        bool dup = false;
        for (auto& e : g_trk) if (_stricmp(e.rel.c_str(), r.c_str()) == 0) { dup = true; break; }
        if (dup) { Log("[trklib] '%s' is BOTH active and inactive - keeping the active copy.", r.c_str()); continue; }
        g_trk.push_back({ r, false, false });
    }
    std::sort(g_trk.begin(), g_trk.end(), [](const TrackEntry& a, const TrackEntry& b) {
        return _stricmp(a.rel.c_str(), b.rel.c_str()) < 0;
    });

    g_trkCursor = 0; g_trkTop = 0;
    g_trkQuery.clear(); g_trkSearch = false;    // fresh open: no filter, not typing
    RebuildTrackView();                         // g_trkView = every row (query empty)
    g_trkOpen.store(true);
    Log("[trklib] track manager opened - %zu tracks (%zu active, %zu inactive).",
        g_trk.size(), active.size(), g_trk.size() - active.size());
}

void CloseTrackManager() { g_trkOpen.store(false); }

// Create every missing directory along a file's parent path (kernel32 only, so no
// shell32 dependency). e.g. "...\FrostMod Inactive Tracks\motocross\X.pkz" creates the
// "motocross" folder. Already-exists is ignored.
static void EnsureDirTree(const char* filePath) {
    char dir[MAX_PATH];
    strncpy_s(dir, filePath, _TRUNCATE);
    char* slash = strrchr(dir, '\\');
    if (!slash) return;
    *slash = 0;                                 // dir = parent folder
    for (char* p = dir; *p; ++p) {
        if (*p == '\\' && p != dir) { *p = 0; CreateDirectoryA(dir, nullptr); *p = '\\'; }
    }
    CreateDirectoryA(dir, nullptr);
}

// Apply the staged toggles: move each changed .pkz between mods\tracks and the store,
// then (only if something actually moved) reload once so the game re-scans. A move that
// fails (e.g. the .pkz is held open because you're on that track) is logged and skipped,
// never fatal, and its stage is reverted so the list stays truthful.
void ApplyTrackChanges() {
    std::string tracksRoot = std::string(g_modsPath) + "\\tracks";
    int moved = 0, failed = 0, changed = 0;
    for (auto& e : g_trk) {
        if (e.staged == e.active) continue;
        ++changed;
        std::string activePath   = tracksRoot + "\\" + e.rel;
        std::string inactivePath = std::string(g_inactivePath) + "\\" + e.rel;
        const std::string& src = e.active ? activePath : inactivePath;   // deactivate: out of active
        const std::string& dst = e.active ? inactivePath : activePath;   // activate:   into active
        EnsureDirTree(dst.c_str());
        if (MoveFileExA(src.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED)) {
            e.active = e.staged; ++moved;
            Log("[trklib] %s: %s", e.staged ? "ACTIVATED  " : "deactivated", e.rel.c_str());
        } else {
            e.staged = e.active; ++failed;                               // revert stage on failure
            Log("[trklib] MOVE FAILED (err %lu) for '%s' - skipped (file in use?).",
                GetLastError(), e.rel.c_str());
        }
    }

    CloseTrackManager();
    if (moved > 0) {
        Log("[trklib] applied: %d moved, %d failed - reloading.", moved, failed);
        RequestReload();                        // rebuild content lists live (its own status/bar)
        if (failed) SetStatus("track manager: some moves failed (see log)", 5000);
    } else if (changed > 0) {
        SetStatus("track manager: moves failed (see log)", 5000);
    } else {
        SetStatus("track manager: no changes", 3000);
    }
}

// ---------------------------------------------------------------------------
// TRACK SWITCHER load path (RE'd - see offsets.h RVA_TRK_*). fcn.1400BB510 loads the
// track named in the session config [0xE4B540] and re-enters the session; it takes
// NO args and re-derives the index by name-match, so we WRITE the name config (folder
// + resolver-name entry+0x33C) then call it ON THE GAME THREAD.
// ---------------------------------------------------------------------------
// POD + SEH: copy this entry's folder (+0x00) and resolver-name (+0x33C) into the
// session name config, exactly as the game does on a row-select.
static bool SB_WriteSessionCfg(const char* entry) {
    __try {
        char* cfg = (char*)(g_base + mxb::RVA_TRK_SESSION_CFG);
        const char* f = entry + mxb::TRK_FOLDER;
        const char* r = entry + mxb::TRK_RESOLVER_NAME;
        int i = 0; for (; i < 0x1F && f[i]; ++i) cfg[i]        = f[i]; cfg[i]        = 0; // +0x00 folder
        i = 0;     for (; i < 0x3F && r[i]; ++i) cfg[0x20 + i] = r[i]; cfg[0x20 + i] = 0; // +0x20 2nd name
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// POD + SEH: call the load-and-enter routine (no args). Runs on the game thread.
static void SB_CallLoadEnter() {
    __try { ((void (*)())(g_base + mxb::RVA_TRK_LOAD_ENTER))(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("[switch] load+enter FAULTED - caught (state may be wrong)."); }
}

// Live switch is OFF unless frostmod_trackswitch.flag exists (set by --switch-live).
// fcn.1400BB510 manipulates the testing-menu widgets, so calling it MID-RIDE crashes
// (the fault can also land a frame later, past our SEH). Default: read + log only, so
// F8>3>Enter is always safe; arm the flag to test the real switch FROM THE TESTING
// MENU (where those widgets exist).
bool g_swLiveLoad = false;

// Switch the localhost map to the track at `idx`. Always logs the fields it would use
// (so +0x33C can be verified safely); only writes the config + calls the loader when
// the live-load flag is armed.
void LoadTrackByIndex(int idx) {
    uintptr_t arr = 0;
    SafeReadBytes((const char*)(g_base + mxb::RVA_TRACK_LIST), (char*)&arr, sizeof(arr));
    if (!arr) { Log("[switch] no track array pointer."); return; }
    char* entry = (char*)(arr + (size_t)idx * mxb::TRACK_STRIDE);

    char folder[80] = "", disp[80] = "", resolver[80] = "";
    SafeCopyStr(entry + mxb::TRK_FOLDER,        folder,   sizeof(folder));
    SafeCopyStr(entry + mxb::TRK_NAME,          disp,     sizeof(disp));
    SafeCopyStr(entry + mxb::TRK_RESOLVER_NAME, resolver, sizeof(resolver));
    Log("[switch] request #%d: folder='%s' disp='%s' resolver@0x33C='%s'", idx, folder, disp, resolver);

    if (!g_swLiveLoad) {
        Log("[switch] live load DISABLED (safe). fcn.1400BB510 crashes mid-ride - it needs the "
            "testing-menu state. Arm with 'frostmod.exe --switch-live' and try FROM THE TESTING MENU.");
        SetStatus("switch preview only (see log; --switch-live to arm)", 6000);
        return;
    }
    if (!SB_WriteSessionCfg(entry)) { Log("[switch] couldn't write session config."); return; }
    Log("[switch] LIVE: cfg set (+0x00='%s', +0x20='%s'); queuing load+enter on the game thread...",
        folder, resolver);
    EnqueueGameThreadTask([] {
        Log("[switch] -> fcn.1400BB510 (load+enter)...");
        SB_CallLoadEnter();
        Log("[switch] load+enter returned.");
    });
    char st[128]; sprintf_s(st, "switching to %s...", disp[0] ? disp : folder);
    SetStatus(st, 8000);
}

// (Re)read the game's loaded track array and open the switcher.
void OpenSwitcher() {
    int count = SafeReadInt((const int*)(g_base + mxb::RVA_TRACK_COUNT));
    if (count <= 0 || count > 100000) { Log("[switch] track count looks wrong (%d).", count);
        SetStatus("switcher: no tracks", 4000); return; }
    uintptr_t arr = 0;
    SafeReadBytes((const char*)(g_base + mxb::RVA_TRACK_LIST), (char*)&arr, sizeof(arr));
    if (!arr) { Log("[switch] no track array pointer."); return; }

    g_swNames.clear();
    g_swNames.reserve(count);
    for (int i = 0; i < count; ++i) {
        char nm[80] = "";
        SafeCopyStr((const char*)(arr + (size_t)i * mxb::TRACK_STRIDE) + mxb::TRK_NAME, nm, sizeof(nm));
        g_swNames.push_back(nm[0] ? nm : "(unnamed)");
    }
    g_swCursor = 0; g_swTop = 0;
    g_swOpen.store(true);
    Log("[switch] switcher opened - %d tracks. Up/Down, Enter=load, Esc=close.", count);
}
void CloseSwitcher() { g_swOpen.store(false); }

// ---------------------------------------------------------------------------
// DIRECT CONNECT (F8 > 6) - join a server by IP:port without the browser.
// PREVIEW step: parse + validate the endpoint and log it. The actual connect is NOT
// wired yet - see offsets.h "direct connect": 0xE53DE0 is connect OUTPUT (reset-to-
// sentinel, packed binary addr), not a settable target, and the real initiator is the
// engine command bus [0x566C48] cmd 0x389, which needs live menu state and an r8 input
// whose layout is still unknown (runtime dump pending). So DoDirectConnect no-ops.
// ---------------------------------------------------------------------------

// Parse "ip[:port]" into a dotted IPv4 + port. Accepts exactly four 0..255 octets and
// an optional :port (1..65535, default MXB_DEFAULT_PORT). Digits and dots only - the
// input box never lets letters in, so hostnames aren't handled here (resolve first).
// On any malformed field returns false and points *err at a short reason.
static bool ParseEndpoint(const std::string& in, char* host, size_t hostCap,
                          uint16_t* port, const char** err) {
    *err = nullptr;
    std::string h = in, p;
    size_t colon = in.rfind(':');
    if (colon != std::string::npos) { h = in.substr(0, colon); p = in.substr(colon + 1); }
    if (h.empty()) { *err = "enter an IP (e.g. 1.2.3.4:54200)"; return false; }

    int octets = 0, val = -1, digits = 0;             // walk h; a trailing sentinel flushes octet 4
    for (size_t i = 0; i <= h.size(); ++i) {
        char c = (i < h.size()) ? h[i] : '.';
        if (c == '.') {
            if (digits == 0 || val > 255) { *err = "bad IPv4 address"; return false; }
            ++octets; val = -1; digits = 0;
        } else if (c >= '0' && c <= '9') {
            val = (val < 0 ? 0 : val) * 10 + (c - '0');
            if (++digits > 3) { *err = "bad IPv4 address"; return false; }
        } else { *err = "IP: digits and dots only"; return false; }
    }
    if (octets != 4) { *err = "IPv4 needs 4 octets"; return false; }

    long portv = mxb::MXB_DEFAULT_PORT;
    if (!p.empty()) {
        portv = 0;
        for (char c : p) {
            if (c < '0' || c > '9') { *err = "bad port"; return false; }
            portv = portv * 10 + (c - '0');
            if (portv > 65535) { *err = "port > 65535"; return false; }
        }
        if (portv < 1) { *err = "bad port"; return false; }
    }
    strncpy_s(host, hostCap, h.c_str(), _TRUNCATE);
    *port = (uint16_t)portv;
    return true;
}

// PREVIEW-only (see offsets.h "direct connect" - CORRECTED MODEL): the connect is NOT
// "write a struct + call a function". 0xE53DE0 is connect OUTPUT (reset-to-sentinel,
// packed binary addr), nothing reads it to drive a socket. The real initiator is the
// engine command bus [0x566C48] cmd 0x389 (JOIN handler ~0x0F0Exx), which needs the
// live bus + menu state and an r8 input whose layout is still unknown (runtime dump
// pending). So for now we validate + log the endpoint and DO NOT touch memory or
// dispatch anything. Wiring the bus call is the next step once the r8 format is known.
void DoDirectConnect(const char* host, uint16_t port) {
    Log("[connect] target %s:%u parsed OK - NOT dispatched. Connect goes via the engine "
        "command bus [RVA 0x%zx] cmd 0x%x (needs live menu state + the r8 input layout, "
        "RE pending); 0xE53DE0 is connect OUTPUT, not a settable target. Preview only.",
        host, port, (size_t)mxb::RVA_CMD_BUS_PTR, mxb::CMD_JOIN);
    char st[112]; sprintf_s(st, "direct connect: %s:%u parsed (dispatch pending RE - see log)", host, port);
    SetStatus(st, 6000);
}

// Enter pressed in the box: parse; on error keep the box open (so it can be fixed),
// otherwise close and fire the connect.
static void AttemptDirectConnect() {
    char host[16] = "";
    uint16_t port = 0;
    const char* err = nullptr;
    if (!ParseEndpoint(g_dcInput, host, sizeof(host), &port, &err)) {
        g_dcError = err ? err : "invalid";
        Log("[connect] rejected '%s': %s", g_dcInput.c_str(), g_dcError.c_str());
        return;
    }
    g_dcOpen.store(false);                 // close before firing (DoDirectConnect owns feedback)
    DoDirectConnect(host, port);
}

void OpenDirectConnect() {
    g_dcInput.clear();
    g_dcError.clear();
    g_dcPrimed = false;                    // first dc frame latches held keys (swallows the menu '6')
    g_dcOpen.store(true);
    Log("[connect] direct connect: type IP[:port], Enter=connect, Esc=cancel (default port %u).",
        (unsigned)mxb::MXB_DEFAULT_PORT);
}
void CloseDirectConnect() { g_dcOpen.store(false); }

// ---------------------------------------------------------------------------
// BIKE MODEL SWAP (F8 menu > 3) - swap a bike's model (its whole file set) for another.
// In MX Bikes a bike lives at <mods>\bikes\<Bike>\ as loose files. A "model" is the ENTIRE
// set of top-level files: model.edf (the mesh) PLUS its .hrc/.cfg lineup/alignment - those
// are tuned to that specific mesh, so the whole set travels together. Only the paints\
// subfolder (universal liveries) stays put across a swap. We keep a per-bike library of
// alternative models at <Bike>\FrostMod Models\<Variant>\ (each variant is a FOLDER holding
// a full file set) plus an "_active.txt" marker naming the live one. Swapping MOVES the
// current file set into the library (auto-backup => reversible) and MOVES the chosen
// variant's set into the bike root, then reloads. Invariant: the ACTIVE variant's files are
// the loose files in the bike root; every OTHER variant is a folder in the library.
// Two-level list (pick bike, then variant). State is touched only on the render thread
// (Tick + draw), like the other panels above - no lock needed.
// ---------------------------------------------------------------------------
static std::atomic<bool> g_msOpen{false};       // model-swap panel open?
static int   g_msLevel = 0;                      // 0 = bike picker, 1 = variant picker
static std::vector<std::string> g_msBikes;       // bike folder names that contain a model.edf
static int   g_msBikeCursor = 0, g_msBikeTop = 0;
static std::string g_msBike;                     // chosen bike folder name (at level 1)
static std::vector<std::string> g_msVars;        // variant names for g_msBike (row 0 = active)
static int   g_msVarCursor = 0, g_msVarTop = 0;
static const char* kMsLibDir   = "FrostMod Models";  // per-bike model library folder
static const char* kMsMarker   = "_active.txt";      // names the active variant
static const char* kMsOriginal = "Original";         // name given to the model captured on first swap

static std::string MsBikesRoot()                         { return std::string(g_modsPath) + "\\bikes"; }
static std::string MsBikeDir(const std::string& b)       { return MsBikesRoot() + "\\" + b; }
static std::string MsLibDir(const std::string& b)        { return MsBikeDir(b) + "\\" + kMsLibDir; }
static std::string MsActiveEdf(const std::string& b)     { return MsBikeDir(b) + "\\model.edf"; }
static std::string MsVariantDir(const std::string& b, const std::string& n) {
    return MsLibDir(b) + "\\" + n;
}
static bool MsFileExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool MsDirExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
// Top-level FILE names (not subfolders) directly inside `dir`. These are a bike's model
// files (model.edf + .hrc/.cfg); the paints\ / FrostMod Models\ subfolders are skipped.
static void MsListFiles(const std::string& dir, std::vector<std::string>& out) {
    out.clear();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
// Move each named file from srcDir to dstDir (created if needed). All-or-nothing: on the
// first failure the ones already moved are moved back and it returns false.
static bool MsMoveSet(const std::string& srcDir, const std::string& dstDir,
                      const std::vector<std::string>& files, const char* what) {
    EnsureDirTree((dstDir + "\\x").c_str());                  // ensure dstDir exists
    std::vector<std::string> done;
    for (const auto& f : files) {
        std::string s = srcDir + "\\" + f, d = dstDir + "\\" + f;
        if (MoveFileExA(s.c_str(), d.c_str(), MOVEFILE_COPY_ALLOWED)) { done.push_back(f); continue; }
        Log("[model] %s MOVE failed (err %lu): %s -> %s (model in use? exit the bike first).",
            what, GetLastError(), s.c_str(), d.c_str());
        for (const auto& g : done) {                          // undo the ones we already moved
            std::string bs = dstDir + "\\" + g, bd = srcDir + "\\" + g;
            MoveFileExA(bs.c_str(), bd.c_str(), MOVEFILE_COPY_ALLOWED);
        }
        return false;
    }
    return true;
}
// Read the active-variant marker; "" when the bike has never been swapped (its loose files
// are still the un-captured original set).
static std::string MsReadActive(const std::string& bike) {
    std::string mk = MsLibDir(bike) + "\\" + kMsMarker;
    FILE* f = nullptr; fopen_s(&f, mk.c_str(), "rb");
    if (!f) return "";
    char buf[128] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f);
    buf[n] = 0;
    size_t L = strlen(buf);
    while (L && (buf[L-1]=='\n' || buf[L-1]=='\r' || buf[L-1]==' ' || buf[L-1]=='\t')) buf[--L] = 0;
    return std::string(buf);
}
static void MsWriteActive(const std::string& bike, const std::string& name) {
    EnsureDirTree((MsLibDir(bike) + "\\x").c_str());     // ensure the library folder exists
    std::string mk = MsLibDir(bike) + "\\" + kMsMarker;
    FILE* f = nullptr; fopen_s(&f, mk.c_str(), "wb");
    if (!f) { Log("[model] could not write marker %s", mk.c_str()); return; }
    fwrite(name.c_str(), 1, name.size(), f); fclose(f);
}

// Scan <mods>\bikes for subfolders that are model-swappable, sorted. A bike qualifies if
// it has a loose model.edf (a real bike) OR a "FrostMod Models" library folder. The library
// check matters when the ACTIVE variant is an Original whose set has no loose model.edf (e.g.
// the stock mesh lives in a .pkz): without it, swapping back to Original would drop the bike
// off the list and leave its remaining variants unreachable.
static void MsScanBikes() {
    g_msBikes.clear();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((MsBikesRoot() + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::string bike = fd.cFileName;
        if (MsFileExists(MsActiveEdf(bike)) || MsDirExists(MsLibDir(bike)))
            g_msBikes.push_back(bike);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(g_msBikes.begin(), g_msBikes.end(),
              [](const std::string& a, const std::string& b){ return _stricmp(a.c_str(), b.c_str()) < 0; });
}

// Build the variant list for `bike`: row 0 is the ACTIVE variant (marker name, or
// "Original" if never swapped); the rest are the library's subfolders (the inactive ones).
static void MsScanVariants(const std::string& bike) {
    g_msVars.clear();
    std::string active = MsReadActive(bike);
    std::string activeLabel = active.empty() ? kMsOriginal : active;
    g_msVars.push_back(activeLabel);                          // row 0 = active
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((MsLibDir(bike) + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.') continue;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;     // variant = subfolder
            std::string name = fd.cFileName;
            if (_stricmp(name.c_str(), activeLabel.c_str()) == 0) continue;      // active is already row 0
            g_msVars.push_back(name);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (g_msVars.size() > 2)                                  // sort the inactive tail; keep active first
        std::sort(g_msVars.begin() + 1, g_msVars.end(),
                  [](const std::string& a, const std::string& b){ return _stricmp(a.c_str(), b.c_str()) < 0; });
    g_msVarCursor = 0; g_msVarTop = 0;
}

// Swap `bike`'s model (whole file set) to variant `target`. Auto-backs-up the current set
// into the library (reversible) and moves the target's set in; on any move failure rolls
// back and aborts without a broken bike. Reloads on success. Runs on the render thread.
static void MsApply(const std::string& bike, const std::string& target) {
    std::string active = MsReadActive(bike);
    std::string activeLabel = active.empty() ? kMsOriginal : active;
    if (_stricmp(target.c_str(), activeLabel.c_str()) == 0) {
        SetStatus("model swap: already active", 3000);
        return;
    }
    std::string rootDir   = MsBikeDir(bike);                    // <Bike>\ (holds the live set)
    std::string backupDir = MsVariantDir(bike, activeLabel);    // where the current set is parked
    std::string targetDir = MsVariantDir(bike, target);         // the variant to bring in
    if (!MsDirExists(targetDir) || !MsFileExists(targetDir + "\\model.edf")) {
        Log("[model] variant '%s' invalid (need %s\\model.edf).", target.c_str(), targetDir.c_str());
        SetStatus("model swap: variant missing model.edf (see log)", 5000);
        return;
    }
    std::vector<std::string> rootFiles, targetFiles;
    MsListFiles(rootDir,   rootFiles);                          // current model files to back up
    MsListFiles(targetDir, targetFiles);                       // variant files to bring in
    // 1) back up the current set into the library (all-or-nothing; MsMoveSet rolls itself back).
    if (!rootFiles.empty() && !MsMoveSet(rootDir, backupDir, rootFiles, "backup")) {
        SetStatus("model swap: files in use - exit the bike first", 6000);
        return;
    }
    // 2) move the variant's set into the bike root.
    if (!MsMoveSet(targetDir, rootDir, targetFiles, "activate")) {
        MsMoveSet(backupDir, rootDir, rootFiles, "rollback");  // restore the backed-up set
        SetStatus("model swap failed - rolled back (see log)", 6000);
        return;
    }
    MsWriteActive(bike, target);
    Log("[model] %s: model set '%s' -> '%s' (%zu files) - reloading.",
        bike.c_str(), activeLabel.c_str(), target.c_str(), targetFiles.size());
    SetStatus("model swapped - reloading", 4000);
    g_msOpen.store(false);
    RequestReload();
}

void OpenModelSwap() {
    if (!g_modsPath[0]) {
        Log("[model] cannot open - mods path unknown (run frostmod.exe so it writes frostmod_mods.txt).");
        SetStatus("model swap: mods path unknown", 4000);
        return;
    }
    MsScanBikes();
    g_msLevel = 0; g_msBikeCursor = 0; g_msBikeTop = 0;
    g_msBike.clear(); g_msVars.clear();
    g_msOpen.store(true);
    Log("[model] model swap opened - %zu bike(s) under %s.", g_msBikes.size(), MsBikesRoot().c_str());
}
void CloseModelSwap() { g_msOpen.store(false); }

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

// ===========================================================================
// RADAR + RIDER OUTLINES (ESP)  -  sanctioned PiBoSo telemetry
//
// Data comes entirely from the plugin API callbacks (implemented at the bottom
// of this file, next to Draw()): RunTelemetry (our world pos), RaceTrackPosition
// (EVERY rider's live world pos + yaw), RaceAddEntry/RaceClassification (race
// number -> name + laps-done, for the lap-status coloring). No memory reads.
//
// The one thing the plugin API does NOT give us is the camera view-projection
// matrix, which the on-screen outline needs to project a rider's world XYZ to a
// screen box. MX Bikes is OpenGL, so we capture it from the fixed-function GL
// pipeline (hkGlLoadMatrixf below). If capture ever fails validation, the ESP
// degrades to a screen-edge directional arrow that needs no matrix.
//
// CALIBRATION: a few world-axis / yaw-sign conventions can only be confirmed by
// running the game (the Windows tester box). They are isolated in the CAL block
// below so calibration is a one-line change; see docs/PLUGIN.md verification.
// ===========================================================================

// ---- SDK structs (verbatim prefix from mxb_example.c; we read only early, fixed
// fields, and every ingest guards on the reported _iDataSize) ----------------
struct SPluginsBikeData_t {          // RunTelemetry payload (local bike)
    int   m_iRPM;
    float m_fEngineTemperature, m_fWaterTemperature;
    int   m_iGear;
    float m_fFuel, m_fSpeedometer;
    float m_fPosX, m_fPosY, m_fPosZ; // world position  (all we read)
    // ... velocity / accel / rotation / controls follow; unused here.
};
struct SPluginsRaceTrackPosition_t { // RaceTrackPosition array element
    int   m_iRaceNum;
    float m_fPosX, m_fPosY, m_fPosZ;
    float m_fYaw;
    float m_fTrackPos;
    int   m_iCrashed;
};
struct SPluginsRaceAddEntry_t {      // RaceAddEntry payload
    int  m_iRaceNum;
    char m_szName[100], m_szBikeName[100], m_szBikeShortName[100], m_szCategory[100];
    int  m_iUnactive, m_iNumberOfGears, m_iMaxRPM;
};
struct SPluginsRaceClassification_t {      // RaceClassification header
    int m_iSession, m_iSessionState, m_iSessionTime, m_iNumEntries;
};
struct SPluginsRaceClassificationEntry_t { // RaceClassification array element
    int m_iRaceNum, m_iState, m_iBestLap, m_iBestLapNum, m_iNumLaps;
    int m_iGap, m_iGapLaps, m_iPenalty, m_iPit;
};

// ---- CALIBRATION knobs (VERIFY on the Windows tester) -----------------------
// Which two world axes form the horizontal (ground) plane, and which is "up".
// GL engines are commonly Y-up (ground = X,Z). If the radar looks squashed or a
// rider directly ahead lands sideways, this is the first thing to flip.
static inline void GroundUV(float x, float y, float z, float& u, float& v) {
    (void)y; u = x; v = z;            // ground plane = (X, Z); up = Y  [verify]
}
static constexpr float RAD_YAW_SIGN   = 1.0f;   // flip if the radar spins the wrong way
static constexpr float RAD_YAW_OFFSET = 0.0f;   // add if "straight ahead" isn't at the top

// ---- shared rider snapshot (producer = game data threads; consumer = render) -
static std::atomic<bool> g_radarOn{false};   // radar HUD panel
static std::atomic<bool> g_espOn{false};     // on-screen rider outlines
static float             g_radarRange = 80.0f; // metres shown edge-to-center

static std::mutex g_radMutex;                // guards everything below
struct RadRider { int raceNum; float x, y, z, yaw; int crashed; };
static bool     g_radHaveMe = false;
static float    g_radMeX = 0, g_radMeY = 0, g_radMeZ = 0;   // from RunTelemetry
static int      g_radN = 0;
static RadRider g_radRiders[64];             // from RaceTrackPosition
struct RadEntry { int raceNum; int numLaps; char name[24]; };
static int      g_radNEntries = 0;
static RadEntry g_radEntries[64];            // from RaceAddEntry / RaceClassification

static int RadFindEntry(int raceNum) {       // caller holds g_radMutex
    for (int i = 0; i < g_radNEntries; ++i)
        if (g_radEntries[i].raceNum == raceNum) return i;
    return -1;
}

// ---- ingest (called from the plugin callbacks; take the lock) ---------------
static void RadStoreTelemetry(const void* d, int size) {
    if (!d || size < (int)sizeof(SPluginsBikeData_t)) return;
    const auto* b = (const SPluginsBikeData_t*)d;
    std::lock_guard<std::mutex> lk(g_radMutex);
    g_radMeX = b->m_fPosX; g_radMeY = b->m_fPosY; g_radMeZ = b->m_fPosZ;
    g_radHaveMe = true;
}
static void RadStoreTrackPositions(int n, const void* arr, int elem) {
    if (!arr || elem <= 0) { std::lock_guard<std::mutex> lk(g_radMutex); g_radN = 0; return; }
    std::lock_guard<std::mutex> lk(g_radMutex);
    if (n < 0) n = 0; if (n > 64) n = 64;
    g_radN = 0;
    for (int i = 0; i < n; ++i) {
        const auto* e = (const SPluginsRaceTrackPosition_t*)((const char*)arr + (size_t)i * elem);
        RadRider& r = g_radRiders[g_radN++];
        r.raceNum = e->m_iRaceNum; r.x = e->m_fPosX; r.y = e->m_fPosY; r.z = e->m_fPosZ;
        r.yaw = e->m_fYaw; r.crashed = e->m_iCrashed;
    }
}
static void RadAddEntry(const void* d, int size) {
    if (!d || size < (int)sizeof(SPluginsRaceAddEntry_t)) return;
    const auto* a = (const SPluginsRaceAddEntry_t*)d;
    std::lock_guard<std::mutex> lk(g_radMutex);
    int i = RadFindEntry(a->m_iRaceNum);
    if (i < 0 && g_radNEntries < 64) { i = g_radNEntries++; g_radEntries[i].numLaps = 0; }
    if (i >= 0) { g_radEntries[i].raceNum = a->m_iRaceNum;
                  strncpy_s(g_radEntries[i].name, a->m_szName, _TRUNCATE); }
}
static void RadRemoveEntry(const void* d, int size) {
    if (!d || size < (int)sizeof(int)) return;
    int raceNum = *(const int*)d;            // payload starts with m_iRaceNum
    std::lock_guard<std::mutex> lk(g_radMutex);
    int i = RadFindEntry(raceNum);
    if (i >= 0) g_radEntries[i] = g_radEntries[--g_radNEntries];
}
static void RadStoreClassification(const void* arr, int n, int elem) {
    if (!arr || elem <= 0) return;
    std::lock_guard<std::mutex> lk(g_radMutex);
    if (n < 0) n = 0; if (n > 64) n = 64;
    for (int k = 0; k < n; ++k) {
        const auto* c = (const SPluginsRaceClassificationEntry_t*)((const char*)arr + (size_t)k * elem);
        int i = RadFindEntry(c->m_iRaceNum);
        if (i < 0 && g_radNEntries < 64) { i = g_radNEntries++; g_radEntries[i].name[0] = 0; }
        if (i >= 0) { g_radEntries[i].raceNum = c->m_iRaceNum; g_radEntries[i].numLaps = c->m_iNumLaps; }
    }
}
static void RadResetRace() {
    std::lock_guard<std::mutex> lk(g_radMutex);
    g_radN = 0; g_radNEntries = 0; g_radHaveMe = false;
}

// ---- persistence: frostmod_radar.cfg, next to frostmod.log ------------------
static void RadarSettingsPath(char* out, size_t n) {
    strncpy_s(out, n, g_logPath, _TRUNCATE);
    char* slash = strrchr(out, '\\');
    if (slash) slash[1] = 0; else out[0] = 0;
    strncat_s(out, n, "frostmod_radar.cfg", _TRUNCATE);
}
static void SaveRadarSettings() {
    char p[MAX_PATH]; RadarSettingsPath(p, sizeof(p)); if (!p[0]) return;
    FILE* f = nullptr; if (fopen_s(&f, p, "w") || !f) return;
    fprintf(f, "radar=%d\noutlines=%d\nrange=%d\n",
            g_radarOn.load() ? 1 : 0, g_espOn.load() ? 1 : 0, (int)g_radarRange);
    fclose(f);
}
static void LoadRadarSettings() {
    char p[MAX_PATH]; RadarSettingsPath(p, sizeof(p)); if (!p[0]) return;
    FILE* f = nullptr; if (fopen_s(&f, p, "r") || !f) return;
    char line[64]; int v;
    while (fgets(line, sizeof(line), f)) {
        if      (sscanf_s(line, "radar=%d",    &v) == 1) g_radarOn.store(v != 0);
        else if (sscanf_s(line, "outlines=%d", &v) == 1) g_espOn.store(v != 0);
        else if (sscanf_s(line, "range=%d",    &v) == 1 && v >= 10 && v <= 500) g_radarRange = (float)v;
    }
    fclose(f);
    Log("[radar] settings loaded: radar=%d outlines=%d range=%dm",
        g_radarOn.load(), g_espOn.load(), (int)g_radarRange);
}

// ---- consumer: a per-frame snapshot of the OTHER riders relative to me ------
// lap: +1 = they are lapping YOU (a lap ahead -> red), -1 = you are lapping them
// (a lap behind -> blue), 0 = same lap (white). rx/ry are heading-up radar-disc
// coords in [-1,1] (ry up = ahead). bearing is the world heading to them
// relative to my facing, for the ESP arrow fallback. wx/wy/wz = world pos.
struct RadBlip { float rx, ry, dist, bearing, wx, wy, wz, yaw; int lap; int raceNum; };
// Returns the count of OTHER riders (0..maxOut), or -1 if we have no "me" yet
// (no telemetry / no track positions). outMe* receive my world pos + heading.
static int RadBuildBlips(RadBlip* out, int maxOut, float rangeM,
                         float* outMeYaw, float* outMeX, float* outMeY, float* outMeZ) {
    std::lock_guard<std::mutex> lk(g_radMutex);
    if (!g_radHaveMe || g_radN == 0) return -1;
    // "me" = the RaceTrackPosition entry closest to my telemetry world pos.
    int me = -1; float best = 1e30f;
    for (int i = 0; i < g_radN; ++i) {
        float dx = g_radRiders[i].x - g_radMeX, dy = g_radRiders[i].y - g_radMeY,
              dz = g_radRiders[i].z - g_radMeZ;
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < best) { best = d2; me = i; }
    }
    if (me < 0) return -1;
    const float myYaw = g_radRiders[me].yaw;
    if (outMeYaw) *outMeYaw = myYaw;
    if (outMeX) *outMeX = g_radRiders[me].x;
    if (outMeY) *outMeY = g_radRiders[me].y;
    if (outMeZ) *outMeZ = g_radRiders[me].z;
    int myLaps = 0; { int ei = RadFindEntry(g_radRiders[me].raceNum);
                      if (ei >= 0) myLaps = g_radEntries[ei].numLaps; }
    float meu, mev; GroundUV(g_radRiders[me].x, g_radRiders[me].y, g_radRiders[me].z, meu, mev);
    const float a  = RAD_YAW_SIGN * myYaw + RAD_YAW_OFFSET;
    const float ca = cosf(a), sa = sinf(a);

    int count = 0;
    for (int i = 0; i < g_radN && count < maxOut; ++i) {
        if (i == me) continue;
        const RadRider& r = g_radRiders[i];
        float ru, rv; GroundUV(r.x, r.y, r.z, ru, rv);
        float du = ru - meu, dv = rv - mev;
        // rotate world delta into heading-up radar space (forward -> +ry/up)
        float rx =  du * ca - dv * sa;
        float ry =  du * sa + dv * ca;
        float dist = sqrtf(du*du + dv*dv);
        int lap = 0; { int ei = RadFindEntry(r.raceNum);
                       if (ei >= 0) { int l = g_radEntries[ei].numLaps;
                                      lap = (l > myLaps) ? +1 : (l < myLaps ? -1 : 0); } }
        RadBlip& b = out[count++];
        b.rx = rangeM > 0 ? rx / rangeM : 0; b.ry = rangeM > 0 ? ry / rangeM : 0;
        // clamp to the disc; riders past range sit on the rim
        float m = sqrtf(b.rx*b.rx + b.ry*b.ry);
        if (m > 1.0f) { b.rx /= m; b.ry /= m; }
        b.dist = dist; b.bearing = atan2f(rx, ry);   // 0 = ahead, +right
        b.wx = r.x; b.wy = r.y; b.wz = r.z; b.yaw = r.yaw; b.lap = lap; b.raceNum = r.raceNum;
    }
    return count;
}

// ---- lap-status colors (shared by both render paths) ------------------------
// same lap = white, they're lapping you = red, you're lapping them = blue.
static void LapColorRGB(int lap, float& r, float& g, float& b) {
    if (lap > 0)      { r = 1.00f; g = 0.32f; b = 0.32f; }   // red
    else if (lap < 0) { r = 0.40f; g = 0.70f; b = 1.00f; }   // blue
    else              { r = 0.94f; g = 0.96f; b = 1.00f; }   // white
}

// ---- camera view-projection capture (fixed-function OpenGL) -----------------
// The engine sets a perspective PROJECTION then loads the camera view as the
// first MODELVIEW before any per-object matrix. We snoop glMatrixMode +
// glLoadMatrixf to grab both, compose VP = Proj * View (column-major), and
// validate it each frame by projecting known riders. g_inOverlay suppresses
// capture while OUR overlay is drawing its own ortho matrices.
using glMatrixMode_t  = void (WINAPI*)(GLenum);
using glLoadMatrixf_t = void (WINAPI*)(const GLfloat*);
static glMatrixMode_t  g_origGlMatrixMode  = nullptr;
static glLoadMatrixf_t g_origGlLoadMatrixf = nullptr;

static std::atomic<bool> g_inOverlay{false};
static std::atomic<bool> g_vpValid{false};
static std::atomic<int>  g_glDiag{0};          // >0 => log the next N matrix loads once
static GLenum g_glMode = 0;
static bool   g_projPrimed = false;
static float  g_capProj[16], g_capView[16], g_vp[16];

static bool IsPerspectiveProj(const GLfloat* m) {   // GL column-major perspective
    return m[15] == 0.0f && m[11] < -0.5f && m[11] > -1.5f;
}
static void MatMul16(const float* a, const float* b, float* o) {   // o = a*b, column-major
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0; for (int k = 0; k < 4; ++k) s += a[k*4 + r] * b[c*4 + k];
            o[c*4 + r] = s;
        }
}
void WINAPI hkGlMatrixMode(GLenum mode) { g_glMode = mode; g_origGlMatrixMode(mode); }
void WINAPI hkGlLoadMatrixf(const GLfloat* m) {
    // glLoadMatrixf is hot; only do capture work when the outline feature (or the
    // one-shot diagnostic) actually needs it. Otherwise this is a cheap passthrough.
    if (m && !g_inOverlay.load(std::memory_order_relaxed)
        && (g_espOn.load(std::memory_order_relaxed) || g_glDiag.load(std::memory_order_relaxed))) {
        if (int d = g_glDiag.load(std::memory_order_relaxed)) {
            Log("[esp/diag] loadMatrix mode=0x%X persp=%d m11=%.3f m15=%.3f",
                g_glMode, (int)IsPerspectiveProj(m), m[11], m[15]);
            g_glDiag.store(d - 1, std::memory_order_relaxed);
        }
        if (g_glMode == GL_PROJECTION && IsPerspectiveProj(m)) {
            memcpy(g_capProj, m, sizeof(g_capProj)); g_projPrimed = true;
        } else if (g_glMode == GL_MODELVIEW && g_projPrimed) {
            memcpy(g_capView, m, sizeof(g_capView)); g_projPrimed = false;
            MatMul16(g_capProj, g_capView, g_vp);    // VP ready; validated per frame
        }
    }
    g_origGlLoadMatrixf(m);
}

// Raw projection through the captured VP (no validity gate). *sx,*sy = normalized
// screen (0..1, top-left origin); *w = clip w (view depth). Returns false if the
// point is at/behind the camera plane.
static bool VPProject01(float x, float y, float z, float* sx, float* sy, float* w) {
    const float* m = g_vp;
    float cx = m[0]*x + m[4]*y + m[8]*z  + m[12];
    float cy = m[1]*x + m[5]*y + m[9]*z  + m[13];
    float cw = m[3]*x + m[7]*y + m[11]*z + m[15];
    if (cw <= 0.0001f) return false;                 // behind camera
    *sx = (cx / cw) * 0.5f + 0.5f; *sy = 0.5f - (cy / cw) * 0.5f; *w = cw;
    return true;
}
// Public projector: only succeeds when the VP passed this frame's validation AND
// the point lands on-screen.
static bool WorldToScreen01(float x, float y, float z, float* sx, float* sy, float* w) {
    if (!g_vpValid.load(std::memory_order_relaxed)) return false;
    if (!VPProject01(x, y, z, sx, sy, w)) return false;
    return (*sx >= 0 && *sx <= 1 && *sy >= 0 && *sy <= 1);
}

// Validate the captured VP each frame: project my own world pos + require it to
// land on-screen with positive depth. Cheap gate; on failure ESP uses arrows.
static void RadValidateVP() {
    if (g_vp[15] == 0 && g_vp[0] == 0) { g_vpValid.store(false); return; }   // never captured
    float meYaw, mx, my, mz; RadBlip tmp[1];
    if (RadBuildBlips(tmp, 1, g_radarRange, &meYaw, &mx, &my, &mz) < 0) { g_vpValid.store(false); return; }
    float sx, sy, w;
    bool ok = VPProject01(mx, my + 1.0f, mz, &sx, &sy, &w) && w > 0
              && sx > -0.5f && sx < 1.5f && sy > -0.5f && sy < 1.5f;   // generous: I'm ~on screen
    g_vpValid.store(ok, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// in-game overlay - a corner hint drawn with immediate-mode GL inside the
// wglSwapBuffers hook, plus the F8 menu and a transient post-reload status line.
// On a core GL profile the fixed-function calls are no-ops (overlay stays hidden);
// everything is push/pop-wrapped so the game's GL state is never disturbed.
// ---------------------------------------------------------------------------
std::atomic<bool>      g_overlayOn{true};
std::atomic<bool>      g_menuOpen{false};       // F8 opens the FrostMod action menu
std::atomic<ULONGLONG> g_statusUntil{0};        // show g_statusText until this tick
std::mutex             g_statusMutex;
char                   g_statusText[128] = {0};

// The FrostMod menu (F8). One entry per action - press its key. New features add a
// row here instead of another global F-key. Keep labels short (they set the width).
struct MenuItem { char key; const char* label; };
static const MenuItem kMenu[] = {
    { '1', "Reload mods" },
    { '2', "Toggle this overlay" },
    { '3', "Bike model swap" },
    { '4', "Radar (riders around you)" },
    { '5', "Rider outlines" },
    // Hidden (code kept, not reachable from the menu): Track manager, Switch track,
    // Track list, Direct connect. Re-add a row here to expose one again.
};
static const int kMenuCount = (int)(sizeof(kMenu) / sizeof(kMenu[0]));

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

// The track-manager panel (top-left, like the menu but taller + scrolled). Cursor row
// is prefixed '>' and highlighted; [x]/[ ] is the STAGED active/inactive state; a
// trailing '*' (amber) marks a row whose staged state differs from disk (pending move).
static void DrawTrackManager(int w, int h, int lh) {
    const int total = (int)g_trk.size();          // all tracks (staged count is over these)
    const int vn    = (int)g_trkView.size();      // visible rows after the search filter
    if (g_trkCursor < g_trkTop)                g_trkTop = g_trkCursor;           // scroll up
    if (g_trkCursor >= g_trkTop + kTrkVisible) g_trkTop = g_trkCursor - kTrkVisible + 1; // down
    if (g_trkTop < 0) g_trkTop = 0;

    const int shown     = vn < kTrkVisible ? vn : kTrkVisible;
    const int listLines = vn == 0 ? 1 : shown;    // reserve a line for the "(no matches)" text
    const bool hasFind  = g_trkSearch || !g_trkQuery.empty();
    const int rows  = listLines + (hasFind ? 4 : 3);  // title [+search] + list + spacer + footer
    const int bw = 600, bh = rows * lh + 8;            // wide enough for the key-hint footer
    const int x0 = 10, x1 = x0 + bw, y1 = h - 10, y0 = y1 - bh;
    glColor4f(0.04f, 0.05f, 0.08f, 0.90f);
    FillRect(x0, y0, x1, y1);

    int pending = 0; for (auto& e : g_trk) if (e.staged != e.active) ++pending;
    int y = y1 - 17;
    glColor4f(0.47f, 0.78f, 1.0f, 1.0f);
    char title[112];
    sprintf_s(title, "FrostMod - Track Manager   (%d/%d shown, %d staged)", vn, total, pending);
    GlText(x0 + 8, y, title); y -= lh;

    // Search box: shown while typing OR whenever a filter is active. A blinking-free
    // caret '_' in search mode makes it obvious the manager is capturing keystrokes.
    if (hasFind) {
        char sline[160];
        sprintf_s(sline, "  Search: %s%s", g_trkQuery.c_str(), g_trkSearch ? "_" : "");
        if (g_trkSearch) glColor4f(0.55f, 0.95f, 0.75f, 1.0f);   // green while typing
        else             glColor4f(0.70f, 0.78f, 0.90f, 1.0f);   // dim once committed
        GlText(x0 + 8, y, sline); y -= lh;
    }

    if (vn == 0) {
        glColor4f(0.80f, 0.62f, 0.62f, 1.0f);
        GlText(x0 + 8, y, g_trkQuery.empty() ? "  (no tracks found)" : "  (no matches)");
        y -= lh;
    }
    for (int vi = g_trkTop; vi < g_trkTop + shown && vi < vn; ++vi) {
        const TrackEntry& e = g_trk[g_trkView[vi]];         // map view row -> real entry
        bool cur = (vi == g_trkCursor);
        if (cur) { glColor4f(0.47f, 0.78f, 1.0f, 0.18f); FillRect(x0 + 4, y - 3, x1 - 4, y + lh - 4); }
        char row[160];
        sprintf_s(row, "%s [%c] %s%s", cur ? ">" : " ", e.staged ? 'x' : ' ',
                  e.rel.c_str(), (e.staged != e.active) ? "  *" : "");
        if (e.staged != e.active) glColor4f(1.0f, 0.85f, 0.45f, 1.0f);   // amber = pending change
        else                      glColor4f(0.90f, 0.94f, 1.0f, 1.0f);
        GlText(x0 + 8, y, row); y -= lh;
    }

    y -= 2;
    glColor4f(0.6f, 0.66f, 0.76f, 1.0f);
    char foot[160];
    if (g_trkSearch)
        sprintf_s(foot, "  type to filter   Backspace del   Enter done   Esc clear   [%d matches]", vn);
    else
        sprintf_s(foot, "  Up/Down  Space toggle  A all  F find  Enter apply  Esc close   [%d-%d/%d]",
                  vn ? g_trkTop + 1 : 0, g_trkTop + shown, vn);
    GlText(x0 + 8, y, foot);
}

// The track switcher panel: scrolled list of the loaded tracks' display names; Enter
// loads the highlighted one into the localhost session.
static void DrawSwitcher(int w, int h, int lh) {
    const int n = (int)g_swNames.size();
    if (g_swCursor < g_swTop)                g_swTop = g_swCursor;
    if (g_swCursor >= g_swTop + kTrkVisible) g_swTop = g_swCursor - kTrkVisible + 1;
    if (g_swTop < 0) g_swTop = 0;

    const int shown = n < kTrkVisible ? n : kTrkVisible;
    const int rows  = shown + 3;
    const int bw = 480, bh = rows * lh + 8;
    const int x0 = 10, x1 = x0 + bw, y1 = h - 10, y0 = y1 - bh;
    glColor4f(0.04f, 0.05f, 0.08f, 0.90f);
    FillRect(x0, y0, x1, y1);

    int y = y1 - 17;
    glColor4f(0.47f, 0.78f, 1.0f, 1.0f);
    char title[96]; sprintf_s(title, "FrostMod - Switch Track (localhost)   (%d tracks)", n);
    GlText(x0 + 8, y, title); y -= lh;

    for (int i = g_swTop; i < g_swTop + shown && i < n; ++i) {
        bool cur = (i == g_swCursor);
        if (cur) { glColor4f(0.47f, 0.78f, 1.0f, 0.18f); FillRect(x0 + 4, y - 3, x1 - 4, y + lh - 4); }
        char row[160]; sprintf_s(row, "%s %s", cur ? ">" : " ", g_swNames[i].c_str());
        glColor4f(0.90f, 0.94f, 1.0f, 1.0f);
        GlText(x0 + 8, y, row); y -= lh;
    }

    y -= 2;
    glColor4f(0.6f, 0.66f, 0.76f, 1.0f);
    char foot[128];
    sprintf_s(foot, "  Up/Down move   Enter load   Esc cancel   [%d-%d/%d]",
              n ? g_swTop + 1 : 0, g_swTop + shown, n);
    GlText(x0 + 8, y, foot);
}

// The direct-connect panel: a single IP:port text line (green, with a caret) plus an
// optional red error line. Mirrors the track-search box styling.
static void DrawDirectConnect(int w, int h, int lh) {
    const bool hasErr = !g_dcError.empty();
    const int rows = hasErr ? 4 : 3;             // title + input [+ error] + footer
    const int bw = 420, bh = rows * lh + 8;
    const int x0 = 10, x1 = x0 + bw, y1 = h - 10, y0 = y1 - bh;
    glColor4f(0.04f, 0.05f, 0.08f, 0.90f);
    FillRect(x0, y0, x1, y1);

    int y = y1 - 17;
    glColor4f(0.47f, 0.78f, 1.0f, 1.0f);
    GlText(x0 + 8, y, "FrostMod - Direct Connect"); y -= lh;
    char inl[96]; sprintf_s(inl, "  IP:  %s_", g_dcInput.c_str());
    glColor4f(0.55f, 0.95f, 0.75f, 1.0f);        // green input line + caret
    GlText(x0 + 8, y, inl); y -= lh;
    if (hasErr) {
        char el[128]; sprintf_s(el, "  ! %s", g_dcError.c_str());
        glColor4f(0.95f, 0.55f, 0.55f, 1.0f);
        GlText(x0 + 8, y, el); y -= lh;
    }
    glColor4f(0.6f, 0.66f, 0.76f, 1.0f);
    GlText(x0 + 8, y, "  digits . :   Enter connect   Esc cancel");
}

// The model-swap panel: two-level scrolled list. Level 0 lists bikes; level 1 lists the
// chosen bike's variants with row 0 = the active one (green "(active)"). Switcher styling.
static void DrawModelSwap(int w, int h, int lh) {
    const bool lvl1 = (g_msLevel == 1);
    std::vector<std::string>& items = lvl1 ? g_msVars : g_msBikes;
    int& cursor = lvl1 ? g_msVarCursor : g_msBikeCursor;
    int& top    = lvl1 ? g_msVarTop    : g_msBikeTop;
    const int n = (int)items.size();
    if (cursor < top)                top = cursor;
    if (cursor >= top + kTrkVisible) top = cursor - kTrkVisible + 1;
    if (top < 0) top = 0;

    const int shown     = n < kTrkVisible ? n : kTrkVisible;
    const bool needHint = lvl1 && n <= 1;            // only the active variant => show "add .edf" hint
    const int listLines = n == 0 ? 1 : shown;
    const int rows = listLines + (needHint ? 4 : 3); // title + list [+ hint] + spacer + footer
    const int bw = 560, bh = rows * lh + 8;
    const int x0 = 10, x1 = x0 + bw, y1 = h - 10, y0 = y1 - bh;
    glColor4f(0.04f, 0.05f, 0.08f, 0.90f);
    FillRect(x0, y0, x1, y1);

    int y = y1 - 17;
    glColor4f(0.47f, 0.78f, 1.0f, 1.0f);
    char title[160];
    if (lvl1) sprintf_s(title, "FrostMod - Model Swap   %s   (%d)", g_msBike.c_str(), n);
    else      sprintf_s(title, "FrostMod - Model Swap   (%d bikes)", n);
    GlText(x0 + 8, y, title); y -= lh;

    if (n == 0) {
        glColor4f(0.80f, 0.62f, 0.62f, 1.0f);
        GlText(x0 + 8, y, "  (no bikes found under mods\\bikes)"); y -= lh;
    }
    for (int i = top; i < top + shown && i < n; ++i) {
        bool cur = (i == cursor);
        if (cur) { glColor4f(0.47f, 0.78f, 1.0f, 0.18f); FillRect(x0 + 4, y - 3, x1 - 4, y + lh - 4); }
        char row[220];
        if (lvl1) {
            bool active = (i == 0);                  // row 0 is the active variant
            sprintf_s(row, "%s %s%s", cur ? ">" : " ", items[i].c_str(), active ? "   (active)" : "");
            if (active) glColor4f(0.55f, 0.95f, 0.75f, 1.0f);
            else        glColor4f(0.90f, 0.94f, 1.0f, 1.0f);
        } else {
            sprintf_s(row, "%s %s", cur ? ">" : " ", items[i].c_str());
            glColor4f(0.90f, 0.94f, 1.0f, 1.0f);
        }
        GlText(x0 + 8, y, row); y -= lh;
    }
    if (needHint) {
        glColor4f(0.70f, 0.74f, 0.82f, 1.0f);
        GlText(x0 + 8, y, "  add a variant folder (model.edf + its .hrc/.cfg) under 'FrostMod Models'"); y -= lh;
    }

    y -= 2;
    glColor4f(0.6f, 0.66f, 0.76f, 1.0f);
    char foot[160];
    if (lvl1) sprintf_s(foot, "  Up/Down  Enter swap+reload  Esc back    [%d-%d/%d]",
                        n ? top + 1 : 0, top + shown, n);
    else      sprintf_s(foot, "  Up/Down  Enter choose bike  Esc close    [%d-%d/%d]",
                        n ? top + 1 : 0, top + shown, n);
    GlText(x0 + 8, y, foot);
}

// ---- radar + ESP, GL immediate-mode (used in injected/menu contexts) --------
static void GlCircle(int cx, int cy, int r, bool fill) {
    glBegin(fill ? GL_TRIANGLE_FAN : GL_LINE_LOOP);
    if (fill) glVertex2i(cx, cy);
    for (int i = 0; i <= 48; ++i) {
        float a = (float)i / 48.0f * 6.2831853f;
        glVertex2i(cx + (int)(cosf(a) * r), cy + (int)(sinf(a) * r));
    }
    glEnd();
}
static void GlTri(int x0,int y0,int x1,int y1,int x2,int y2) {
    glBegin(GL_TRIANGLES); glVertex2i(x0,y0); glVertex2i(x1,y1); glVertex2i(x2,y2); glEnd();
}
static void GlRectOutline(int x0,int y0,int x1,int y1,int t) {
    FillRect(x0,y0,x1,y0+t); FillRect(x0,y1-t,x1,y1);
    FillRect(x0,y0,x0+t,y1); FillRect(x1-t,y0,x1,y1);
}
static float ClampF(float v,float lo,float hi){ return v<lo?lo:(v>hi?hi:v); }

// The heading-up radar disc, top-right corner. Blips colored by lap status.
static void DrawRadarGL(int w, int h) {
    const int R = 92, M = 18, cx = w - M - R, cy = h - M - R;   // GL y-up: top-right
    glColor4f(0.03f,0.05f,0.09f,0.72f); GlCircle(cx,cy,R,true);
    glColor4f(0.45f,0.55f,0.72f,0.55f); GlCircle(cx,cy,R,false); GlCircle(cx,cy,R/2,false);
    glColor4f(0.30f,0.38f,0.52f,0.5f);
    FillRect(cx-R,cy-1,cx+R,cy+1); FillRect(cx-1,cy-R,cx+1,cy+R);
    glColor4f(0.55f,0.85f,1.0f,1.0f);                          // "you" arrow, points up
    GlTri(cx,cy+9, cx-6,cy-6, cx+6,cy-6);
    RadBlip blips[64]; float meYaw,mx,my,mz;
    int n = RadBuildBlips(blips,64,g_radarRange,&meYaw,&mx,&my,&mz);
    for (int i = 0; i < n; ++i) {
        float r,g,b; LapColorRGB(blips[i].lap,r,g,b); glColor4f(r,g,b,1.0f);
        int bx = cx + (int)(blips[i].rx * R), by = cy + (int)(blips[i].ry * R);
        FillRect(bx-3,by-3,bx+3,by+3);
    }
    glColor4f(0.6f,0.66f,0.76f,1.0f);
    char lbl[32]; sprintf_s(lbl,"RADAR %dm", (int)g_radarRange);
    GlText(cx-R, cy+R+14, lbl);
}

// On-screen rider outlines. Box when the VP projects them on-screen; otherwise a
// screen-edge directional arrow (needs no matrix). Colored by lap status.
static void DrawEspGL(int w, int h) {
    RadBlip blips[64]; float meYaw,mx,my,mz;
    int n = RadBuildBlips(blips,64,g_radarRange,&meYaw,&mx,&my,&mz);
    if (n < 0) return;
    const bool vp = g_vpValid.load(std::memory_order_relaxed);
    const int scx = w/2, scy = h/2;
    for (int i = 0; i < n; ++i) {
        float r,g,b; LapColorRGB(blips[i].lap,r,g,b);
        float sx,sy,wd;
        if (vp && WorldToScreen01(blips[i].wx, blips[i].wy, blips[i].wz, &sx,&sy,&wd)) {
            int px = (int)(sx*w), py = (int)((1.0f-sy)*h);      // 0..1 top-left -> GL y-up
            int hw = (int)ClampF(1400.0f/wd, 8.0f, 140.0f), hh = hw*2;
            glColor4f(r,g,b,0.95f); GlRectOutline(px-hw, py, px+hw, py+hh, 2);
        } else {
            // edge arrow: bearing 0 = ahead(up), +right. GL y-up so ahead = +y.
            float dx = sinf(blips[i].bearing), dy = cosf(blips[i].bearing);
            float rad = (float)(h < w ? h : w) * 0.34f;
            int ax = scx + (int)(dx*rad), ay = scy + (int)(dy*rad);
            int tx = (int)(dx*10), ty = (int)(dy*10), nx = (int)(-dy*6), ny = (int)(dx*6);
            glColor4f(r,g,b,0.9f);
            GlTri(ax+tx, ay+ty, ax-tx+nx, ay-ty+ny, ax-tx-nx, ay-ty-ny);
        }
    }
}

void DrawOverlay(HDC hdc) {
    // The menu always draws (even if the corner hint was toggled off), so it's never
    // possible to hide the overlay and lose the way back to it.
    if (!g_overlayOn.load() && !g_menuOpen.load() && !g_reloadActive.load()
        && !g_trkOpen.load() && !g_swOpen.load() && !g_dcOpen.load() && !g_msOpen.load()
        && !g_radarOn.load() && !g_espOn.load()) return;
    EnsureFont(hdc);

    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    const int w = vp[2], h = vp[3];
    if (w <= 0 || h <= 0) return;

    static unsigned frame = 0; ++frame;              // advances every presented frame
    const bool reloading = g_reloadActive.load();
    const bool menu      = g_menuOpen.load() && !reloading;
    const bool trk       = g_trkOpen.load() && !reloading;
    const bool sw        = g_swOpen.load()  && !reloading;
    const bool dc        = g_dcOpen.load()  && !reloading;
    const bool ms        = g_msOpen.load()  && !reloading;
    const int  done = g_reloadDone.load(), total = kReloadStepCount;
    const float frac = (reloading && total) ? (float)done / (float)total : 0.0f;

    char line[128];
    if (reloading) {
        static const char spin[4] = {'|', '/', '-', '\\'};
        sprintf_s(line, "%c  Reloading mods...  %d%%", spin[(frame / 3) % 4],
                  total ? (done * 100 / total) : 0);
    } else if (GetTickCount64() < g_statusUntil.load()) {
        std::lock_guard<std::mutex> lk(g_statusMutex);
        strncpy_s(line, g_statusText, _TRUNCATE);
    } else {
        strcpy_s(line, "FrostMod v" FROSTMOD_VERSION "   -   F8: menu");
    }

    g_inOverlay.store(true, std::memory_order_relaxed);   // don't let our ortho corrupt VP capture
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0, w, 0, h, -1, 1);                  // origin bottom-left
    glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);   glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const int lh = 18;                           // line height
    if (ms) {
        DrawModelSwap(w, h, lh);
    } else if (trk) {
        DrawTrackManager(w, h, lh);
    } else if (sw) {
        DrawSwitcher(w, h, lh);
    } else if (dc) {
        DrawDirectConnect(w, h, lh);
    } else if (menu) {
        // The action menu: title + one row per item + a footer. Press a row's key.
        const int rows = kMenuCount + 2;         // title + items + footer
        const int bw = 260, bh = rows * lh + 8;
        const int x0 = 10, x1 = x0 + bw, y1 = h - 10, y0 = y1 - bh;
        glColor4f(0.04f, 0.05f, 0.08f, 0.86f);
        FillRect(x0, y0, x1, y1);
        int y = y1 - 17;
        glColor4f(0.47f, 0.78f, 1.0f, 1.0f);
        GlText(x0 + 8, y, "FrostMod v" FROSTMOD_VERSION "  -  menu"); y -= lh;
        glColor4f(0.90f, 0.94f, 1.0f, 1.0f);
        for (int i = 0; i < kMenuCount; ++i) {
            char row[96]; sprintf_s(row, "  %c   %s", kMenu[i].key, kMenu[i].label);
            GlText(x0 + 8, y, row); y -= lh;
        }
        glColor4f(0.6f, 0.66f, 0.76f, 1.0f);
        GlText(x0 + 8, y, "  F8 / Esc   close");
    } else if (g_overlayOn.load() || reloading || GetTickCount64() < g_statusUntil.load()) {
        // compact pill: hint / status, or the reload progress bar
        const int bw = 250, bh = reloading ? 38 : 24;
        const int x0 = 10, x1 = x0 + bw, y1 = h - 10, y0 = y1 - bh;
        glColor4f(0.04f, 0.05f, 0.08f, 0.72f);
        FillRect(x0, y0, x1, y1);
        glColor4f(0.47f, 0.78f, 1.0f, 1.0f);         // FrostMod light-blue
        GlText(x0 + 8, y1 - 17, line);
        if (reloading) {                             // progress bar along the bottom
            const int bx0 = x0 + 8, bx1 = x1 - 8, by0 = y0 + 7, by1 = by0 + 6;
            glColor4f(1.0f, 1.0f, 1.0f, 0.18f); FillRect(bx0, by0, bx1, by1);
            glColor4f(0.47f, 0.78f, 1.0f, 0.95f);
            FillRect(bx0, by0, bx0 + (int)((bx1 - bx0) * frac), by1);
        }
    }

    if (g_espOn.load())   DrawEspGL(w, h);       // HUD overlays draw on top of any panel
    if (g_radarOn.load()) DrawRadarGL(w, h);

    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
    glPopAttrib();
    g_inOverlay.store(false, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Sanctioned overlay path: the PiBoSo Draw() callback (see the export below).
// On track/spectate/replay the game calls Draw() and renders the quads+strings
// we hand it - no GL, no SwapBuffers draw, resolution-independent. We mirror the
// same panels DrawOverlay() builds, but in the engine's normalized 0..1 space
// (top-left origin, ABGR colors). In menus - where Draw() is NOT called - and in
// injected mode, the GL DrawOverlay() path still runs; g_drawCalls lets the swap
// hook suppress the GL draw whenever Draw() is live, so there is never a double
// image. Input (Tick, via the swap hook) is unchanged and drives both paths.
// ---------------------------------------------------------------------------
std::atomic<uint64_t> g_drawCalls{0};   // ++ each Draw(); the swap hook watches it

// PiBoSo draw items (from mxb_example.c). The engine reads these AFTER Draw()
// returns, so the arrays handed back must persist - they are the static ones below.
struct SPluginQuad_t {
    float         m_aafPos[4][2];   // 4 corners, normalized 0..1, counter-clockwise
    int           m_iSprite;        // 1-based sprite; 0 = solid fill with m_ulColor
    unsigned long m_ulColor;        // ABGR
};
struct SPluginString_t {
    char          m_szString[100];
    float         m_afPos[2];       // normalized 0..1 (top-left origin)
    int           m_iFont;          // 1-based font index (1 = engine default)
    float         m_fSize;
    int           m_iJustify;       // 0 left, 1 center, 2 right
    unsigned long m_ulColor;        // ABGR
};

// Sized with headroom for the radar disc + blips and per-rider outline boxes on
// top of a panel; DQuad/DText bounds-check against these so overflow just drops.
static SPluginQuad_t   g_drawQuads[128];
static SPluginString_t g_drawStrs[128];
static int             g_nDrawQuads = 0, g_nDrawStrs = 0;

// our glColor4f palette -> ABGR (so both renderers share exactly one set of colors)
static unsigned long ToABGR(float r, float g, float b, float a) {
    auto c = [](float v) { int i = (int)(v * 255.0f + 0.5f); return (unsigned long)(i < 0 ? 0 : i > 255 ? 255 : i); };
    return (c(a) << 24) | (c(b) << 16) | (c(g) << 8) | c(r);
}

static void DQuad(float x0, float y0, float x1, float y1, unsigned long abgr) {
    if (g_nDrawQuads >= (int)(sizeof(g_drawQuads) / sizeof(g_drawQuads[0]))) return;
    SPluginQuad_t& q = g_drawQuads[g_nDrawQuads++];
    q.m_aafPos[0][0] = x0; q.m_aafPos[0][1] = y0;   // top-left
    q.m_aafPos[1][0] = x0; q.m_aafPos[1][1] = y1;   // bottom-left
    q.m_aafPos[2][0] = x1; q.m_aafPos[2][1] = y1;   // bottom-right
    q.m_aafPos[3][0] = x1; q.m_aafPos[3][1] = y0;   // top-right
    q.m_iSprite = 0;
    q.m_ulColor = abgr;
}

static void DText(float x, float y, const char* s, unsigned long abgr, float size) {
    if (g_nDrawStrs >= (int)(sizeof(g_drawStrs) / sizeof(g_drawStrs[0])) || !s || !*s) return;
    SPluginString_t& t = g_drawStrs[g_nDrawStrs++];
    strncpy_s(t.m_szString, s, _TRUNCATE);
    t.m_afPos[0] = x; t.m_afPos[1] = y;
    t.m_iFont = 1; t.m_fSize = size; t.m_iJustify = 0; t.m_ulColor = abgr;
}

// PiBoSo mirror of the radar/ESP HUD. Normalized 0..1, top-left origin; the disc
// is drawn in an aspect-corrected square (assume 16:9 - Draw() gives no resolution).
// Quad budget is shared (cap 64), so blips/boxes are capped to stay within it.
static constexpr float PB_ASPECT = 9.0f / 16.0f;   // narrow x so the disc isn't stretched
static void EmitRadarPiBoSo() {
    const float cx = 0.905f, cy = 0.135f, Ry = 0.105f, Rx = Ry * PB_ASPECT;
    DQuad(cx - Rx*1.12f, cy - Ry*1.12f, cx + Rx*1.12f, cy + Ry*1.12f, ToABGR(0.03f,0.05f,0.09f,0.72f));
    DQuad(cx - 0.004f*PB_ASPECT, cy - Ry, cx + 0.004f*PB_ASPECT, cy + Ry, ToABGR(0.30f,0.38f,0.52f,0.5f)); // v axis
    DQuad(cx - Rx, cy - 0.004f, cx + Rx, cy + 0.004f, ToABGR(0.30f,0.38f,0.52f,0.5f));                     // h axis
    DQuad(cx - 0.010f*PB_ASPECT, cy - 0.012f, cx + 0.010f*PB_ASPECT, cy + 0.006f, ToABGR(0.55f,0.85f,1.0f,1.0f)); // "you"
    RadBlip blips[64]; float meYaw,mx,my,mz;
    int n = RadBuildBlips(blips,64,g_radarRange,&meYaw,&mx,&my,&mz);
    int drawn = 0;
    for (int i = 0; i < n && drawn < 22; ++i) {
        float r,g,b; LapColorRGB(blips[i].lap,r,g,b);
        float bx = cx + blips[i].rx * Rx;
        float by = cy - blips[i].ry * Ry;         // top-left origin: ahead(+ry) -> up(-y)
        float s = 0.006f;
        DQuad(bx - s*PB_ASPECT, by - s, bx + s*PB_ASPECT, by + s, ToABGR(r,g,b,1.0f));
        ++drawn;
    }
    DText(cx - Rx, cy + Ry*1.12f + 0.006f, "RADAR", ToABGR(0.6f,0.66f,0.76f,1.0f), 0.017f);
}
static void EmitEspPiBoSo() {
    RadBlip blips[64]; float meYaw,mx,my,mz;
    int n = RadBuildBlips(blips,64,g_radarRange,&meYaw,&mx,&my,&mz);
    if (n < 0) return;
    const bool vp = g_vpValid.load(std::memory_order_relaxed);
    int boxes = 0;
    for (int i = 0; i < n && boxes < 10; ++i) {
        float r,g,b; LapColorRGB(blips[i].lap,r,g,b);
        unsigned long col = ToABGR(r,g,b,0.95f);
        float sx,sy,wd;
        if (vp && WorldToScreen01(blips[i].wx, blips[i].wy, blips[i].wz, &sx,&sy,&wd)) {
            float hh = ClampF(1.6f/wd, 0.012f, 0.16f), hw = hh * 0.5f * PB_ASPECT; // taller than wide
            float x0 = sx - hw, x1 = sx + hw, y0 = sy - hh, y1 = sy + hh, t = 0.0025f;
            DQuad(x0, y0, x1, y0+t, col); DQuad(x0, y1-t, x1, y1, col);      // hollow box (4 quads)
            DQuad(x0, y0, x0+t*PB_ASPECT, y1, col); DQuad(x1-t*PB_ASPECT, y0, x1, y1, col);
            ++boxes;
        } else {
            // fallback marker: a dot on a centered ring in the rider's direction
            float dx = sinf(blips[i].bearing), dy = -cosf(blips[i].bearing);  // ahead -> up
            float ax = 0.5f + dx*0.34f*PB_ASPECT, ay = 0.5f + dy*0.34f, s = 0.006f;
            DQuad(ax - s*PB_ASPECT, ay - s, ax + s*PB_ASPECT, ay + s, col);
            ++boxes;
        }
    }
}

// Fill g_drawQuads/g_drawStrs from the same overlay state DrawOverlay() reads.
// Normalized-coord mirror of DrawOverlay: quads are backgrounds/highlights/bars
// (no font needed), strings are labels (font index 1). Panel widths are fractions
// tuned to match the GL layout's proportions - adjust here if text overflows.
static void BuildOverlayDrawLists() {
    g_nDrawQuads = 0; g_nDrawStrs = 0;
    if (!g_overlayOn.load() && !g_menuOpen.load() && !g_reloadActive.load()
        && !g_trkOpen.load() && !g_swOpen.load() && !g_dcOpen.load() && !g_msOpen.load()
        && !g_radarOn.load() && !g_espOn.load()) return;

    // HUD overlays draw first (independent of the modal panel chain below), so the
    // panels' quads sit on top of them and both share the 64-quad budget.
    if (g_espOn.load())   EmitEspPiBoSo();
    if (g_radarOn.load()) EmitRadarPiBoSo();

    const bool reloading = g_reloadActive.load();
    const bool menu = g_menuOpen.load() && !reloading;
    const bool trk  = g_trkOpen.load()  && !reloading;
    const bool sw   = g_swOpen.load()   && !reloading;
    const bool dc   = g_dcOpen.load()   && !reloading;
    const bool ms   = g_msOpen.load()   && !reloading;
    const int  done = g_reloadDone.load(), total = kReloadStepCount;
    const float frac = (reloading && total) ? (float)done / (float)total : 0.0f;

    const float MX = 0.010f, MY = 0.014f;   // top-left anchor
    const float LH = 0.028f, FS = 0.021f;   // line height, font size (normalized)
    const float PADX = 0.006f;

    const unsigned long cPanel = ToABGR(0.04f, 0.05f, 0.08f, 0.90f);
    const unsigned long cBlue  = ToABGR(0.47f, 0.78f, 1.0f, 1.0f);
    const unsigned long cWhite = ToABGR(0.90f, 0.94f, 1.0f, 1.0f);
    const unsigned long cGray  = ToABGR(0.60f, 0.66f, 0.76f, 1.0f);
    const unsigned long cAmber = ToABGR(1.0f,  0.85f, 0.45f, 1.0f);
    const unsigned long cHi    = ToABGR(0.47f, 0.78f, 1.0f, 0.18f);

    if (ms) {
        const bool lvl1 = (g_msLevel == 1);
        std::vector<std::string>& items = lvl1 ? g_msVars : g_msBikes;
        int& cursor = lvl1 ? g_msVarCursor : g_msBikeCursor;
        int& top    = lvl1 ? g_msVarTop    : g_msBikeTop;
        const int n = (int)items.size();
        if (cursor < top)                top = cursor;
        if (cursor >= top + kTrkVisible) top = cursor - kTrkVisible + 1;
        if (top < 0) top = 0;
        const int  shown     = n < kTrkVisible ? n : kTrkVisible;
        const bool needHint  = lvl1 && n <= 1;
        const int  listLines = n == 0 ? 1 : shown;
        const int  rows      = listLines + (needHint ? 4 : 3);
        const float w = 0.44f, h = rows * LH + 0.010f;
        DQuad(MX, MY, MX + w, MY + h, cPanel);
        float y = MY + 0.006f;
        char title[160];
        if (lvl1) sprintf_s(title, "FrostMod - Model Swap   %s   (%d)", g_msBike.c_str(), n);
        else      sprintf_s(title, "FrostMod - Model Swap   (%d bikes)", n);
        DText(MX + PADX, y, title, cBlue, FS); y += LH;
        if (n == 0) {
            DText(MX + PADX, y, "  (no bikes found under mods\\bikes)", ToABGR(0.80f, 0.62f, 0.62f, 1.0f), FS);
            y += LH;
        }
        for (int i = top; i < top + shown && i < n; ++i) {
            bool cur = (i == cursor);
            if (cur) DQuad(MX + 0.003f, y - 0.002f, MX + w - 0.003f, y + LH - 0.004f, cHi);
            char row[220];
            if (lvl1) {
                bool active = (i == 0);
                sprintf_s(row, "%s %s%s", cur ? ">" : " ", items[i].c_str(), active ? "   (active)" : "");
                DText(MX + PADX, y, row, active ? ToABGR(0.55f, 0.95f, 0.75f, 1.0f) : cWhite, FS);
            } else {
                sprintf_s(row, "%s %s", cur ? ">" : " ", items[i].c_str());
                DText(MX + PADX, y, row, cWhite, FS);
            }
            y += LH;
        }
        if (needHint) {
            DText(MX + PADX, y, "  add a variant folder (model.edf + .hrc/.cfg) under 'FrostMod Models'",
                  ToABGR(0.70f, 0.74f, 0.82f, 1.0f), FS);
            y += LH;
        }
        y += 0.004f;
        char foot[160];
        if (lvl1) sprintf_s(foot, "  Up/Down  Enter swap+reload  Esc back   [%d-%d/%d]",
                            n ? top + 1 : 0, top + shown, n);
        else      sprintf_s(foot, "  Up/Down  Enter choose bike  Esc close   [%d-%d/%d]",
                            n ? top + 1 : 0, top + shown, n);
        DText(MX + PADX, y, foot, cGray, FS);
    } else if (trk) {
        const int total_t = (int)g_trk.size();
        const int vn      = (int)g_trkView.size();
        if (g_trkCursor < g_trkTop)                g_trkTop = g_trkCursor;
        if (g_trkCursor >= g_trkTop + kTrkVisible) g_trkTop = g_trkCursor - kTrkVisible + 1;
        if (g_trkTop < 0) g_trkTop = 0;
        const int  shown     = vn < kTrkVisible ? vn : kTrkVisible;
        const bool hasFind   = g_trkSearch || !g_trkQuery.empty();
        const int  listLines = vn == 0 ? 1 : shown;
        const int  rows      = listLines + (hasFind ? 4 : 3);
        const float w = 0.46f, h = rows * LH + 0.010f;
        DQuad(MX, MY, MX + w, MY + h, cPanel);
        int pending = 0; for (auto& e : g_trk) if (e.staged != e.active) ++pending;
        float y = MY + 0.006f;
        char title[112];
        sprintf_s(title, "FrostMod - Track Manager   (%d/%d shown, %d staged)", vn, total_t, pending);
        DText(MX + PADX, y, title, cBlue, FS); y += LH;
        if (hasFind) {
            char sline[160]; sprintf_s(sline, "  Search: %s%s", g_trkQuery.c_str(), g_trkSearch ? "_" : "");
            DText(MX + PADX, y, sline, g_trkSearch ? ToABGR(0.55f, 0.95f, 0.75f, 1.0f)
                                                   : ToABGR(0.70f, 0.78f, 0.90f, 1.0f), FS);
            y += LH;
        }
        if (vn == 0) {
            DText(MX + PADX, y, g_trkQuery.empty() ? "  (no tracks found)" : "  (no matches)",
                  ToABGR(0.80f, 0.62f, 0.62f, 1.0f), FS);
            y += LH;
        }
        for (int vi = g_trkTop; vi < g_trkTop + shown && vi < vn; ++vi) {
            const TrackEntry& e = g_trk[g_trkView[vi]];
            bool cur = (vi == g_trkCursor);
            if (cur) DQuad(MX + 0.003f, y - 0.002f, MX + w - 0.003f, y + LH - 0.004f, cHi);
            char row[160];
            sprintf_s(row, "%s [%c] %s%s", cur ? ">" : " ", e.staged ? 'x' : ' ',
                      e.rel.c_str(), (e.staged != e.active) ? "  *" : "");
            DText(MX + PADX, y, row, (e.staged != e.active) ? cAmber : cWhite, FS); y += LH;
        }
        y += 0.004f;
        char foot[160];
        if (g_trkSearch)
            sprintf_s(foot, "  type to filter   Backspace del   Enter done   Esc clear   [%d matches]", vn);
        else
            sprintf_s(foot, "  Up/Down  Space toggle  A all  F find  Enter apply  Esc close   [%d-%d/%d]",
                      vn ? g_trkTop + 1 : 0, g_trkTop + shown, vn);
        DText(MX + PADX, y, foot, cGray, FS);
    } else if (sw) {
        const int n = (int)g_swNames.size();
        if (g_swCursor < g_swTop)                g_swTop = g_swCursor;
        if (g_swCursor >= g_swTop + kTrkVisible) g_swTop = g_swCursor - kTrkVisible + 1;
        if (g_swTop < 0) g_swTop = 0;
        const int shown = n < kTrkVisible ? n : kTrkVisible;
        const int rows  = shown + 3;
        const float w = 0.36f, h = rows * LH + 0.010f;
        DQuad(MX, MY, MX + w, MY + h, cPanel);
        float y = MY + 0.006f;
        char title[96]; sprintf_s(title, "FrostMod - Switch Track (localhost)   (%d tracks)", n);
        DText(MX + PADX, y, title, cBlue, FS); y += LH;
        for (int i = g_swTop; i < g_swTop + shown && i < n; ++i) {
            bool cur = (i == g_swCursor);
            if (cur) DQuad(MX + 0.003f, y - 0.002f, MX + w - 0.003f, y + LH - 0.004f, cHi);
            char row[160]; sprintf_s(row, "%s %s", cur ? ">" : " ", g_swNames[i].c_str());
            DText(MX + PADX, y, row, cWhite, FS); y += LH;
        }
        y += 0.004f;
        char foot[128];
        sprintf_s(foot, "  Up/Down move   Enter load   Esc cancel   [%d-%d/%d]",
                  n ? g_swTop + 1 : 0, g_swTop + shown, n);
        DText(MX + PADX, y, foot, cGray, FS);
    } else if (dc) {
        const bool hasErr = !g_dcError.empty();
        const int rows = hasErr ? 4 : 3;
        const float w = 0.32f, h = rows * LH + 0.010f;
        DQuad(MX, MY, MX + w, MY + h, cPanel);
        float y = MY + 0.006f;
        DText(MX + PADX, y, "FrostMod - Direct Connect", cBlue, FS); y += LH;
        char inl[96]; sprintf_s(inl, "  IP:  %s_", g_dcInput.c_str());
        DText(MX + PADX, y, inl, ToABGR(0.55f, 0.95f, 0.75f, 1.0f), FS); y += LH;
        if (hasErr) {
            char el[128]; sprintf_s(el, "  ! %s", g_dcError.c_str());
            DText(MX + PADX, y, el, ToABGR(0.95f, 0.55f, 0.55f, 1.0f), FS); y += LH;
        }
        DText(MX + PADX, y, "  digits . :   Enter connect   Esc cancel", cGray, FS);
    } else if (menu) {
        const int rows = kMenuCount + 2;
        const float w = 0.22f, h = rows * LH + 0.010f;
        DQuad(MX, MY, MX + w, MY + h, ToABGR(0.04f, 0.05f, 0.08f, 0.86f));
        float y = MY + 0.006f;
        DText(MX + PADX, y, "FrostMod v" FROSTMOD_VERSION "  -  menu", cBlue, FS); y += LH;
        for (int i = 0; i < kMenuCount; ++i) {
            char row[96]; sprintf_s(row, "  %c   %s", kMenu[i].key, kMenu[i].label);
            DText(MX + PADX, y, row, cWhite, FS); y += LH;
        }
        DText(MX + PADX, y, "  F8 / Esc   close", cGray, FS);
    } else if (g_overlayOn.load() || reloading || GetTickCount64() < g_statusUntil.load()) {
        char line[128];
        if (reloading) {
            static const char spin[4] = {'|', '/', '-', '\\'};
            unsigned f = (unsigned)g_drawCalls.load();
            sprintf_s(line, "%c  Reloading mods...  %d%%", spin[(f / 3) % 4], total ? (done * 100 / total) : 0);
        } else if (GetTickCount64() < g_statusUntil.load()) {
            std::lock_guard<std::mutex> lk(g_statusMutex);
            strncpy_s(line, g_statusText, _TRUNCATE);
        } else {
            strcpy_s(line, "FrostMod v" FROSTMOD_VERSION "   -   F8: menu");
        }
        const float w = 0.20f, h = reloading ? 0.060f : 0.034f;
        DQuad(MX, MY, MX + w, MY + h, ToABGR(0.04f, 0.05f, 0.08f, 0.72f));
        DText(MX + PADX, MY + 0.006f, line, cBlue, FS);
        if (reloading) {
            const float bx0 = MX + PADX, bx1 = MX + w - PADX, by0 = MY + h - 0.014f, by1 = by0 + 0.008f;
            DQuad(bx0, by0, bx1, by1, ToABGR(1.0f, 1.0f, 1.0f, 0.18f));
            DQuad(bx0, by0, bx0 + (bx1 - bx0) * frac, by1, ToABGR(0.47f, 0.78f, 1.0f, 0.95f));
        }
    }
}

// ---------------------------------------------------------------------------
// MASTER CAPTURE  (opt-in: frostmod.exe --capture-master)
//
// Reverse-engineer the master protocol before building the mimic master server.
// We hook the ws2_32 EXPORTS (standard, stable signatures - offset-drift-proof,
// unlike the game's internal net wrappers, and no signature to author) and log
// ONLY the datagrams to/from the master: UDP 54200, or the IP master.mx-bikes.com
// resolved to. Read-only - every packet passes through untouched.
//
// Works even while the real master is DOWN: the client still SENDs GETLIST (~3s
// cadence) and a local `mxbikes.exe --dedicated` still SENDs REGISTER - both are
// captured on the outbound side regardless of any reply. The HOSTED reply format
// is captured later against our own daemon during bring-up.
//
// If the game uses WSASendTo/WSARecvFrom instead of classic sendto/recvfrom, the
// [cap] getaddrinfo line still fires (the host resolves) but no [cap] SEND/RECV
// will - that's the signal to add the WSA variants.
// ---------------------------------------------------------------------------
static const uint16_t kMasterPort = 54200;   // 0xD3B8 - the master server-list port
std::atomic<uint32_t> g_capMasterIp{0};       // master IPv4 as stored in sockaddr (net order); 0 = unknown
std::atomic<int>      g_capLeft{300};          // cap total [cap] dumps so the log can't run away

using sendto_t      = int (WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using recvfrom_t    = int (WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
using getaddrinfo_t = int (WSAAPI*)(const char*, const char*, const addrinfo*, addrinfo**);
static sendto_t      g_origSendto      = nullptr;
static recvfrom_t    g_origRecvfrom    = nullptr;
static getaddrinfo_t g_origGetaddrinfo = nullptr;

// AF_INET sockaddr parsed by hand, so we need no ws2_32 calls (no WSAStartup/link):
//   +0 family(u16 host order)  +2 port(u16 big-endian)  +4 addr(4 bytes, net order)
static bool CapIsMaster(const void* sa) {
    if (!sa) return false;
    const unsigned char* p = (const unsigned char*)sa;
    if ((uint16_t)(p[0] | (p[1] << 8)) != AF_INET) return false;
    uint16_t port = (uint16_t)((p[2] << 8) | p[3]);
    if (port == kMasterPort) return true;
    uint32_t ip; memcpy(&ip, p + 4, 4);
    uint32_t m = g_capMasterIp.load();
    return m && ip == m;
}

static void CapLog(const char* dir, const void* sa, const char* buf, int len) {
    if (g_capLeft.load() <= 0) return;
    g_capLeft.fetch_sub(1);
    const unsigned char* p = (const unsigned char*)sa;
    uint16_t port = (uint16_t)((p[2] << 8) | p[3]);
    Log("[cap] %s %u.%u.%u.%u:%u  len=%d", dir, p[4], p[5], p[6], p[7], port, len);
    int n = len; if (n < 0) n = 0; if (n > 512) n = 512;   // clamp the dump
    if (n) { LogHexTag("[cap.hex]", buf, (size_t)n); LogPrintableRuns("[cap.str]", buf, (size_t)n); }
}

int WSAAPI hkSendto(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
    if (CapIsMaster(to)) CapLog("SEND", to, buf, len);
    return g_origSendto(s, buf, len, flags, to, tolen);
}
int WSAAPI hkRecvfrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen) {
    int r = g_origRecvfrom(s, buf, len, flags, from, fromlen);
    if (r > 0 && CapIsMaster(from)) CapLog("RECV", from, buf, r);
    return r;
}
int WSAAPI hkGetaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** res) {
    int r = g_origGetaddrinfo(node, service, hints, res);
    auto containsCI = [](const char* hay, const char* nee) {
        if (!hay) return false;
        auto lc = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; };
        for (const char* h = hay; *h; ++h) {
            const char* a = h; const char* b = nee;
            while (*a && *b && lc(*a) == lc(*b)) { ++a; ++b; }
            if (!*b) return true;
        }
        return false;
    };
    if (node && containsCI(node, "mx-bikes")) {
        Log("[cap] getaddrinfo node='%s' service='%s' rc=%d", node, service ? service : "(null)", r);
        if (r == 0 && res) {
            for (addrinfo* ai = *res; ai; ai = ai->ai_next) {
                if (ai->ai_family == AF_INET && ai->ai_addr && ai->ai_addrlen >= 8) {
                    const unsigned char* p = (const unsigned char*)ai->ai_addr;
                    Log("[cap]   resolved -> %u.%u.%u.%u", p[4], p[5], p[6], p[7]);
                    uint32_t ip; memcpy(&ip, p + 4, 4);
                    uint32_t expected = 0;
                    g_capMasterIp.compare_exchange_strong(expected, ip);   // latch the first
                }
            }
        }
    }
    return r;
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

// Run a FrostMod menu action by its digit key. Add a case + a kMenu[] row to expose
// a new feature - no new global F-key needed. Most actions close the menu after.
void MenuAction(int d) {
    switch (d) {
    case 1: RequestReload();    g_menuOpen.store(false); break;   // reload mods (shows the bar)
    case 2: { bool on = !g_overlayOn.load(); g_overlayOn.store(on);
              Log("[overlay] hint %s", on ? "shown" : "hidden"); g_menuOpen.store(false); } break;
    case 3: g_menuOpen.store(false); OpenModelSwap();    break;   // swap a bike's model.edf (list UI)
    case 4: { bool on = !g_radarOn.load(); g_radarOn.store(on); SaveRadarSettings();
              SetStatus(on ? "radar: on" : "radar: off", 1500);
              Log("[radar] %s", on ? "on" : "off"); g_menuOpen.store(false); } break;
    case 5: { bool on = !g_espOn.load(); g_espOn.store(on); SaveRadarSettings();
              SetStatus(on ? "rider outlines: on" : "rider outlines: off", 1500);
              Log("[esp] %s", on ? "on" : "off"); g_menuOpen.store(false); } break;
    default: break;
    }
    // Hidden actions kept for reference / easy re-enable (their functions still exist):
    //   OpenTrackManager()               - track manager      OpenSwitcher() - switch track
    //   DumpTrackList()                  - track list -> log   OpenDirectConnect() - direct connect
}

void Tick() {
    // Heartbeat: proves the render hook fires. No [tick] line in the log => the game
    // isn't calling the SwapBuffers we hooked, so F8 / reload can't run.
    static bool firstFrame = true;
    if (firstFrame) { firstFrame = false; Log("[tick] render hook alive - first frame presented"); }

    // F8 opens the FrostMod menu; while open, a digit runs an item, Esc/F8 closes.
    // New features are rows in kMenu[] / MenuAction(), not new global F-keys.
    static bool prevF8 = false, prevEsc = false, prevDigit[10] = {false};
    bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8 && !prevF8) {
        if (g_msOpen.load()) {                               // F8 also closes an open list
            CloseModelSwap(); Log("[model] model swap closed (F8).");
        } else if (g_trkOpen.load()) {
            CloseTrackManager(); Log("[trklib] track manager closed (F8).");
        } else if (g_swOpen.load()) {
            CloseSwitcher(); Log("[switch] switcher closed (F8).");
        } else if (g_dcOpen.load()) {
            CloseDirectConnect(); Log("[connect] direct connect closed (F8).");
        } else {
            bool open = !g_menuOpen.load();
            g_menuOpen.store(open);
            Log("[menu] %s", open ? "opened (press a number; Esc/F8 to close)" : "closed");
        }
    }
    prevF8 = f8;
    if (g_menuOpen.load()) {
        for (int d = 1; d <= 9; ++d) {                       // digit -> menu action
            bool k = (GetAsyncKeyState('0' + d) & 0x8000) != 0;
            if (k && !prevDigit[d] && d <= kMenuCount) MenuAction(d);
            prevDigit[d] = k;
        }
        bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        if (esc && !prevEsc) g_menuOpen.store(false);
        prevEsc = esc;
    }

    // Track manager (F8 > 2): modal keyboard list with two sub-modes.
    //   NAV (default): Up/Down move the cursor over the *filtered* view (held-key repeat so
    //     a long list scrolls); Space toggles the cursor row's staged state; A selects /
    //     unselects ALL rows in the current view; F (or '/') opens the search box; Enter
    //     applies (moves the .pkz + reloads, closing the list); Esc closes.
    //   SEARCH (after F / '/'): typed letters/digits/space build g_trkQuery and the list
    //     filters live; Backspace deletes; Enter commits (back to NAV, filter kept); Esc
    //     clears the query and returns to NAV.
    // One prevKey[] edge table serves both modes. Enter/Esc are polled in both branches so
    // a key held across a mode switch can't double-fire (it must be released + pressed
    // again). Its state is mutually exclusive with the menu above (opening it clears
    // g_menuOpen), so the two input blocks never fight.
    if (g_trkOpen.load()) {
        static bool prevKey[256] = {false};
        static ULONGLONG upNext = 0, downNext = 0;
        const ULONGLONG now = GetTickCount64();
        auto keyDown = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        // Fresh press since last frame (always updates prevKey so mode switches stay clean).
        auto edge = [&](int vk) { bool k = keyDown(vk); bool e = k && !prevKey[vk]; prevKey[vk] = k; return e; };
        // Edge, then auto-repeat while held: initial 350ms delay, 90ms cadence.
        auto repeat = [&](int vk, ULONGLONG& nextAt) {
            bool k = keyDown(vk), fire = false;
            if (k && !prevKey[vk])       { nextAt = now + 350; fire = true; }
            else if (k && now >= nextAt) { nextAt = now + 90;  fire = true; }
            prevKey[vk] = k; return fire;
        };
        const int vn = (int)g_trkView.size();

        if (g_trkSearch) {                                    // ---- SEARCH: build the query
            bool changed = false;
            for (int vk = 'A'; vk <= 'Z'; ++vk) if (edge(vk)) { g_trkQuery.push_back((char)(vk + 32)); changed = true; }
            for (int vk = '0'; vk <= '9'; ++vk) if (edge(vk)) { g_trkQuery.push_back((char)vk);         changed = true; }
            if (edge(VK_SPACE))                        { g_trkQuery.push_back(' '); changed = true; }
            if (edge(VK_BACK) && !g_trkQuery.empty())  { g_trkQuery.pop_back();      changed = true; }
            if (edge(VK_RETURN)) {                            // commit; keep the filter
                g_trkSearch = false;
                Log("[trklib] search committed: \"%s\" (%d matches).", g_trkQuery.c_str(), vn);
            } else if (edge(VK_ESCAPE)) {                     // cancel; drop the filter
                g_trkSearch = false;
                if (!g_trkQuery.empty()) { g_trkQuery.clear(); changed = true; }
            }
            if (changed) RebuildTrackView();
        } else {                                             // ---- NAV: move / toggle / apply
            if (vn) {
                if (repeat(VK_UP,   upNext))   g_trkCursor = (g_trkCursor - 1 + vn) % vn;   // wraps
                if (repeat(VK_DOWN, downNext)) g_trkCursor = (g_trkCursor + 1)      % vn;
            }
            if (edge(VK_SPACE) && vn) {                        // toggle the cursor row
                TrackEntry& e = g_trk[g_trkView[g_trkCursor]]; e.staged = !e.staged;
            }
            if (edge('A') && vn) {                             // select / unselect all in view
                bool allStaged = true;
                for (int vi = 0; vi < vn; ++vi) if (!g_trk[g_trkView[vi]].staged) { allStaged = false; break; }
                const bool target = !allStaged;               // all staged -> clear; else set all
                for (int vi = 0; vi < vn; ++vi) g_trk[g_trkView[vi]].staged = target;
                Log("[trklib] %s all %d shown track(s).", target ? "selected" : "unselected", vn);
            }
            const bool find = edge('F'), slash = edge(VK_OEM_2);  // both, so prevKey stays fresh
            if (find || slash) { g_trkSearch = true; Log("[trklib] search: type to filter, Enter=done, Esc=clear."); }
            if (edge(VK_RETURN))      ApplyTrackChanges();    // moves + reload; closes the manager
            else if (edge(VK_ESCAPE)) { CloseTrackManager(); Log("[trklib] track manager closed (Esc)."); }
        }
    }

    // Track switcher (F8 > 3): Up/Down move (held-key repeat), Enter loads the
    // highlighted track into the localhost session, Esc cancels.
    if (g_swOpen.load()) {
        static bool sUp = false, sDown = false, sEnter = false, sEsc = false;
        static ULONGLONG sUpNext = 0, sDownNext = 0;
        const int n = (int)g_swNames.size();
        const ULONGLONG now = GetTickCount64();
        bool up    = (GetAsyncKeyState(VK_UP)     & 0x8000) != 0;
        bool down  = (GetAsyncKeyState(VK_DOWN)   & 0x8000) != 0;
        bool enter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
        bool esc   = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        auto step = [&](bool held, bool prev, ULONGLONG& nextAt) -> bool {
            if (held && !prev)         { nextAt = now + 350; return true; }
            if (held && now >= nextAt) { nextAt = now + 90;  return true; }
            return false;
        };
        if (n) {
            if (step(up,   sUp,   sUpNext))   g_swCursor = (g_swCursor - 1 + n) % n;
            if (step(down, sDown, sDownNext)) g_swCursor = (g_swCursor + 1)     % n;
        }
        if (enter && !sEnter && n) { int idx = g_swCursor; CloseSwitcher(); LoadTrackByIndex(idx); }
        else if (esc && !sEsc)     { CloseSwitcher(); Log("[switch] switcher closed (Esc)."); }
        sUp = up; sDown = down; sEnter = enter; sEsc = esc;
    }

    // Direct connect (F8 > 6): one-line IP:port text box. Digits (top row + numpad),
    // '.', and ':' build g_dcInput; Backspace deletes; Enter parses + connects; Esc
    // cancels. On the first frame we latch the currently-held keys so the '6' that
    // opened the box (handled by the menu digit block) isn't captured as input.
    if (g_dcOpen.load()) {
        static bool dcPrev[256] = {false};
        auto keyDown = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        if (!g_dcPrimed) {
            for (int vk = 0; vk < 256; ++vk) dcPrev[vk] = keyDown(vk);
            g_dcPrimed = true;
        }
        auto edge = [&](int vk) { bool k = keyDown(vk); bool e = k && !dcPrev[vk]; dcPrev[vk] = k; return e; };
        bool changed = false;
        auto emit = [&](char c) { if (g_dcInput.size() < kDcMax) { g_dcInput.push_back(c); changed = true; } };

        for (int vk = '0'; vk <= '9'; ++vk)                 if (edge(vk)) emit((char)vk);
        for (int vk = VK_NUMPAD0; vk <= VK_NUMPAD9; ++vk)   if (edge(vk)) emit((char)('0' + (vk - VK_NUMPAD0)));
        const bool dot1 = edge(VK_OEM_PERIOD), dot2 = edge(VK_DECIMAL);   // both, so dcPrev stays fresh
        if (dot1 || dot2) emit('.');
        if (edge(VK_OEM_1)) emit(':');                       // ';:' key -> port separator
        if (edge(VK_BACK) && !g_dcInput.empty()) { g_dcInput.pop_back(); changed = true; }

        const bool enter = edge(VK_RETURN), esc = edge(VK_ESCAPE);   // both, so dcPrev stays fresh
        if (changed) g_dcError.clear();                      // editing clears the last error
        if (enter)     AttemptDirectConnect();               // parse + connect (keeps box open on error)
        else if (esc) { CloseDirectConnect(); Log("[connect] direct connect closed (Esc)."); }
    }

    // Bike model swap (F8 > 3): two-level list. Up/Down move (held-key repeat); Enter picks
    // a bike (level 0 -> level 1) or swaps the highlighted variant (level 1); Esc backs out
    // one level, or closes from the bike list. State touched only here + the draw paths.
    if (g_msOpen.load()) {
        static bool mUp = false, mDown = false, mEnter = false, mEsc = false;
        static ULONGLONG mUpNext = 0, mDownNext = 0;
        const ULONGLONG now = GetTickCount64();
        const bool lvl1 = (g_msLevel == 1);
        const int  n = (int)(lvl1 ? g_msVars.size() : g_msBikes.size());
        int& cursor = lvl1 ? g_msVarCursor : g_msBikeCursor;
        bool up    = (GetAsyncKeyState(VK_UP)     & 0x8000) != 0;
        bool down  = (GetAsyncKeyState(VK_DOWN)   & 0x8000) != 0;
        bool enter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
        bool esc   = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        auto step = [&](bool held, bool prev, ULONGLONG& nextAt) -> bool {
            if (held && !prev)         { nextAt = now + 350; return true; }
            if (held && now >= nextAt) { nextAt = now + 90;  return true; }
            return false;
        };
        if (n) {
            if (step(up,   mUp,   mUpNext))   cursor = (cursor - 1 + n) % n;
            if (step(down, mDown, mDownNext)) cursor = (cursor + 1)     % n;
        }
        if (enter && !mEnter) {
            if (!lvl1) {                                     // level 0: enter the chosen bike
                if (n) {
                    g_msBike = g_msBikes[g_msBikeCursor];
                    MsScanVariants(g_msBike);
                    g_msLevel = 1;
                    Log("[model] bike '%s' - %zu variant(s) (active='%s').",
                        g_msBike.c_str(), g_msVars.size(), g_msVars.empty() ? "" : g_msVars[0].c_str());
                }
            } else if (n) {                                  // level 1: swap the highlighted variant
                MsApply(g_msBike, g_msVars[g_msVarCursor]);   // owns its own reload + status
            }
        } else if (esc && !mEsc) {
            if (lvl1) g_msLevel = 0;                          // back to the bike list
            else { CloseModelSwap(); Log("[model] model swap closed (Esc)."); }
        }
        mUp = up; mDown = down; mEnter = enter; mEsc = esc;
    }

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

    // Radar range adjust (PageUp/PageDown) while the radar is shown; validate the
    // captured camera matrix each frame so the ESP can fall back to arrows if it drifts.
    if (g_radarOn.load() || g_espOn.load()) {
        static bool prevPgUp = false, prevPgDn = false;
        bool pu = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;   // PageUp
        bool pd = (GetAsyncKeyState(VK_NEXT)  & 0x8000) != 0;   // PageDown
        if (pu && !prevPgUp) { g_radarRange = ClampF(g_radarRange + 20.0f, 20.0f, 400.0f); SaveRadarSettings(); }
        if (pd && !prevPgDn) { g_radarRange = ClampF(g_radarRange - 20.0f, 20.0f, 400.0f); SaveRadarSettings(); }
        prevPgUp = pu; prevPgDn = pd;
        RadValidateVP();
    }

    DrainGameThreadTasks();
    AdvanceReload();   // run at most one reload step, so a frame presents between steps
}

// One-shot: log the GL pipeline identity + arm a short matrix-flow dump. Tells us
// (from the tester's log) whether fixed-function VP capture is viable or we must
// pivot to a shader/uniform capture for the ESP. Read-only.
static void LogGlInfoOnce() {
    static bool done = false; if (done) return; done = true;
    const char* ver = (const char*)glGetString(GL_VERSION);
    const char* sl  = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
    const char* rnd = (const char*)glGetString(GL_RENDERER);
    GLint vp[4] = {0,0,0,0}; glGetIntegerv(GL_VIEWPORT, vp);
    Log("[esp/diag] GL_VERSION='%s' GLSL='%s' RENDERER='%s' viewport=%dx%d",
        ver ? ver : "?", sl ? sl : "?", rnd ? rnd : "?", vp[2], vp[3]);
    g_glDiag.store(240, std::memory_order_relaxed);   // dump the next ~240 matrix loads
}

BOOL WINAPI hkSwapBuffers(HDC hdc)      { Tick(); return g_origSwapBuffers(hdc); }
BOOL WINAPI hkWglSwapBuffers(HDC hdc) {
    LogGlInfoOnce();
    Tick();
    // Draw the GL overlay only when the sanctioned Draw() path is NOT feeding the
    // engine this frame. On track the game calls Draw() every frame (g_drawCalls
    // advances) -> skip GL, no double image. In menus / injected mode Draw() never
    // fires (count is stable) -> GL draws the overlay as before.
    static uint64_t lastDraw = 0;
    uint64_t now = g_drawCalls.load(std::memory_order_relaxed);
    if (now == lastDraw) DrawOverlay(hdc);
    lastDraw = now;
    return g_origWglSwapBuffers(hdc);
}

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
        // camera view-projection capture for the rider outlines (ESP). Snoop the
        // fixed-function matrix calls; if the engine is core-profile these never
        // fire and g_vp stays uncaptured (ESP falls back to directional arrows).
        if (auto p = GetProcAddress(gl, "glMatrixMode"))
            InstallHook((void*)p, &hkGlMatrixMode,  (void**)&g_origGlMatrixMode,  "opengl32!glMatrixMode");
        if (auto p = GetProcAddress(gl, "glLoadMatrixf"))
            InstallHook((void*)p, &hkGlLoadMatrixf, (void**)&g_origGlLoadMatrixf, "opengl32!glLoadMatrixf");
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

    // OPT-IN (frostmod.exe --capture-master): sniff the master protocol to RE the
    // login / GETLIST / REGISTER / HOSTED wire format before we build the mimic
    // master. Hooks the ws2_32 exports and logs only master (UDP 54200 / resolved
    // master IP) traffic as [cap]/[cap.hex]/[cap.str]. Read-only. Off by default.
    {
        char flag[MAX_PATH] = {0};
        if (g_logPath[0]) {
            strcpy_s(flag, g_logPath);
            if (char* s = strrchr(flag, '\\')) { *(s + 1) = 0; strcat_s(flag, "frostmod_capture.flag"); }
        }
        if (flag[0] && GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES) {
            if (HMODULE ws = LoadLibraryA("ws2_32.dll")) {
                if (auto p = GetProcAddress(ws, "sendto"))
                    InstallHook((void*)p, &hkSendto, (void**)&g_origSendto, "ws2_32!sendto");
                if (auto p = GetProcAddress(ws, "recvfrom"))
                    InstallHook((void*)p, &hkRecvfrom, (void**)&g_origRecvfrom, "ws2_32!recvfrom");
                if (auto p = GetProcAddress(ws, "getaddrinfo"))
                    InstallHook((void*)p, &hkGetaddrinfo, (void**)&g_origGetaddrinfo, "ws2_32!getaddrinfo");
                Log("[cap] --capture-master ARMED (UDP %u). Open the online browser (client) and/or run "
                    "`mxbikes.exe --dedicated`, then send frostmod.log back. If you see a [cap] getaddrinfo "
                    "line but no SEND/RECV, the game uses WSASendTo/WSARecvFrom - tell me and I'll add those.",
                    (unsigned)kMasterPort);
            } else {
                Log("[cap] --capture-master: could not load ws2_32.dll; capture not installed.");
            }
        }
    }

    // OPT-IN (create an empty frostmod_edfcap.flag next to frostmod.log): log every
    // bike-model (.edf) file the game opens, WITH its call stack, to RE the garage
    // bike-preview reload path - which loader re-reads model.edf from disk, and
    // whether re-selecting the SAME bike re-opens it (the mesh-cache test). See the
    // hkCreateFileW/A capture hooks above. Read-only; off by default.
    {
        char flag[MAX_PATH] = {0};
        if (g_logPath[0]) {
            strcpy_s(flag, g_logPath);
            if (char* s = strrchr(flag, '\\')) { *(s + 1) = 0; strcat_s(flag, "frostmod_edfcap.flag"); }
        }
        if (flag[0] && GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES) {
            if (HMODULE k32 = GetModuleHandleA("kernel32.dll")) {
                if (auto p = GetProcAddress(k32, "CreateFileW"))
                    InstallHook((void*)p, &hkCreateFileW, (void**)&g_origCreateFileW, "kernel32!CreateFileW");
                if (auto p = GetProcAddress(k32, "CreateFileA"))
                    InstallHook((void*)p, &hkCreateFileA, (void**)&g_origCreateFileA, "kernel32!CreateFileA");
                Log("[edf] --edfcap ARMED. In the GARAGE: (A) swap a bike's model then RE-SELECT the SAME "
                    "bike (no class change) - if NO new [edf] line appears, the mesh is cached by identity. "
                    "(B) switch bike CLASS away then back - the new [edf] line's stack names the loader (top "
                    "mxbikes.exe RVA) and the selection caller above it. Then send frostmod.log back.");
            } else {
                Log("[edf] --edfcap: could not get kernel32; capture not installed.");
            }
        }
    }

    // Track library (F10): learn the mods folder from frostmod_mods.txt (the launcher
    // writes it next to us) and derive the inactive-tracks store as a sibling of mods,
    // OUTSIDE the scanned tree so deactivated tracks are never loaded.
    {
        char info[MAX_PATH] = {0};
        if (g_logPath[0]) {
            strcpy_s(info, g_logPath);
            if (char* s = strrchr(info, '\\')) { *(s + 1) = 0; strcat_s(info, "frostmod_mods.txt"); }
        }
        if (info[0]) {
            if (FILE* f; fopen_s(&f, info, "r") == 0 && f) {
                if (fgets(g_modsPath, sizeof(g_modsPath), f)) {
                    size_t L = strlen(g_modsPath);
                    while (L && (g_modsPath[L-1] == '\n' || g_modsPath[L-1] == '\r' ||
                                 g_modsPath[L-1] == ' '  || g_modsPath[L-1] == '\t')) g_modsPath[--L] = 0;
                }
                fclose(f);
            }
        }
        if (g_modsPath[0]) {
            strcpy_s(g_inactivePath, g_modsPath);
            if (char* s = strrchr(g_inactivePath, '\\')) *s = 0;   // strip trailing "\mods"
            strcat_s(g_inactivePath, "\\FrostMod Inactive Tracks");
            Log("[trklib] mods=%s", g_modsPath);
            Log("[trklib] inactive store=%s (F8 menu > 2 opens the track manager)", g_inactivePath);
        } else {
            Log("[trklib] mods path unknown yet (run frostmod.exe so it writes frostmod_mods.txt).");
        }
    }

    // Server-browser spam filter: load rules from <dll folder>\frostmod_serverfilter.yaml
    // (created with docs on first run). The actual hook that feeds entries in is
    // wired once the server-list function is RE'd - see the SERVER FILTER block above.
    {
        std::string cfg = g_logPath;
        if (size_t s = cfg.find_last_of("\\/"); s != std::string::npos)
            cfg = cfg.substr(0, s + 1) + "frostmod_serverfilter.yaml";
        else
            cfg = "frostmod_serverfilter.yaml";
        frostmod::serverfilter::Init(cfg, &SfLog);
        LoadRadarSettings();   // restore radar / rider-outline toggles + range

        // OPT-IN (frostmod.exe --filter-servers): install the loop-top filter that logs
        // every server row and skips (hides) the ones matching the rules BEFORE the row
        // is created. Mid-function hook, so gated behind a flag. Scope is cheat/ad ghosts
        // by default (serverfilter config v4, YAML); see SB_SuppressRow.
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

    // Track switcher live-load: opt-in via frostmod_trackswitch.flag (--switch-live),
    // because calling fcn.1400BB510 outside the testing menu crashes. Default off ->
    // the switcher is a safe field preview.
    {
        char sflag[MAX_PATH] = {0};
        if (g_logPath[0]) {
            strcpy_s(sflag, g_logPath);
            if (char* s = strrchr(sflag, '\\')) { *(s + 1) = 0; strcat_s(sflag, "frostmod_trackswitch.flag"); }
        }
        g_swLiveLoad = sflag[0] && GetFileAttributesA(sflag) != INVALID_FILE_ATTRIBUTES;
        Log("[switch] live load %s.", g_swLiveLoad ? "ARMED (--switch-live) - try from the testing menu"
                                                   : "off (safe preview) - arm with --switch-live");
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
// We also implement the optional Draw() callback as the sanctioned overlay path
// (on track/spectate/replay); other data/telemetry callbacks are omitted, and the
// game only calls the exports that exist. See docs/PLUGIN.md.
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

// Optional. Called each frame while on track (0), spectating (1), or in a replay
// (2) - NOT in menus. We build our overlay as quads+strings and hand the engine
// the (persistent) arrays to render. Bumping g_drawCalls tells the swap hook the
// sanctioned path is live so it skips the GL fallback (no double image). The
// out-arrays stay valid after return because they are static.
__declspec(dllexport) void Draw(int /*_iState*/, int* _piNumQuads, void** _ppQuad,
                                int* _piNumString, void** _ppString) {
    g_drawCalls.fetch_add(1, std::memory_order_relaxed);
    BuildOverlayDrawLists();
    if (_piNumQuads)  *_piNumQuads  = g_nDrawQuads;
    if (_ppQuad)      *_ppQuad      = g_drawQuads;
    if (_piNumString) *_piNumString = g_nDrawStrs;
    if (_ppString)    *_ppString    = g_drawStrs;
}

// ---- radar / rider-outline data feed (all read-only; see the RADAR section) --
// RunTelemetry: our own bike pose. Startup() returned rate 3, so the game calls
// this and we keep the latest world position (used to identify "me" among the
// track-position entries).
__declspec(dllexport) void RunTelemetry(void* _pData, int _iDataSize, float, float) {
    RadStoreTelemetry(_pData, _iDataSize);
}
// Every vehicle's live world position + yaw, once per update. The radar + outlines.
__declspec(dllexport) void RaceTrackPosition(int _iNumVehicles, void* _pArray, int _iElemSize) {
    RadStoreTrackPositions(_iNumVehicles, _pArray, _iElemSize);
}
__declspec(dllexport) void RaceAddEntry(void* _pData, int _iDataSize) {
    RadAddEntry(_pData, _iDataSize);
}
__declspec(dllexport) void RaceRemoveEntry(void* _pData, int _iDataSize) {
    RadRemoveEntry(_pData, _iDataSize);
}
// Classification carries laps-done per race number -> the lap-status coloring.
__declspec(dllexport) void RaceClassification(void* _pData, int _iDataSize, void* _pArray, int _iElemSize) {
    int n = (_pData && _iDataSize >= (int)sizeof(SPluginsRaceClassification_t))
            ? ((SPluginsRaceClassification_t*)_pData)->m_iNumEntries : 0;
    RadStoreClassification(_pArray, n, _iElemSize);
}
__declspec(dllexport) void RaceSession(void*, int)  { RadResetRace(); }
__declspec(dllexport) void RaceDeinit()             { RadResetRace(); }

} // extern "C"
