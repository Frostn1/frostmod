// ============================================================================
//  frostmod.exe - FrostMod launcher & live monitor.
//
//  Unlike a fire-and-forget injector, this STAYS OPEN and gives you feedback:
//
//    1. Stays resident and watches for mxbikes.exe: injects frostmod.dll when
//       the game launches (classic CreateRemoteThread + LoadLibraryA), and
//       RE-INJECTS a fresh copy every time you relaunch the game - so a rebuilt
//       DLL always takes effect without any manual restart juggling.
//    2. Lists the .pkz mods it found in your MX Bikes mods folder, and keeps
//       watching that folder - new / removed .pkz files are printed live.
//    3. Streams frostmod.log (what the injected DLL writes) to this console in
//       real time, so you can see the capture / reload activity.
//    4. Press  R  to reload mods,  Q  (or Ctrl+C) to quit. Reload also works
//       from inside the game (F8 / the floating window).
//
//  The log lives NEXT TO the binaries (frostmod.exe + frostmod.dll are in the
//  same folder), so the exe and the injected dll always agree on the file even
//  if the game's %TEMP% differs. Falls back to %TEMP% if that folder is read-only.
//
//  Usage:
//     frostmod.exe                         (auto: waits for mxbikes.exe,
//                                           loads .\frostmod.dll, watches
//                                           Documents\PiBoSo\MX Bikes\mods)
//     frostmod.exe --process gpbikes.exe   (different game)
//     frostmod.exe --mods "D:\path\mods"   (override the mods folder)
//     frostmod.exe C:\path\frostmod.dll    (explicit DLL path)
//
//  Run as the same user as the game (and elevated if the game is elevated).
// ============================================================================

#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>       // SHGetFolderPathA / CSIDL_PERSONAL
#include <conio.h>        // _kbhit / _getch
#include <cstdio>
#include <cstring>        // strrchr / strcmp / _stricmp / _strnicmp
#include <string>
#include <set>

// ---------------------------------------------------------------------------
// graceful shutdown
// ---------------------------------------------------------------------------
static volatile bool g_running = true;

