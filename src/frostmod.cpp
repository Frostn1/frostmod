// ============================================================================
//  FrostMod - on-demand mods-folder reloader for MX Bikes
//
//  What it does
//  ------------
//  MX Bikes reads the mods/content folders once at startup and mounts every
//  .pkz into an in-memory virtual filesystem. New files dropped in while the
//  game runs are ignored until a restart. FrostMod adds a small floating
//  window with a "Reload Mods" button that re-triggers the game's own content
//  scan so newly added tracks/skins register live.
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

#include "MinHook.h"
#include "offsets.h"
#include "serverfilter.h"

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

// fcn.140158be0 - folder scanner  (4 register args, no stack args)
using ScanFolder_t = int64_t(__fastcall*)(void*, void*, void*, void*);
ScanFolder_t g_origScan = nullptr;

// fcn.140159340 - registry reset/rebuild  (2 register args)
using RegistryReset_t = int64_t(__fastcall*)(void*, void*);
RegistryReset_t g_origReset = nullptr;

struct CapturedCall {
    std::atomic<bool> valid{false};
    void* a0{}; void* a1{}; void* a2{}; void* a3{};
};
CapturedCall g_scanArgs;   // args of the last .pkz (mods) scan we saw
CapturedCall g_resetArgs;  // last args seen for the registry reset

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

    // Keep the args of the ".pkz" scan as the primary replay target - that's the
    // one that mounts mods/tracks.
    if (_stricmp(ext.c_str(), "pkz") == 0) {
        g_scanArgs.a0 = a0; g_scanArgs.a1 = a1; g_scanArgs.a2 = a2; g_scanArgs.a3 = a3;
        g_scanArgs.valid.store(true);
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

// ---------------------------------------------------------------------------
// SERVER FILTER - hide spam/"ghost" servers from the browser.
//
// The RE (offsets.h) shows the browser builds SB_Entry working copies (stride
// 0x1D8) and a populate loop (RVA_SB_POPULATE_LOOP) emits one row each, with a
// row-skip target (RVA_SB_ROW_SKIP_TGT) that keeps the game's counts consistent.
//
// Given a pointer to the current SB_Entry, this decides show/hide. It's the exact
// callback the loop-splice stub will call: read the fields (SafeCopyStr for the
// inline name), build a ServerInfo, ask serverfilter. Returns true => SKIP the row.
// The powerful default: ping == 0xFFFFFFFF ("---") means unjoinable = ghost/ad.
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

extern "C" bool SB_ShouldHideEntry(void* entry) {   // extern "C": easy to call from an asm stub
    if (!entry) return false;

    SBNums nums = SafeReadSBNums(entry);
    if (!nums.ok) return false;                     // bad read -> never hide/crash

    frostmod::serverfilter::ServerInfo si;
    char nameBuf[256];
    if (SafeCopyStr((char*)entry + mxb::SBE_NAME, nameBuf, sizeof(nameBuf))) si.name = nameBuf;
    si.players    = nums.players;
    si.maxPlayers = nums.maxPlayers;
    si.unjoinable = nums.unjoinable;

    std::string why = frostmod::serverfilter::ShouldHide(si);
    if (why.empty()) return false;
    Log("[filter] hid '%s' (%s)", si.name.c_str(), why.c_str());
    return true;
}

// The populate emit is INLINE. We splice the game's hide-empty branch
//   0x0ABAB6:  cmp [rsp+rdi+0xCC], r12d    (entry = rsp+rdi, maxplayers @ +0xCC)
//   0x0ABABE:  jz  <row-skip>              (skip the row when equal)
// with a MinHook hook to a hand-built stub that: computes rsp+rdi, calls
// SB_ShouldHideEntry, and if it says HIDE, writes r12d into [entry+0xCC] so the
// game's OWN cmp/jz then skips the row through its normal, consistent path (no
// control-flow surgery from us = no desync). Always continues via the trampoline.
void* g_sbTramp = nullptr;

bool InstallServerFilterHook() {
    uintptr_t target = g_base + mxb::RVA_SB_HIDE_EMPTY_BR;   // 0x0ABAB6

    // verify the splice site really is the expected cmp before touching it
    unsigned char here[sizeof(mxb::SB_HIDE_EMPTY_BYTES)];
    if (SafeReadBytes((const char*)target, (char*)here, sizeof(here)) < sizeof(here) ||
        memcmp(here, mxb::SB_HIDE_EMPTY_BYTES, sizeof(here)) != 0) {
        Log("[filter] site 0x%zx isn't the expected cmp - build changed? not hooking.",
            (size_t)mxb::RVA_SB_HIDE_EMPTY_BR);
        return false;
    }

    auto stub = (unsigned char*)VirtualAlloc(nullptr, 0x100, MEM_COMMIT | MEM_RESERVE,
                                             PAGE_EXECUTE_READWRITE);
    if (!stub) { Log("[filter] stub alloc failed"); return false; }

    // register the hook first so MinHook builds the trampoline (detour = our stub);
    // we then fill the stub bytes (which reference the trampoline), then enable.
    if (MH_CreateHook((void*)target, stub, &g_sbTramp) != MH_OK) {
        Log("[filter] MH_CreateHook failed @ 0x%zx", (size_t)mxb::RVA_SB_HIDE_EMPTY_BR);
        VirtualFree(stub, 0, MEM_RELEASE); return false;
    }

    size_t o = 0;
    auto b  = [&](unsigned char x){ stub[o++] = x; };
    auto b8 = [&](uint64_t v){ for (int i = 0; i < 8; ++i) b((unsigned char)(v >> (i * 8))); };
    b(0x50);                                             // push rax
    b(0x48);b(0x8D);b(0x44);b(0x3C);b(0x08);             // lea rax,[rsp+rdi+8]  (entry ptr)
    b(0x51);b(0x52);b(0x41);b(0x50);b(0x41);b(0x51);     // push rcx,rdx,r8,r9
    b(0x41);b(0x52);b(0x41);b(0x53);                     // push r10,r11
    b(0x9C);                                             // pushfq
    b(0x48);b(0x89);b(0xC1);                             // mov rcx,rax   (arg = entry)
    b(0x49);b(0x89);b(0xE2);                             // mov r10,rsp   (save rsp)
    b(0x48);b(0x83);b(0xE4);b(0xF0);                     // and rsp,-16   (align)
    b(0x48);b(0x83);b(0xEC);b(0x20);                     // sub rsp,0x20  (shadow)
    b(0x48);b(0xB8);b8((uint64_t)&SB_ShouldHideEntry);   // mov rax, &SB_ShouldHideEntry
    b(0xFF);b(0xD0);                                     // call rax
    b(0x4C);b(0x89);b(0xD4);                             // mov rsp,r10   (restore rsp -> post-pushfq)
    b(0x84);b(0xC0);                                     // test al,al
    b(0x74);b(0x0C);                                     // jz +0x0C  (skip the hide-write)
    b(0x48);b(0x8D);b(0x44);b(0x3C);b(0x40);             // lea rax,[rsp+rdi+0x40]  (entry; rsp now -0x40)
    b(0x44);b(0x89);b(0xA0);b(0xCC);b(0x00);b(0x00);b(0x00); // mov [rax+0xCC], r12d  (force maxplayers==r12d)
    // restore regs+flags, then continue into the original cmp (trampoline)
    b(0x9D);                                             // popfq
    b(0x41);b(0x5B);b(0x41);b(0x5A);b(0x41);b(0x59);b(0x41);b(0x58);b(0x5A);b(0x59);b(0x58); // pop r11..rax
    b(0xFF);b(0x25);b(0x00);b(0x00);b(0x00);b(0x00);     // jmp [rip+0] -> tramp slot
    b8((uint64_t)g_sbTramp);                             // tramp slot

    if (MH_EnableHook((void*)target) != MH_OK) {
        Log("[filter] MH_EnableHook failed @ 0x%zx", (size_t)mxb::RVA_SB_HIDE_EMPTY_BR);
        return false;
    }
    Log("[filter] server-filter hook LIVE @ 0x%zx (entry=rsp+rdi; hides via the game's "
        "own row-skip). Unjoinable/spam servers will drop from the browser.",
        (size_t)mxb::RVA_SB_HIDE_EMPTY_BR);
    return true;
}

// ---------------------------------------------------------------------------
// the reload action - runs ON THE GAME THREAD (called from the swap hook)
// ---------------------------------------------------------------------------
// Two families of reload, several variants - cycle with F7 (in-game) or S (in the
// frostmod.exe console) and see which one actually makes a new track appear:
//   A  replay: re-run the exact scan the game did (needs it captured at startup).
//   B  direct: call the scanner ourselves with our own args (works without capture,
//              experimental - guarded so a bad guess can't crash the game).
enum class ReloadStrategy {
    ReplayPkzScan,        // A : replay the captured .pkz scan only
    ReplayResetThenPkz,   // A+: replay captured reset, then the .pkz scan  (default)
    ReplayAllContent,     // A++: replay captured reset, then EVERY captured scan
    DirectCallScanner,    // B : construct + call the scanner (experimental)
    COUNT
};
std::atomic<ReloadStrategy> g_strategy{ReloadStrategy::ReplayResetThenPkz};

const char* StrategyName(ReloadStrategy s) {
    switch (s) {
    case ReloadStrategy::ReplayPkzScan:      return "A  (replay .pkz scan)";
    case ReloadStrategy::ReplayResetThenPkz: return "A+ (reset + replay .pkz scan)";
    case ReloadStrategy::ReplayAllContent:   return "A++ (reset + replay ALL scans)";
    case ReloadStrategy::DirectCallScanner:  return "B  (direct-call scanner, experimental)";
    default:                                 return "?";
    }
}

void CycleStrategy() {
    int cur  = (int)g_strategy.load();
    auto next = (ReloadStrategy)((cur + 1) % (int)ReloadStrategy::COUNT);
    g_strategy.store(next);
    Log("[reload] strategy -> %s", StrategyName(next));
}

// SEH-guarded calls: never let a wrong-argument replay/construct crash the game.
// (No C++ objects here, so __try/__except is allowed.)
static int64_t SafeCallScan(void* a0, void* a1, void* a2, void* a3) {
    __try { return g_origScan(a0, a1, a2, a3); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -0x1EAD; }
}
static int64_t SafeCallReset(void* a0, void* a1) {
    __try { return g_origReset(a0, a1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -0x1EAD; }
}

void ReplayReset() {
    if (g_resetArgs.valid.load() && g_origReset) {
        Log("[reload] replay registryReset(rcx=%p rdx=%p)", g_resetArgs.a0, g_resetArgs.a1);
        int64_t r = SafeCallReset(g_resetArgs.a0, g_resetArgs.a1);
        Log("[reload]   registryReset returned %lld%s", (long long)r,
            r == -0x1EAD ? "  (FAULTED - caught)" : "");
    } else {
        Log("[reload] note: registry-reset args not captured; skipping reset");
    }
}

// Strategy B: build the call ourselves. Reuse the game's real dir/ext string
// pointers if we captured them (best), else construct <savePath>\mods + "pkz".
// Pass FRESH zeroed status/out buffers instead of the game's old stack pointers.
void DoDirectCall() {
    static char          dirBuf[MAX_PATH];
    static char          extBuf[] = "pkz";
    static unsigned char statusBuf[0x40];
    static unsigned char outBuf[0x200];       // scanner out-buf is ~0x108

    void* dirPtr; void* extPtr;
    if (g_scanArgs.valid.load()) {
        dirPtr = g_scanArgs.a1; extPtr = g_scanArgs.a2;   // exact strings the game used
    } else {
        dirBuf[0] = 0;
        if (g_savePath[0]) { strcpy_s(dirBuf, g_savePath); strcat_s(dirBuf, "\\mods"); }
        else               { strcpy_s(dirBuf, "mods"); }
        dirPtr = dirBuf; extPtr = extBuf;
    }
    memset(statusBuf, 0, sizeof(statusBuf));
    memset(outBuf,    0, sizeof(outBuf));

    if (g_strategy.load() == ReloadStrategy::DirectCallScanner) ReplayReset();  // reset first helps
    Log("[reload] DIRECT scanner dir='%s' ext='%s' (fresh status/out buffers)",
        SafeStr(dirPtr).c_str(), SafeStr(extPtr).c_str());
    int64_t r = SafeCallScan(statusBuf, dirPtr, extPtr, outBuf);
    Log("[reload] direct scanner returned %lld%s", (long long)r,
        r == -0x1EAD ? "  (FAULTED - caught; the constructed args are wrong)" : ". Done.");
}

void DoReloadOnGameThread() {
    ReloadStrategy s = g_strategy.load();
    Log("[reload] running on game thread - strategy %s", StrategyName(s));

    if (!g_origScan) {
        Log("[reload] ABORT: content hooks were not installed (offsets.h didn't match "
            "this mxbikes build - see the [sig] lines above). Reload is unavailable.");
        return;
    }

    if (s == ReloadStrategy::DirectCallScanner) { DoDirectCall(); return; }

    // replay strategies need the .pkz scan captured at startup
    if (!g_scanArgs.valid.load()) {
        Log("[reload] ABORT: no .pkz scan captured. Load FrostMod BEFORE the startup scan "
            "(plugin mode, or inject before launch). Or press F7/S to try strategy B "
            "(direct-call), which doesn't need a capture.");
        return;
    }

    if (s == ReloadStrategy::ReplayResetThenPkz || s == ReloadStrategy::ReplayAllContent)
        ReplayReset();

    if (s == ReloadStrategy::ReplayAllContent) {
        std::vector<ScanCall> scans;
        { std::lock_guard<std::mutex> lk(g_scansMutex); scans = g_scans; }
        Log("[reload] replaying ALL %zu captured content scans...", scans.size());
        for (auto& c : scans) {
            int64_t r = SafeCallScan(c.a0, c.a1, c.a2, c.a3);
            Log("[reload]   dir='%s' ext='%s' -> %lld%s", c.dir.c_str(), c.ext.c_str(),
                (long long)r, r == -0x1EAD ? " (FAULTED)" : "");
        }
        Log("[reload] done replaying all scans.");
    } else {   // ReplayPkzScan or ReplayResetThenPkz
        Log("[reload] replay .pkz scan dir='%s' ext='%s'",
            SafeStr(g_scanArgs.a1).c_str(), SafeStr(g_scanArgs.a2).c_str());
        int64_t r = SafeCallScan(g_scanArgs.a0, g_scanArgs.a1, g_scanArgs.a2, g_scanArgs.a3);
        Log("[reload] scanner returned %lld. Done.%s", (long long)r,
            r == -0x1EAD ? "  (FAULTED - caught)"
                         : "  If the track still doesn't show, press F7/S to try another strategy.");
    }
}

void RequestReload() {
    Log("[ui] reload requested");
    // Also re-read the server-filter blocklist, so editing it takes effect without
    // restarting the game (the next server-list refresh uses the new rules).
    frostmod::serverfilter::Reload();
    EnqueueGameThreadTask(DoReloadOnGameThread);
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
HANDLE g_cycleEvent  = nullptr;   // S in the console -> cycle reload strategy
HANDLE g_dumpEvent   = nullptr;   // D in the console -> dump the server-list blob now

void DumpServerListBlob(bool force);   // fwd (defined near hkMpMsg)
// g_origMpMsg (defined above) is non-null once --dump-serverlist hooked the handler

void Tick() {
    // Heartbeat: prove the render hook is actually firing. If you never see this
    // line in frostmod.log, the game isn't calling the SwapBuffers we hooked, so
    // F8 / reload can't run - that's the thing to fix, not the reload itself.
    static bool firstFrame = true;
    if (firstFrame) { firstFrame = false; Log("[tick] render hook alive - first frame presented"); }

    // In-game hotkeys (work in fullscreen): F8 = reload, F7 = cycle strategy,
    // F9 = dump the server list right now (handy while the browser is on screen).
    static bool prevF8 = false, prevF7 = false, prevF9 = false;
    bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8 && !prevF8) RequestReload();
    prevF8 = f8;
    bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (f7 && !prevF7) CycleStrategy();
    prevF7 = f7;
    bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9 && !prevF9) { Log("[srvlist] manual dump (F9)"); DumpServerListBlob(true); }
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

    // Same, driven from the frostmod.exe console (R / S).
    if (g_reloadEvent && WaitForSingleObject(g_reloadEvent, 0) == WAIT_OBJECT_0) {
        Log("[event] reload signal received from frostmod.exe");
        RequestReload();
    }
    if (g_cycleEvent && WaitForSingleObject(g_cycleEvent, 0) == WAIT_OBJECT_0)
        CycleStrategy();
    if (g_dumpEvent && WaitForSingleObject(g_dumpEvent, 0) == WAIT_OBJECT_0) {
        Log("[srvlist] manual dump (D)"); DumpServerListBlob(true);
    }

    DrainGameThreadTasks();
}

BOOL WINAPI hkSwapBuffers(HDC hdc)      { Tick(); return g_origSwapBuffers(hdc); }
BOOL WINAPI hkWglSwapBuffers(HDC hdc)   { Tick(); return g_origWglSwapBuffers(hdc); }

// ---------------------------------------------------------------------------
// the floating UI window (its own thread + message loop)
// ---------------------------------------------------------------------------
constexpr int ID_BTN_RELOAD = 1001;
HWND g_hwnd = nullptr;

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == ID_BTN_RELOAD) { RequestReload(); return 0; }
        break;
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, RGB(24, 26, 32));
        SetTextColor((HDC)wp, RGB(120, 200, 255));
        return (LRESULT)GetStockObject(DC_BRUSH);
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

