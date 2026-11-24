#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "fixesguard.h"
#include "guards.h"
#include "hookutil.h"
#include "offsets.h"
#include "version.h"

namespace {

HMODULE g_self = nullptr;
HANDLE g_worker = nullptr;
HANDLE g_stop = nullptr;
FILE* g_log = nullptr;
SRWLOCK g_log_lock = SRWLOCK_INIT;
std::atomic<bool> g_started{false};
std::atomic<bool> g_unloading{false};
std::atomic<bool> g_stand_down{false};
char g_save_path[MAX_PATH] = {};
char g_plugin_dir[MAX_PATH] = {};

struct OverlapState {
    bool mxbfix_present = false;
    int values[fixesguard::kOverlaps.size()] = {};
};
OverlapState g_overlap;

void Log(const char* fmt, ...) {
    AcquireSRWLockExclusive(&g_log_lock);
    if (g_log) {
        va_list args;
        va_start(args, fmt);
        std::vfprintf(g_log, fmt, args);
        va_end(args);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }
    ReleaseSRWLockExclusive(&g_log_lock);
}

void ParentDir(char* path) {
    if (char* slash = std::strrchr(path, '\\')) *slash = '\0';
}

bool JoinPath(char* out, size_t cap, const char* dir, const char* leaf) {
    if (!out || !cap || !dir || !leaf) return false;
    const size_t n = std::strlen(dir);
    const char* sep = n && (dir[n - 1] == '\\' || dir[n - 1] == '/') ? "" : "\\";
    return sprintf_s(out, cap, "%s%s%s", dir, sep, leaf) > 0;
}

bool FileExists(const char* path) {
    const DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

void OpenLog() {
    char path[MAX_PATH] = {};
    if (!JoinPath(path, sizeof(path), g_plugin_dir, "frostmod_fixes.log")) return;
    FILE* opened = nullptr;
    if (fopen_s(&opened, path, "w") == 0) g_log = opened;
    Log("FrostMod Fixes v" FROSTMOD_VERSION " - crash guards only (no overlay, no network)");
}

void ResetConfigDefaults() {
    guards::mapguard_on.store(true);
    guards::mapguard_test_every.store(0);
    guards::shockguard_on.store(true);
    guards::bikelistguard_on.store(true);
    guards::wakeguard_on.store(true);
    guards::listboxguard_on.store(true);
    guards::ptr64fix_on.store(true);
    guards::riderinfoguard_on.store(true);
    guards::mipmapguard_on.store(true);
    guards::eventcfgguard_on.store(true);
    guards::nanrecover_on.store(true);
}

void ReadConfig() {
    ResetConfigDefaults();
    char path[MAX_PATH] = {};
    if (!JoinPath(path, sizeof(path), g_plugin_dir, "frostmod_fixes.cfg")) return;
    FILE* file = nullptr;
    if (fopen_s(&file, path, "r") || !file) return;
    char line[160] = {};
    int value = 0;
    while (std::fgets(line, sizeof(line), file)) {
        if (sscanf_s(line, "mapguard=%d", &value) == 1) guards::mapguard_on.store(value != 0);
        else if (sscanf_s(line, "mapguardtest=%d", &value) == 1) guards::mapguard_test_every.store(value);
        else if (sscanf_s(line, "shockguard=%d", &value) == 1) guards::shockguard_on.store(value != 0);
        else if (sscanf_s(line, "bikelistguard=%d", &value) == 1) guards::bikelistguard_on.store(value != 0);
        else if (sscanf_s(line, "wakeguard=%d", &value) == 1) guards::wakeguard_on.store(value != 0);
        else if (sscanf_s(line, "listboxguard=%d", &value) == 1) guards::listboxguard_on.store(value != 0);
        else if (sscanf_s(line, "ptr64fix=%d", &value) == 1) guards::ptr64fix_on.store(value != 0);
        else if (sscanf_s(line, "riderinfoguard=%d", &value) == 1) guards::riderinfoguard_on.store(value != 0);
        else if (sscanf_s(line, "mipmapguard=%d", &value) == 1) guards::mipmapguard_on.store(value != 0);
        else if (sscanf_s(line, "eventcfgguard=%d", &value) == 1) guards::eventcfgguard_on.store(value != 0);
        else if (sscanf_s(line, "nanrecover=%d", &value) == 1) guards::nanrecover_on.store(value != 0);
    }
    std::fclose(file);
}

// Only looks: full FrostMod's ClaimProcess() makes this name, and a second FrostMod that
// finds it stands down. Creating it here would make a later full FrostMod stand down in
// favour of the guards-only build.
bool FullMutexExists() {
    char name[64] = {};
    sprintf_s(name, "Local\\FrostModActive-%lu", GetCurrentProcessId());
    HANDLE h = OpenMutexA(SYNCHRONIZE, FALSE, name);
    if (!h) return false;
    CloseHandle(h);
    return true;
}

fixesguard::StandDownDecision DoubleLoadDecision() {
    char full_path[MAX_PATH] = {};
    JoinPath(full_path, sizeof(full_path), g_plugin_dir, "frostmod.dlo");
    const bool full_file = FileExists(full_path);
    const bool full_mutex = !full_file && FullMutexExists();
    const bool full_module = GetModuleHandleA("frostmod.dll") != nullptr ||
                             GetModuleHandleA("frostmod.dlo") != nullptr;
    return fixesguard::DecideDoubleLoad(full_file, full_mutex, full_module);
}

void ReadMxbfixOverlap() {
    char plugin[MAX_PATH] = {};
    JoinPath(plugin, sizeof(plugin), g_plugin_dir, "mxbfix.dlo");
    g_overlap.mxbfix_present = FileExists(plugin);
    if (!g_overlap.mxbfix_present) return;

    char ini[MAX_PATH] = {};
    JoinPath(ini, sizeof(ini), g_save_path, "mxbfix\\mxbfix.ini");
    for (size_t i = 0; i < fixesguard::kOverlaps.size(); ++i) {
        const auto& overlap = fixesguard::kOverlaps[i];
        g_overlap.values[i] = GetPrivateProfileIntA("fixes", overlap.mxbfix_key, 1, ini);
    }
}

bool UseGuard(fixesguard::Guard guard) {
    const auto* overlap = fixesguard::FindOverlap(guard);
    if (!overlap || !g_overlap.mxbfix_present) return true;
    const size_t index = static_cast<size_t>(overlap - fixesguard::kOverlaps.data());
    if (fixesguard::UseFrostModGuard(guard, true, g_overlap.values[index])) return true;
    Log("[mxbfix] %s off: MXBFix's %s already fixes this crash (set %s=0 in mxbfix.ini "
        "to use FrostMod's instead)",
        overlap->guard_name, overlap->mxbfix_key, overlap->mxbfix_key);
    return false;
}

void PinSelf() {
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&PinSelf), &pinned))
        Log("[unload] could not pin the module (error %lu)", GetLastError());
}

