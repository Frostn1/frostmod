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

#include "MinHook.h"
#include "offsets.h"

// ---------------------------------------------------------------------------
// small logging helper -> %TEMP%\frostmod.log  (and OutputDebugString)
// ---------------------------------------------------------------------------
namespace {

std::mutex g_logMutex;

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lk(g_logMutex);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    char path[MAX_PATH];
    if (GetTempPathA(sizeof(path), path)) {
        strcat_s(path, "frostmod.log");
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
    if (!g_scanArgs.valid.load()) {
        Log("[capture] scanner(0x158be0) args: rcx=%p rdx=%p('%s') r8=%p('%s') r9=%p",
            a0, a1, SafeStr(a1).c_str(), a2, SafeStr(a2).c_str(), a3);
    }
    g_scanArgs.a0 = a0; g_scanArgs.a1 = a1; g_scanArgs.a2 = a2; g_scanArgs.a3 = a3;
    g_scanArgs.valid.store(true);
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

    if (!g_scanArgs.valid.load()) {
        Log("[reload] ABORT: scanner args not captured yet. Trigger a content "
            "scan first (e.g. open the track/bike selection menu), then retry.");
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

    Log("[reload] replay scanner(rcx=%p rdx=%p('%s') r8=%p r9=%p)",
        g_scanArgs.a0, g_scanArgs.a1, SafeStr(g_scanArgs.a1).c_str(),
        g_scanArgs.a2, g_scanArgs.a3);
    int64_t r = g_origScan(g_scanArgs.a0, g_scanArgs.a1, g_scanArgs.a2, g_scanArgs.a3);
    Log("[reload] scanner returned %lld. Done.", (long long)r);
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
    // Optional in-game hotkey (F8) as a fullscreen-friendly alternative to the
    // floating window, which some exclusive-fullscreen modes will hide.
    static bool prev = false;
    bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (down && !prev) RequestReload();
    prev = down;

    // Reload requested from frostmod.exe? (auto-reset event self-clears on wait.)
    if (g_reloadEvent && WaitForSingleObject(g_reloadEvent, 0) == WAIT_OBJECT_0)
        RequestReload();

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

    CreateWindowA("STATIC", "FrostMod  \xE2\x9D\x84",   // snowflake
                  WS_CHILD | WS_VISIBLE | SS_CENTER,
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
    Log("=============== FrostMod loading ===============");

    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));  // mxbikes.exe
    Log("[init] module base = %p", (void*)g_base);

    if (MH_Initialize() != MH_OK) { Log("[init] MinHook init failed"); return 1; }

    // reload trigger shared with frostmod.exe (press R in the launcher console).
    g_reloadEvent = CreateEventA(nullptr, FALSE /*auto-reset*/, FALSE, "Local\\FrostModReload");
    if (!g_reloadEvent) Log("[init] note: could not create reload event (%lu)", GetLastError());

    // content functions (capture-and-replay)
    InstallHook((void*)(g_base + mxb::RVA_SCAN_FOLDER),     &hkScan,
                (void**)&g_origScan,  "scanFolder(0x158be0)");
    InstallHook((void*)(g_base + mxb::RVA_REGISTRY_RESET),  &hkReset,
                (void**)&g_origReset, "registryReset(0x159340)");

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

    Log("[init] ready. Open a content menu once (to capture the scan), then after "
        "adding a .pkz reload via: R in frostmod.exe, F8 in-game, or the window button.");
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}