DWORD WINAPI UiThread(LPVOID) {
    WNDCLASSA wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "FrostModWindow";
    wc.hbrBackground = CreateSolidBrush(RGB(24, 26, 32));
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "FrostModWindow", "FrostMod",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        60, 60, 200, 120,
        nullptr, nullptr, wc.hInstance, nullptr);

    CreateWindowA("STATIC", "FrostMod",   // (plain ASCII: the console/GDI code page
                  WS_CHILD | WS_VISIBLE | SS_CENTER,  //  can't render a UTF-8 snowflake)
                  10, 10, 170, 22, g_hwnd, nullptr, wc.hInstance, nullptr);

    CreateWindowA("BUTTON", "Reload Mods",
                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  30, 42, 130, 34,
                  g_hwnd, (HMENU)(intptr_t)ID_BTN_RELOAD, wc.hInstance, nullptr);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    Log("[ui] floating window created (also: press F8 in-game to reload)");

    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
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
    Log("=============== FrostMod loading (dll built " __DATE__ " " __TIME__ ") ===============");

    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));  // mxbikes.exe
    Log("[init] module base = %p", (void*)g_base);

    if (MH_Initialize() != MH_OK) { Log("[init] MinHook init failed"); return 1; }

    // triggers shared with frostmod.exe: R = reload, S = cycle strategy.
    g_reloadEvent = CreateEventA(nullptr, FALSE /*auto-reset*/, FALSE, "Local\\FrostModReload");
    g_cycleEvent  = CreateEventA(nullptr, FALSE /*auto-reset*/, FALSE, "Local\\FrostModCycle");
    g_dumpEvent   = CreateEventA(nullptr, FALSE /*auto-reset*/, FALSE, "Local\\FrostModDumpNow");
    if (!g_reloadEvent) Log("[init] note: could not create reload event (%lu)", GetLastError());
    Log("[init] reload strategy = %s  (F7 in-game or S in the console cycles it)",
        StrategyName(g_strategy.load()));

    CreateThread(nullptr, 0, UiThread, nullptr, 0, nullptr);

    // CONTENT hooks first and ASAP - they're timing-critical: we must be hooked
    // before the game's one-time startup mods scan. Wait for the code to be
    // decrypted (SteamStub), then hook.
    intptr_t delta = 0;
    uintptr_t scanAddr = WaitForScanner(&delta, 30000 /*ms*/);
    if (scanAddr) {
        InstallHook((void*)scanAddr, &hkScan, (void**)&g_origScan, "scanFolder");

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

        // OPT-IN (frostmod.exe --filter-servers): splice the populate loop so the
        // rules actually hide rows. Mid-function hook, so gated behind a flag.
        char fflag[MAX_PATH] = {0};
        if (g_logPath[0]) {
            strcpy_s(fflag, g_logPath);
            if (char* s = strrchr(fflag, '\\')) { *(s + 1) = 0; strcat_s(fflag, "frostmod_filter.flag"); }
        }
        if (fflag[0] && GetFileAttributesA(fflag) != INVALID_FILE_ATTRIBUTES)
            InstallServerFilterHook();
        else
            Log("[filter] rules loaded (inert). Run frostmod.exe --filter-servers to "
                "install the populate-loop hook and actually hide servers.");
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
