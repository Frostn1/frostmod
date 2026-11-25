// setupfix.cpp - "FrostServer setup fix": the beta21f "Cheat - Bike Setup" fix for a stock
// MX Bikes DEDICATED server, delivered as dinput8.dll.
//
// WHY A dinput8.dll. A dedicated server (mxbikes.exe -dedicated <port>) never loads
// plugins\*.dlo: the game scans for plugins in its client start-up (event 0), and -dedicated runs
// the server start-up (event 45) instead. So frostserver.dlo cannot reach a dedicated server.
// mxbikes.exe does import DINPUT8.dll (DirectInput8Create, one function), and Windows looks for
// it beside the exe first. This file is that dinput8.dll: it forwards DirectInput8Create to the
// system's dinput8.dll untouched, and in a -dedicated process it installs the setup guard
// (setupguard.h) and nothing else.
//
// In a normal game (no -dedicated) it does nothing but forward. FrostMod covers players who host
// from the game, since the game does load plugins.
//
// The hooks go in from a background thread once the game's code matches beta21f byte for byte
// (the exe's code is not final while DllMain runs). On any other build it logs that and stays
// out. The log is setupfix.log beside this file, rewritten each start.
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "setupguard.h"
#include "version.h"

namespace {

HMODULE g_self = nullptr;
char    g_logPath[MAX_PATH] = {0};
CRITICAL_SECTION g_logLock;

void Log(const char* fmt, ...) {
    if (!g_logPath[0]) return;
    char msg[1200];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    EnterCriticalSection(&g_logLock);
    FILE* f = nullptr;
    if (!fopen_s(&f, g_logPath, "a") && f) {
        fprintf(f, "[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, msg);
        fclose(f);
    }
    LeaveCriticalSection(&g_logLock);
}

bool IsDedicated() {
    const char* cmd = GetCommandLineA();
    if (!cmd) return false;
    for (const char* p = cmd; *p; ++p)
        if ((p == cmd || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '"') && _strnicmp(p, "-dedicated", 10) == 0 &&
            (p[10] == 0 || p[10] == ' ' || p[10] == '\t' || p[10] == '"'))
            return true;
    return false;
}

bool IsMxBikes() {
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe, sizeof(exe));
    const char* leaf = strrchr(exe, '\\');
    return _stricmp(leaf ? leaf + 1 : exe, "mxbikes.exe") == 0;
}

bool CodeIs(uintptr_t base, uintptr_t rva, const uint8_t* want, size_t n) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (rva + n > nt->OptionalHeader.SizeOfImage) return false;
    __try { return memcmp(reinterpret_cast<const void*>(base + rva), want, n) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- the guard (same rule as FrostServer / FrostMod, setupguard.h) -----------------------
using ValidateFn = int (*)(const uint8_t* def, const uint8_t* setup);
using SetupJobFn = int (*)(int world, const uint8_t* setup);
ValidateFn g_validateOrig = nullptr;
SetupJobFn g_setupJobOrig = nullptr;
thread_local bool t_validatorRan = false;
std::atomic<unsigned long long> g_passed{0}, g_kept{0};

bool ShouldLog(unsigned long long n) { return n <= 20 || (n & (n - 1)) == 0; }

void LogFailures(const setupguard::Failure* f, size_t n, const char* verdict) {
    char line[1024];
    int at = _snprintf_s(line, sizeof(line), _TRUNCATE, "[setup] the game rejected a setup (%s):", verdict);
    for (size_t i = 0; i < n && i < 12 && at > 0 && at < (int)sizeof(line) - 80; ++i) {
        const auto& fd = setupguard::kFields[f[i].field];
        const int w = f[i].gear >= 0
            ? _snprintf_s(line + at, sizeof(line) - at, _TRUNCATE, " gear%d(+0x%X)=%d max %d",
                          f[i].gear, fd.setup + 4 * f[i].gear, f[i].value, f[i].limit)
            : _snprintf_s(line + at, sizeof(line) - at, _TRUNCATE, " %s(+0x%X)=%d max %d [def+0x%X]",
                          fd.name, fd.setup, f[i].value, f[i].limit, fd.limit);
        if (w > 0) at += w;
    }
    Log("%s", line);
}

size_t EvaluateSafely(const uint8_t* def, const uint8_t* setup, setupguard::Failure* f, size_t cap, bool* ok) {
    __try { *ok = true; return setupguard::Evaluate(def, setup, f, cap); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *ok = false; return 0; }
}

int GuardValidate(const uint8_t* def, const uint8_t* setup) {
    t_validatorRan = true;
    const int rc = g_validateOrig(def, setup);
    if (rc == 0 || !def || !setup) return rc;
    setupguard::Failure f[48];
    bool ok = false;
    size_t n = EvaluateSafely(def, setup, f, 48, &ok);
    if (!ok) { Log("[setup] could not read the setup or the bike; the game's ban stands"); return rc; }
    if (n > 48) n = 48;
    if (setupguard::OnlyBroken(f, n)) {
        const unsigned long long k = g_passed.fetch_add(1) + 1;
        if (ShouldLog(k)) LogFailures(f, n, "a beta21f mistake, let through");
        return 0;
    }
    const unsigned long long k = g_kept.fetch_add(1) + 1;
    if (ShouldLog(k)) LogFailures(f, n, n ? "out of range, the game's ban stands" : "no field out of range by our table");
    return rc;
}

int GuardSetupJob(int world, const uint8_t* setup) {
    t_validatorRan = false;
    const int rc = g_setupJobOrig(world, setup);
    if (rc != 0 && !t_validatorRan)
        Log("[setup] no bike found for world %d, so the game bans this rider for Bike Setup without "
            "looking at the setup", world);
    return rc;
}

DWORD WINAPI InstallThread(void*) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    using namespace setupguard;
    // Wait for the game's code to be final, then require beta21f's exact bytes (up to 60 s).
    bool match = false;
    for (int i = 0; i < 600 && !match; ++i) {
        match = CodeIs(base, kRvaValidator, kValidator, kValidatorLen) &&
                CodeIs(base, kRvaSetupJob, kSetupJob, kSetupJobLen);
        if (!match) Sleep(100);
    }
    if (!match) {
        Log("[setup] not applied: this mxbikes.exe is not beta21f (or the setup check moved). Nothing patched.");
        return 0;
    }
    if (MH_Initialize() != MH_OK) { Log("[setup] not applied: MinHook init failed"); return 0; }
    void* v = reinterpret_cast<void*>(base + kRvaValidator);
    void* j = reinterpret_cast<void*>(base + kRvaSetupJob);
    if (MH_CreateHook(v, reinterpret_cast<void*>(&GuardValidate), reinterpret_cast<void**>(&g_validateOrig)) != MH_OK ||
        MH_CreateHook(j, reinterpret_cast<void*>(&GuardSetupJob), reinterpret_cast<void**>(&g_setupJobOrig)) != MH_OK ||
        MH_EnableHook(v) != MH_OK || MH_EnableHook(j) != MH_OK) {
        MH_DisableHook(v);
        MH_DisableHook(j);
        Log("[setup] not applied: could not hook +0x%llX / +0x%llX", (unsigned long long)kRvaValidator,
            (unsigned long long)kRvaSetupJob);
        return 0;
    }
    Log("[setup] on: a setup the game rejects is re-checked field by field; only a tyre size checked "
        "against an empty size list on this server is let through (hooks at +0x%llX, +0x%llX)",
        (unsigned long long)kRvaValidator, (unsigned long long)kRvaSetupJob);
    return 0;
}

// ---- the dinput8 forward ---------------------------------------------------------------
using DI8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
DI8CreateFn RealDI8Create() {
    static DI8CreateFn fn = nullptr;
    if (!fn) {
        char path[MAX_PATH];
        const UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 16) return nullptr;
        strcat_s(path, "\\dinput8.dll");
        HMODULE m = LoadLibraryA(path);
        if (m) fn = reinterpret_cast<DI8CreateFn>(GetProcAddress(m, "DirectInput8Create"));
    }
    return fn;
}

}  // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid,
                                                                   LPVOID* out, LPUNKNOWN outer) {
    const DI8CreateFn fn = RealDI8Create();
    if (!fn) { if (out) *out = nullptr; return E_FAIL; }
    return fn(inst, version, riid, out, outer);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    g_self = h;
    DisableThreadLibraryCalls(h);
    if (!IsDedicated() || !IsMxBikes()) return TRUE;   // a normal game: forward only
    InitializeCriticalSection(&g_logLock);
    char dir[MAX_PATH] = {0};
    if (GetModuleFileNameA(h, dir, MAX_PATH)) {
        char* slash = strrchr(dir, '\\');
        if (slash) {
            slash[1] = 0;
            _snprintf_s(g_logPath, sizeof(g_logPath), _TRUNCATE, "%ssetupfix.log", dir);
            DeleteFileA(g_logPath);
        }
    }
    Log("FrostServer setup fix %s (dinput8.dll) in a dedicated server", FROSTMOD_VERSION);
    HANDLE t = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else Log("[setup] not applied: could not start the install thread");
    return TRUE;
}