static BOOL WINAPI CtrlHandler(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// find a process by exe name
// ---------------------------------------------------------------------------
static DWORD FindProcess(const char* name) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe{ sizeof(pe) };
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// Is a module (e.g. frostmod.dll) already loaded in the target process? Windows
// won't reload a live DLL, so if it's already there a rebuild won't take effect
// until the game restarts - we detect that and warn instead of silently running
// the old code.
static bool IsModuleLoaded(DWORD pid, const char* moduleName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32 me{ sizeof(me) };
    bool found = false;
    if (Module32First(snap, &me)) {
        do {
            if (_stricmp(me.szModule, moduleName) == 0) { found = true; break; }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// ---------------------------------------------------------------------------
// inject frostmod.dll into the target process
// ---------------------------------------------------------------------------
static bool InjectDll(DWORD pid, const std::string& dllPath) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                              PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                              FALSE, pid);
    if (!proc) { printf("[!] OpenProcess failed (%lu). Run elevated?\n", GetLastError()); return false; }

    SIZE_T len = dllPath.size() + 1;
    void* remote = VirtualAllocEx(proc, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { printf("[!] VirtualAllocEx failed (%lu)\n", GetLastError()); CloseHandle(proc); return false; }

    if (!WriteProcessMemory(proc, remote, dllPath.c_str(), len, nullptr)) {
        printf("[!] WriteProcessMemory failed (%lu)\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE); CloseHandle(proc); return false;
    }

    auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!thread) {
        printf("[!] CreateRemoteThread failed (%lu)\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE); CloseHandle(proc); return false;
    }

    WaitForSingleObject(thread, 5000);
    DWORD loaded = 0; GetExitCodeThread(thread, &loaded);

    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(thread);
    CloseHandle(proc);

    if (!loaded) { printf("[!] remote LoadLibrary returned 0 - DLL failed to load.\n"); return false; }
    printf("[+] injected (module handle 0x%lX in target).\n", loaded);
    return true;
}

// ---------------------------------------------------------------------------
// paths: this exe's folder, the log file, and the default mods folder
// ---------------------------------------------------------------------------
static std::string ExeDir() {
    char p[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, p, sizeof(p))) return "";
    if (char* slash = strrchr(p, '\\')) *(slash + 1) = 0;
    return p;
}

// Log lives next to the binaries so the exe and the injected dll agree on it.
// (frostmod.dll uses the exact same rule.) Falls back to %TEMP% if read-only.
static std::string LogPath() {
    std::string dir = ExeDir();
    if (!dir.empty()) {
        std::string cand = dir + "frostmod.log";
        if (FILE* f = nullptr; fopen_s(&f, cand.c_str(), "a") == 0 && f) { fclose(f); return cand; }
    }
    char t[MAX_PATH];
    if (GetTempPathA(sizeof(t), t)) return std::string(t) + "frostmod.log";
    return dir + "frostmod.log";
}

static std::string DefaultModsPath() {
    // MX Bikes (PiBoSo) keeps user content under Documents\PiBoSo\MX Bikes\mods.
    char docs[MAX_PATH];
    if (SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs) != S_OK)
        return "";
    return std::string(docs) + "\\PiBoSo\\MX Bikes\\mods";
}

static void EnumPkz(const std::string& dir, std::set<std::string>& out) {
    std::string pattern = dir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        std::string fullPath = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            EnumPkz(fullPath, out);                        // recurse into subfolders
        } else {
            const char* ext = strrchr(fd.cFileName, '.');
            if (ext && _stricmp(ext, ".pkz") == 0) out.insert(fullPath);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// show a path relative to the mods root, for readability
static std::string Rel(const std::string& modsRoot, const std::string& full) {
    if (full.size() > modsRoot.size() &&
        _strnicmp(full.c_str(), modsRoot.c_str(), modsRoot.size()) == 0) {
        size_t i = modsRoot.size();
        while (i < full.size() && (full[i] == '\\' || full[i] == '/')) ++i;
        return full.substr(i);
    }
    return full;
}

// ---------------------------------------------------------------------------
// tail the log -> this console (only the current session's new lines).
// We re-open the file each poll: cheap, and immune to stale-handle/caching
// issues when another process is appending to it.
// ---------------------------------------------------------------------------
static LONGLONG g_logPos    = 0;      // where we've read up to
static LONGLONG g_logStart  = 0;      // size of the log before we injected
static bool     g_logOpened = false;  // have we anchored g_logPos yet?
static bool     g_sawDllLog = false;  // did the DLL ever write anything we read?

static void TailLog(const std::string& path) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;                 // not created yet
    if (!g_logOpened) { g_logPos = g_logStart; g_logOpened = true; }

    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz)) {
        if (sz.QuadPart < g_logPos) g_logPos = 0;          // rotated / truncated
        while (g_logPos < sz.QuadPart) {
            LARGE_INTEGER mv; mv.QuadPart = g_logPos;
            SetFilePointerEx(h, mv, nullptr, FILE_BEGIN);
            char buf[8192];
            LONGLONG remain = sz.QuadPart - g_logPos;
            DWORD toRead = (DWORD)(remain < (LONGLONG)sizeof(buf) ? remain : sizeof(buf));
            DWORD got = 0;
            if (!ReadFile(h, buf, toRead, &got, nullptr) || got == 0) break;
            fwrite(buf, 1, got, stdout);
            g_logPos += got;
            g_sawDllLog = true;
        }
        fflush(stdout);
    }
    CloseHandle(h);
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    SetConsoleTitleA("FrostMod");
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    const char* processName = "mxbikes.exe";
    std::string dllPath;
    std::string modsPath;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--process" && i + 1 < argc)    processName = argv[++i];
        else if (a == "--mods" && i + 1 < argc)  modsPath = argv[++i];
        else                                     dllPath = a;
    }

    // default DLL path: frostmod.dll next to this exe
    if (dllPath.empty()) dllPath = ExeDir() + "frostmod.dll";
    // make it absolute (LoadLibrary in the target runs from the game's cwd)
    char full[MAX_PATH];
    if (GetFullPathNameA(dllPath.c_str(), sizeof(full), full, nullptr))
        dllPath = full;

    if (GetFileAttributesA(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        printf("[!] DLL not found: %s\n", dllPath.c_str());
        return 1;
    }

    if (modsPath.empty()) modsPath = DefaultModsPath();
    bool modsExist = !modsPath.empty() &&
                     GetFileAttributesA(modsPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    const std::string logPath = LogPath();

    printf("================== FrostMod ==================\n");
    printf("[*] build : " __DATE__ " " __TIME__ "  (this exe)\n");
    printf("[*] DLL   : %s\n", dllPath.c_str());
    printf("[*] mods  : %s%s\n", modsPath.empty() ? "<unknown>" : modsPath.c_str(),
           (!modsPath.empty() && !modsExist) ? "  (not found - pass --mods \"...\")" : "");
    printf("[*] log   : %s\n", logPath.c_str());
    printf("=============================================\n");

    // cross-process reload trigger (the DLL watches the same named event).
    HANDLE reloadEvent = CreateEventA(nullptr, FALSE /*auto-reset*/, FALSE, "Local\\FrostModReload");

    // base name of the DLL (for the "already loaded?" check)
    std::string dllName = dllPath;
    if (size_t s = dllName.find_last_of("\\/"); s != std::string::npos) dllName = dllName.substr(s + 1);

    // ----- resident loop: inject on each game launch, re-inject on relaunch ---
    // We never quit when the game closes - we wait for it to come back. Only Q /
    // Ctrl+C exits. Every fresh game process gets a fresh DLL (DllMain re-runs),
    // which sidesteps the "Windows won't hot-swap a live DLL" trap entirely.
    DWORD     injectedPid  = 0;      // pid we've handled this session (0 = none)
    DWORD     pendingPid   = 0;      // a new pid, warming up before we inject
    ULONGLONG pendingSince = 0;
    ULONGLONG injectedAt   = 0;
    bool      alreadyLoaded = false;
    bool      hintShown     = false;
    bool      announcedWait = false;
    ULONGLONG lastScan      = 0;
    std::set<std::string> known;

    while (g_running) {
        DWORD pid = FindProcess(processName);

        // ---- game not running: reset session state, wait -------------------
        if (!pid) {
            if (injectedPid) {
                printf("\n[*] %s closed. Waiting for it to launch again... (Q/Ctrl+C quits)\n",
                       processName);
                injectedPid = 0; pendingPid = 0; announcedWait = true;
            } else if (!announcedWait) {
                printf("[*] waiting for %s to start (Q/Ctrl+C quits)...\n", processName);
                announcedWait = true;
            }
            if (_kbhit()) { int c = _getch(); if (c == 'q' || c == 'Q') break; }
            Sleep(400);
            continue;
        }

        // ---- a new game process appeared: warm up, then inject a FRESH dll --
        if (pid != injectedPid) {
            if (pendingPid != pid) {                       // first sighting of this pid
                pendingPid = pid; pendingSince = GetTickCount64();
                printf("[*] %s detected (pid=%lu) - letting it initialize...\n", processName, pid);
            }
            if (GetTickCount64() - pendingSince < 2000) {  // ~2s warmup before inject
                if (_kbhit()) { int c = _getch(); if (c == 'q' || c == 'Q') break; }
                Sleep(150);
                continue;
            }

            // anchor the log tail to the current end so we stream THIS session.
            g_logStart = 0; g_logPos = 0; g_logOpened = false; g_sawDllLog = false;
            if (HANDLE h = CreateFileA(logPath.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, 0, nullptr);
                h != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER sz; if (GetFileSizeEx(h, &sz)) g_logStart = sz.QuadPart;
                CloseHandle(h);
            }
            hintShown = false;

            alreadyLoaded = IsModuleLoaded(pid, dllName.c_str());
            if (alreadyLoaded) {
                printf("[!] %s is already loaded in pid %lu (another injector running?).\n"
                       "    Not re-injecting; a rebuilt DLL needs a full game restart.\n",
                       dllName.c_str(), pid);
            } else {
                printf("[*] injecting into pid %lu...\n", pid);
                if (!InjectDll(pid, dllPath)) {
                    printf("[!] injection failed (elevation? run frostmod.exe as admin).\n"
                           "    Will retry when the game is relaunched.\n");
                    injectedPid = pid; pendingPid = 0;     // don't spin on this pid
                    Sleep(500);
                    continue;
                }
            }
            injectedPid = pid; pendingPid = 0; announcedWait = false;
            injectedAt = GetTickCount64();

            // list mods for this session
            known.clear();
            if (modsExist) EnumPkz(modsPath, known);
            printf("\nMods found (%zu):\n", known.size());
            if (known.empty())
                printf("  (none%s)\n", modsExist ? " - drop .pkz files into the mods folder" : "");
            else
                for (const auto& m : known) printf("  - %s\n", Rel(modsPath, m).c_str());
            printf("\nIMPORTANT: MX Bikes scans the mods folder only ONCE, at startup.\n"
                   "  For reload to work, FrostMod must be loaded BEFORE that scan - i.e. start\n"
                   "  frostmod.exe FIRST, then launch the game, and watch for a [capture] line\n"
                   "  during loading. If you injected into an already-running game, the scan was\n"
                   "  already done: quit the game (leave this running) and relaunch it.\n");
            printf("\n--- live log ---   [R] reload mods   [Q]/Ctrl+C quit\n");
        }

        // ---- monitor the running, injected game ----------------------------
        TailLog(logPath);

        // if the (freshly injected) DLL never logs, something's off - say so once.
        if (!hintShown && !alreadyLoaded && !g_sawDllLog && GetTickCount64() - injectedAt > 4000) {
            hintShown = true;
            printf("[!] no output from frostmod.dll yet. It loaded, but isn't logging to\n"
                   "    %s\n"
                   "    Check that this folder is writable, or that the render hook ticked.\n",
                   logPath.c_str());
        }

        // re-scan the mods folder about once a second and report changes
        ULONGLONG now = GetTickCount64();
        if (modsExist && now - lastScan >= 1000) {
            lastScan = now;
            std::set<std::string> current;
            EnumPkz(modsPath, current);
            for (const auto& m : current)
                if (!known.count(m)) printf("[mods] + new : %s\n", Rel(modsPath, m).c_str());
            for (const auto& m : known)
                if (!current.count(m)) printf("[mods] - gone: %s\n", Rel(modsPath, m).c_str());
            known.swap(current);
        }

        // keyboard: R = reload, Q = quit
        if (_kbhit()) {
            int c = _getch();
            if (c == 'r' || c == 'R') {
                if (reloadEvent) { SetEvent(reloadEvent); printf("[you] reload requested (R)\n"); }
            } else if (c == 'q' || c == 'Q') {
                break;
            }
        }

        Sleep(150);
    }

    if (reloadEvent) CloseHandle(reloadEvent);
    return 0;
}