DWORD WINAPI GuardWorker(void*) {
    if (MH_Initialize() != MH_OK) {
        Log("[init] MinHook init failed");
        return 1;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    hookutil::SetLog(&Log);
    hookutil::SetUnloadingFlag(&g_unloading);
    hookutil::SetGameContext(base, &GAME_MXB);

    intptr_t delta = 0;
    const uintptr_t scanner = hookutil::WaitForScanner(&delta, 30000);
    if (g_unloading.load()) return 0;
    if (!scanner) {
        Log("[init] game code did not become ready; no crash guards were installed");
        return 1;
    }
    guards::Configure({&Log, base, delta, &GAME_MXB, nullptr});
    if (delta)
        Log("[sig] build drift detected: delta = 0x%zx (fixed RVAs are adjusted by this).",
            static_cast<size_t>(delta));

    if (UseGuard(fixesguard::Guard::Ghs)) guards::InstallGhsGuard();
    if (UseGuard(fixesguard::Guard::Terrain)) guards::InstallTerrainGuard();
    if (UseGuard(fixesguard::Guard::Trainer)) guards::InstallTrainerGuard();
    guards::InstallTyreFallback();
    guards::InstallNanRecoverHook(delta);
    if (UseGuard(fixesguard::Guard::Map)) guards::InstallMapGuard();
    if (UseGuard(fixesguard::Guard::Shock)) guards::InstallShockGuard();
    guards::InstallSessionGuards();

    while (!g_unloading.load()) {
        if (g_stop) {
            if (WaitForSingleObject(g_stop, 1000) == WAIT_OBJECT_0) break;
        } else {
            Sleep(1000);
        }
        guards::Tick();
    }
    return 0;
}

void StopWorker() {
    g_unloading.store(true);
    guards::SetUnloading(true);
    if (g_stop) SetEvent(g_stop);
    if (g_worker) {
        if (WaitForSingleObject(g_worker, 2000) != WAIT_OBJECT_0)
            Log("[unload] guard worker still running; it can install nothing from here on");
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
}

void Teardown() {
    StopWorker();
    const MH_STATUS status = MH_DisableHook(MH_ALL_HOOKS);
    if (status != MH_OK && status != MH_ERROR_NOT_INITIALIZED)
        Log("[unload] MH_DisableHook(all) failed: %s", MH_StatusToString(status));
    guards::RevertTyreFallback();
    guards::RevertMapGuard();
    guards::RevertShockGuard();
}

}  // namespace

extern "C" {

__declspec(dllexport) char* GetModID() {
    static char id[] = "frostmod_fixes";
    return id;
}
__declspec(dllexport) int GetModDataVersion() { return GAME_MXB.plugin_data_version; }
__declspec(dllexport) int GetInterfaceVersion() { return kPluginInterfaceVersion; }

__declspec(dllexport) int Startup(char* save_path) {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return 3;
    if (save_path) strncpy_s(g_save_path, save_path, _TRUNCATE);
    if (g_self) GetModuleFileNameA(g_self, g_plugin_dir, sizeof(g_plugin_dir));
    ParentDir(g_plugin_dir);
    OpenLog();
    ReadConfig();

    const auto decision = DoubleLoadDecision();
    if (!decision.install) {
        g_stand_down.store(true);
        Log("[init] %s", decision.message);
        return 3;
    }

    ReadMxbfixOverlap();
    PinSelf();
    g_stop = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_worker = CreateThread(nullptr, 0, &GuardWorker, nullptr, 0, nullptr);
    if (!g_worker) Log("[init] could not start guard worker (error %lu); no guards installed",
                       GetLastError());
    return 3;
}

__declspec(dllexport) void Shutdown() {
    PinSelf();
    if (!g_stand_down.load()) Teardown();
    if (g_stop) { CloseHandle(g_stop); g_stop = nullptr; }
    Log("[unload] Shutdown complete");
    AcquireSRWLockExclusive(&g_log_lock);
    FILE* file = g_log;
    g_log = nullptr;
    if (file) std::fclose(file);
    ReleaseSRWLockExclusive(&g_log_lock);
}

__declspec(dllexport) void Draw(int, int* quads, void**, int* strings, void**) {
    if (quads) *quads = 0;
    if (strings) *strings = 0;
}
__declspec(dllexport) void RunTelemetry(void*, int, float, float) {}
__declspec(dllexport) void RaceTrackPosition(int, void*, int) {}
__declspec(dllexport) void RaceAddEntry(void*, int) {}
__declspec(dllexport) void RaceRemoveEntry(void*, int) {}
__declspec(dllexport) void RaceClassification(void*, int, void*, int) {}
__declspec(dllexport) void EventInit(void*, int) {}
__declspec(dllexport) void EventDeinit() {}
__declspec(dllexport) void RaceEvent(void*, int) {}
__declspec(dllexport) void RaceSession(void*, int) {}
__declspec(dllexport) void RaceDeinit() {}
__declspec(dllexport) void RunInit(void*, int) {}
__declspec(dllexport) void RunDeinit() {}
__declspec(dllexport) void RunStart() {}
__declspec(dllexport) void RunStop() {}
__declspec(dllexport) void RunLap(void*, int) {}
__declspec(dllexport) void RunSplit(void*, int) {}
__declspec(dllexport) void RaceSessionState(void*, int) {}
__declspec(dllexport) void RaceLap(void*, int) {}
__declspec(dllexport) void RaceSplit(void*, int) {}
__declspec(dllexport) void RaceCommunication(void*, int) {}

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
