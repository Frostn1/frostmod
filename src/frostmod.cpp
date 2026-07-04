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
#include <functional>
#include <mutex>
#include <atomic>
#include <set>
#include <cstring>

#include "MinHook.h"
#include "offsets.h"

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
CapturedCall g_scanArgs;   // last args seen for the scanner
CapturedCall g_resetArgs;  // last args seen for the registry reset

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

// Read a NUL-terminated string from a (possibly bogus) pointer, safely.
std::string SafeStr(void* p) {
    if (!p) return "<null>";
    char tmp[261];
    if (!SafeCopyStr(p, tmp, sizeof(tmp))) return "<unreadable>";
    return std::string(tmp);
}

// ---------------------------------------------------------------------------
// hooks that CAPTURE the game's own content-load calls
// ---------------------------------------------------------------------------
int64_t __fastcall hkScan(void* a0, void* a1, void* a2, void* a3) {
    // This scanner is GENERIC: (status, directory, file-extension, buf). The game
    // calls it for many folders - e.g. ui/str, and mods content with ext "pkz".
    std::string dir = SafeStr(a1);
    std::string ext = SafeStr(a2);

    // Log each DISTINCT (dir, ext) once, so the console shows every folder the
    // game scans and which extension - that's how we know which call is the
    // mods/content one to replay.
    {
        static std::mutex m;
        static std::set<std::string> seen;
        std::string key = dir + "|" + ext;
        bool isNew;
        { std::lock_guard<std::mutex> lk(m); isNew = seen.insert(key).second; }
        if (isNew)
            Log("[capture] scan dir='%s' ext='%s' (rcx=%p rdx=%p r8=%p r9=%p)",
                dir.c_str(), ext.c_str(), a0, a1, a2, a3);
    }

    // Keep the args of the ".pkz" scan as our replay target - that's the one that
    // mounts mods/tracks. (The old code kept whatever was scanned LAST, which was
    // the ui/str scan, so replay did nothing.)
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

// ---------------------------------------------------------------------------
// the reload action - runs ON THE GAME THREAD (called from the swap hook)
// ---------------------------------------------------------------------------
// Strategy B (default): reset the registry, then re-run the scan, replaying the
// exact arguments the game used. Strategy A: scan only. Toggle in the UI/README.
enum class ReloadStrategy { ScanOnly, ResetThenScan };
std::atomic<ReloadStrategy> g_strategy{ReloadStrategy::ResetThenScan};

void DoReloadOnGameThread() {
    Log("[reload] running on game thread (strategy=%d)",
        (int)g_strategy.load());

    if (!g_origScan) {
        Log("[reload] ABORT: content hooks were not installed (offsets.h didn't match "
            "this mxbikes build - see the [sig] lines above). Reload is unavailable.");
        return;
    }

    if (!g_scanArgs.valid.load()) {
        Log("[reload] ABORT: scanner args never captured. MX Bikes scans the mods folder "
            "only ONCE at startup, and FrostMod was loaded after that. Fix: quit the game, "
            "start frostmod.exe FIRST, then relaunch - watch for a [capture] line on load.");
        return;
    }

    if (g_strategy.load() == ReloadStrategy::ResetThenScan) {
        if (g_resetArgs.valid.load() && g_origReset) {
            Log("[reload] replay registryReset(rcx=%p rdx=%p)",
                g_resetArgs.a0, g_resetArgs.a1);
            g_origReset(g_resetArgs.a0, g_resetArgs.a1);
        } else {
            Log("[reload] note: registry-reset args not captured; scanning without reset");
        }
    }

    Log("[reload] replay scanner dir='%s' ext='%s' (rcx=%p rdx=%p r8=%p r9=%p)",
        SafeStr(g_scanArgs.a1).c_str(), SafeStr(g_scanArgs.a2).c_str(),
        g_scanArgs.a0, g_scanArgs.a1, g_scanArgs.a2, g_scanArgs.a3);
    int64_t r = g_origScan(g_scanArgs.a0, g_scanArgs.a1, g_scanArgs.a2, g_scanArgs.a3);
    Log("[reload] scanner returned %lld. Done. (if the track still doesn't show, the "
        "scanner likely skips already-loaded folders - we'll need the registry reset.)",
        (long long)r);
}

void RequestReload() {
    Log("[ui] reload requested");
    EnqueueGameThreadTask(DoReloadOnGameThread);
}

// ---------------------------------------------------------------------------
// SwapBuffers hooks - our per-frame tick on the render thread
// ---------------------------------------------------------------------------
using SwapBuffers_t    = BOOL(WINAPI*)(HDC);
using wglSwapBuffers_t = BOOL(WINAPI*)(HDC);
SwapBuffers_t    g_origSwapBuffers    = nullptr;
wglSwapBuffers_t g_origWglSwapBuffers = nullptr;

// Cross-process reload trigger. frostmod.exe (the launcher console) signals this
// named auto-reset event when you press R; we consume it here on the render
// thread. Created in Init(); "Local\..." keeps it scoped to the logon session.
HANDLE g_reloadEvent = nullptr;

void Tick() {
    // Heartbeat: prove the render hook is actually firing. If you never see this
    // line in frostmod.log, the game isn't calling the SwapBuffers we hooked, so
    // F8 / reload can't run - that's the thing to fix, not the reload itself.
    static bool firstFrame = true;
    if (firstFrame) { firstFrame = false; Log("[tick] render hook alive - first frame presented"); }

    // Optional in-game hotkey (F8) as a fullscreen-friendly alternative to the
    // floating window, which some exclusive-fullscreen modes will hide.
    static bool prev = false;
    bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (down && !prev) RequestReload();
    prev = down;

    // Reload requested from frostmod.exe? (auto-reset event self-clears on wait.)
    if (g_reloadEvent && WaitForSingleObject(g_reloadEvent, 0) == WAIT_OBJECT_0) {
        Log("[event] reload signal received from frostmod.exe");
        RequestReload();
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

    // reload trigger shared with frostmod.exe (press R in the launcher console).
    g_reloadEvent = CreateEventA(nullptr, FALSE /*auto-reset*/, FALSE, "Local\\FrostModReload");
    if (!g_reloadEvent) Log("[init] note: could not create reload event (%lu)", GetLastError());

    // content functions (capture-and-replay). Resolve the scanner by its byte
    // signature so a stale offsets.h can't make us hook the wrong code.
    intptr_t delta = 0;
    uintptr_t scanAddr = ResolveScanner(&delta);
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
    } else {
        Log("[init] content hooks NOT installed - reload is unavailable on this build "
            "until offsets.h is updated. (mods listing + logs still work.)");
    }

    // per-frame tick (render thread). Hook both entry points OpenGL games use.
    if (HMODULE gdi = GetModuleHandleA("gdi32.dll"))
        if (auto p = GetProcAddress(gdi, "SwapBuffers"))
            InstallHook((void*)p, &hkSwapBuffers,
                        (void**)&g_origSwapBuffers, "gdi32!SwapBuffers");

    if (HMODULE gl = GetModuleHandleA("opengl32.dll"))
        if (auto p = GetProcAddress(gl, "wglSwapBuffers"))
            InstallHook((void*)p, &hkWglSwapBuffers,
                        (void**)&g_origWglSwapBuffers, "opengl32!wglSwapBuffers");

    CreateThread(nullptr, 0, UiThread, nullptr, 0, nullptr);

    Log("[init] ready. Waiting to capture the game's startup folder scan (must be loaded "
        "before it). Once you see [capture], add a .pkz and reload via R / F8 / the button.");
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        InitLogPath(hModule);   // resolve <dll folder>\frostmod.log before we log
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}
