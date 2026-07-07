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
//       from inside the game (F8, or the in-game overlay hint).
//
//  The log lives NEXT TO the binaries (frostmod.exe + frostmod.dll are in the
//  same folder), so the exe and the injected dll always agree on the file even
//  if the game's %TEMP% differs. Falls back to %TEMP% if that folder is read-only.
//
//  Usage:
//     frostmod.exe                         (no flags needed: mod reload + the
//                                           server filter are both ON. Waits for
//                                           mxbikes.exe, loads .\frostmod.dll.)
//     frostmod.exe --install-startup       (run automatically at login from now on,
//                                           minimized, and keep running now)
//     frostmod.exe --update                (download + install the latest release)
//     frostmod.exe --uninstall-startup     (stop running at login)
//     frostmod.exe --no-update-check       (don't check GitHub for a newer version)
//     frostmod.exe --no-filter-servers     (reload only; leave the browser alone)
//     frostmod.exe --process gpbikes.exe   (different game)
//     frostmod.exe --mods "D:\path\mods"   (override the mods folder)
//     frostmod.exe C:\path\frostmod.dll    (explicit DLL path)
//
//  Auto-start uses a per-user HKCU\...\Run entry (no admin). Because FrostMod already
//  watches for the game and injects on launch, "run at startup" gives you the
//  "when the game runs, FrostMod runs" behaviour with nothing to configure per-game.
//
//  Run as the same user as the game (and elevated if the game is elevated).
// ============================================================================

#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>       // SHGetFolderPathA / CSIDL_PERSONAL
#include <conio.h>        // _kbhit / _getch
#include <winhttp.h>      // GitHub update check
#include <shellapi.h>     // ShellExecuteA (relaunch after --update)
#include <cstdio>
#include <cstring>        // strrchr / strcmp / _stricmp / _strnicmp
#include <string>
#include <set>
#include "version.h"    // FROSTMOD_VERSION

// Where updates come from (public GitHub Releases).
#define FROSTMOD_UPDATE_HOST L"api.github.com"
#define FROSTMOD_UPDATE_PATH L"/repos/Frostn1/frostmod/releases/latest"
#define FROSTMOD_RELEASES_URL "https://github.com/Frostn1/frostmod/releases/latest"

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
static bool InjectDll(DWORD pid, const std::string& dllPath, bool quiet = false) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                              PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                              FALSE, pid);
    if (!proc) { if (!quiet) printf("[!] OpenProcess failed (%lu). Run elevated?\n", GetLastError()); return false; }

    SIZE_T len = dllPath.size() + 1;
    void* remote = VirtualAllocEx(proc, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { if (!quiet) printf("[!] VirtualAllocEx failed (%lu)\n", GetLastError()); CloseHandle(proc); return false; }

    if (!WriteProcessMemory(proc, remote, dllPath.c_str(), len, nullptr)) {
        if (!quiet) printf("[!] WriteProcessMemory failed (%lu)\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE); CloseHandle(proc); return false;
    }

    auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!thread) {
        if (!quiet) printf("[!] CreateRemoteThread failed (%lu)\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE); CloseHandle(proc); return false;
    }

    WaitForSingleObject(thread, 5000);
    DWORD loaded = 0; GetExitCodeThread(thread, &loaded);

    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(thread);
    CloseHandle(proc);

    if (!loaded) { if (!quiet) printf("[!] remote LoadLibrary returned 0 - DLL failed to load.\n"); return false; }
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
// auto-start at login: a HKCU Run entry (no admin needed) so FrostMod is always
// watching and injects the moment MX Bikes launches - however you start the game.
// ---------------------------------------------------------------------------
static const char* kRunKey = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char* kRunVal = "FrostMod";

static bool InstallStartup() {
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, exe, sizeof(exe))) return false;
    // Run minimized at login (--startup) so it's out of the way but still available.
    std::string cmd = "\"" + std::string(exe) + "\" --startup";
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &hk, nullptr) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExA(hk, kRunVal, 0, REG_SZ,
                            (const BYTE*)cmd.c_str(), (DWORD)cmd.size() + 1);
    RegCloseKey(hk);
    return r == ERROR_SUCCESS;
}

static bool UninstallStartup() {
    HKEY hk;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
        return true;   // key absent => nothing to remove
    LONG r = RegDeleteValueA(hk, kRunVal);
    RegCloseKey(hk);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

static bool IsStartupInstalled() {
    HKEY hk;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return false;
    LONG r = RegQueryValueExA(hk, kRunVal, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(hk);
    return r == ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// update check: ask the GitHub Releases API for the latest tag and, if it's newer
// than this build, tell the user. Read-only (no self-modification) - safe.
// ---------------------------------------------------------------------------
// HTTPS GET via WinHTTP. Returns the response body, or "" on any failure (offline,
// rate-limited, TLS error) - the caller treats "" as "no update info, skip".
static std::string HttpsGet(const wchar_t* host, const wchar_t* path) {
    std::string out;
    HINTERNET hSes = WinHttpOpen(L"FrostMod-Updater",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) return out;
    WinHttpSetTimeouts(hSes, 3000, 3000, 4000, 4000);  // don't hang startup on a bad network
    if (HINTERNET hCon = WinHttpConnect(hSes, host, INTERNET_DEFAULT_HTTPS_PORT, 0)) {
        if (HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", path, nullptr,
                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
            const wchar_t* hdrs = L"User-Agent: FrostMod\r\nAccept: application/vnd.github+json\r\n";
            if (WinHttpSendRequest(hReq, hdrs, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hReq, nullptr)) {
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
                    std::string chunk(avail, '\0');
                    DWORD got = 0;
                    if (!WinHttpReadData(hReq, &chunk[0], avail, &got) || got == 0) break;
                    out.append(chunk.data(), got);
                }
            }
            WinHttpCloseHandle(hReq);
        }
        WinHttpCloseHandle(hCon);
    }
    WinHttpCloseHandle(hSes);
    return out;
}

// pull the first  "key": "value"  string out of a JSON blob (flat, good enough here).
static std::string JsonString(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);            if (p == std::string::npos) return "";
    p = j.find(':', p + k.size());   if (p == std::string::npos) return "";
    p = j.find('"', p);              if (p == std::string::npos) return "";
    size_t e = j.find('"', p + 1);   if (e == std::string::npos) return "";
    return j.substr(p + 1, e - p - 1);
}

// compare dotted versions ("v0.9.3" vs "0.9.2"): >0 if a newer than b.
static int VersionCompare(const std::string& a, const std::string& b) {
    auto parse = [](const std::string& s, int v[3]) {
        v[0] = v[1] = v[2] = 0;
        const char* p = s.c_str();
        if (*p == 'v' || *p == 'V') ++p;
        sscanf_s(p, "%d.%d.%d", &v[0], &v[1], &v[2]);
    };
    int x[3], y[3]; parse(a, x); parse(b, y);
    for (int i = 0; i < 3; ++i) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

static void CheckForUpdate() {
    std::string body = HttpsGet(FROSTMOD_UPDATE_HOST, FROSTMOD_UPDATE_PATH);
    if (body.empty()) return;                        // offline / rate-limited -> silent
    std::string tag = JsonString(body, "tag_name");
    if (tag.empty()) return;
    if (VersionCompare(tag, FROSTMOD_VERSION) > 0) {
        printf("\n  ============================================================\n");
        printf("   UPDATE AVAILABLE:  %s   (you have v%s)\n", tag.c_str(), FROSTMOD_VERSION);
        printf("   Download:  %s\n", FROSTMOD_RELEASES_URL);
        printf("  ============================================================\n\n");
    } else {
        printf("[*] up to date (v%s).\n", FROSTMOD_VERSION);
    }
}

// run the check off the main thread so a slow network never delays startup.
static DWORD WINAPI UpdateCheckThread(LPVOID) { CheckForUpdate(); return 0; }

// ---------------------------------------------------------------------------
// --update : download the latest release's frostmod.exe + frostmod.dll and swap
// them in. The DLL is locked while the game runs, so we require it closed; the
// running exe is replaced with the rename-self trick, then FrostMod relaunches.
// ---------------------------------------------------------------------------
// browser_download_url of the asset named `fileName` in the release JSON.
static std::string FindAssetUrl(const std::string& j, const char* fileName) {
    std::string q = std::string("\"") + fileName + "\"";
    size_t p = j.find(q);                          if (p == std::string::npos) return "";
    size_t u = j.find("\"browser_download_url\"", p); if (u == std::string::npos) return "";
    size_t c = j.find(':', u);                     if (c == std::string::npos) return "";
    size_t s = j.find('"', c);                     if (s == std::string::npos) return "";
    size_t e = j.find('"', s + 1);                 if (e == std::string::npos) return "";
    return j.substr(s + 1, e - s - 1);
}

// Download `url` (HTTPS, follows redirects - GitHub asset URLs 302 to a CDN) to a
// file. Returns true only if it wrote a non-empty file with HTTP 200.
static bool DownloadToFile(const std::string& url, const std::string& outPath) {
    wchar_t wurl[2048] = {0};
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wurl, 2047);
    URL_COMPONENTS uc; ZeroMemory(&uc, sizeof(uc)); uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[1600] = {0};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 1599;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return false;

    bool ok = false;
    HINTERNET hSes = WinHttpOpen(L"FrostMod-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (hSes) {
        WinHttpSetTimeouts(hSes, 5000, 5000, 30000, 30000);
        if (HINTERNET hCon = WinHttpConnect(hSes, host, uc.nPort, 0)) {
            DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
            if (HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", path, nullptr,
                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)) {
                const wchar_t* ua = L"User-Agent: FrostMod\r\n";
                if (WinHttpSendRequest(hReq, ua, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                    WinHttpReceiveResponse(hReq, nullptr)) {
                    DWORD status = 0, sz = sizeof(status);
                    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
                    if (status == 200) {
                        FILE* f = nullptr;
                        if (fopen_s(&f, outPath.c_str(), "wb") == 0 && f) {
                            size_t total = 0;
                            for (;;) {
                                DWORD avail = 0;
                                if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
                                std::string buf(avail, '\0'); DWORD got = 0;
                                if (!WinHttpReadData(hReq, &buf[0], avail, &got) || got == 0) break;
                                fwrite(buf.data(), 1, got, f); total += got;
                            }
                            fclose(f);
                            ok = total > 0;
                        }
                    }
                }
                WinHttpCloseHandle(hReq);
            }
            WinHttpCloseHandle(hCon);
        }
        WinHttpCloseHandle(hSes);
    }
    if (!ok) DeleteFileA(outPath.c_str());
    return ok;
}

static int DoUpdate() {
    printf("[*] checking for the latest release...\n");
    std::string body = HttpsGet(FROSTMOD_UPDATE_HOST, FROSTMOD_UPDATE_PATH);
    if (body.empty()) { printf("[!] couldn't reach GitHub (offline?). Try again later.\n"); return 1; }
    std::string tag = JsonString(body, "tag_name");
    if (tag.empty()) { printf("[!] no release found on GitHub.\n"); return 1; }
    if (VersionCompare(tag, FROSTMOD_VERSION) <= 0) {
        printf("[+] already up to date (v%s is the latest).\n", FROSTMOD_VERSION);
        return 0;
    }
    printf("[*] updating v%s -> %s\n", FROSTMOD_VERSION, tag.c_str());

    // the DLL is loaded (and locked) while MX Bikes runs, so it must be closed.
    if (FindProcess("mxbikes.exe")) {
        printf("[!] MX Bikes is running - close it, then run  frostmod.exe --update  again.\n");
        return 1;
    }

    std::string exeUrl = FindAssetUrl(body, "frostmod.exe");
    std::string dllUrl = FindAssetUrl(body, "frostmod.dll");
    if (exeUrl.empty() || dllUrl.empty()) {
        printf("[!] release %s is missing the frostmod.exe/.dll assets. Grab it here:\n    %s\n",
               tag.c_str(), FROSTMOD_RELEASES_URL);
        return 1;
    }

    const std::string dir    = ExeDir();
    const std::string exeNew = dir + "frostmod.exe.new";
    const std::string dllNew = dir + "frostmod.dll.new";
    printf("[*] downloading frostmod.dll ...\n");
    if (!DownloadToFile(dllUrl, dllNew)) { printf("[!] dll download failed.\n"); return 1; }
    printf("[*] downloading frostmod.exe ...\n");
    if (!DownloadToFile(exeUrl, exeNew)) { printf("[!] exe download failed.\n"); DeleteFileA(dllNew.c_str()); return 1; }

    // Both are downloaded + verified non-empty. Swap: dll first, then the running exe.
    const std::string dll = dir + "frostmod.dll";
    if (!MoveFileExA(dllNew.c_str(), dll.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        printf("[!] couldn't replace frostmod.dll (err %lu). Nothing changed. Manual: %s\n",
               GetLastError(), FROSTMOD_RELEASES_URL);
        DeleteFileA(exeNew.c_str()); DeleteFileA(dllNew.c_str());
        return 1;
    }
    char self[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, self, sizeof(self));
    std::string old = std::string(self) + ".old";
    DeleteFileA(old.c_str());
    if (!MoveFileA(self, old.c_str()) ||
        !MoveFileExA(exeNew.c_str(), self, MOVEFILE_REPLACE_EXISTING)) {
        printf("[!] dll updated but the exe swap failed (err %lu). Get the new exe from:\n    %s\n",
               GetLastError(), FROSTMOD_RELEASES_URL);
        return 1;
    }

    printf("[+] updated to %s. Relaunching FrostMod...\n", tag.c_str());
    ShellExecuteA(nullptr, "open", self, nullptr, dir.c_str(), SW_SHOWNORMAL);
    return 0;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    SetConsoleTitleA("FrostMod");
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    const char* processName = "mxbikes.exe";
    std::string dllPath;
    std::string modsPath;
    long warmupMs = 400;     // delay after seeing the process before injecting;
                             // small = catch the startup scan. Override with --wait.
    bool probeMount  = false; // --probe-mount: hook the pkz-mount fn to log its args
    bool dumpList    = false; // --dump-serverlist: dump the master server-list blob
    bool switchLive  = false; // --switch-live: arm the track switcher's real load (may crash)
    bool filterSrv   = true;  // server-browser filter: ON by default (--no-filter-servers disables)
    bool installStartup   = false; // --install-startup: run automatically at login
    bool uninstallStartup = false; // --uninstall-startup: stop running at login
    bool startupMode      = false; // --startup: launched by the login entry (start minimized)
    bool checkUpdate      = true;  // check GitHub for a newer release on startup
    bool doUpdate         = false; // --update: download + install the latest release

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--process" && i + 1 < argc)    processName = argv[++i];
        else if (a == "--mods" && i + 1 < argc)  modsPath = argv[++i];
        else if (a == "--wait" && i + 1 < argc)  warmupMs = atol(argv[++i]);
        else if (a == "--probe-mount")           probeMount = true;
        else if (a == "--dump-serverlist")       dumpList = true;
        else if (a == "--switch-live")           switchLive = true;
        else if (a == "--filter-servers")        filterSrv = true;    // explicit (already default)
        else if (a == "--no-filter-servers")     filterSrv = false;   // opt out of the filter
        else if (a == "--install-startup")       installStartup = true;
        else if (a == "--uninstall-startup")     uninstallStartup = true;
        else if (a == "--startup")               startupMode = true;
        else if (a == "--no-update-check")       checkUpdate = false;
        else if (a == "--update")                doUpdate = true;
        else                                     dllPath = a;
    }

    // Clean up leftovers from a previous self-update (a running exe can't delete
    // itself, so the freshly-launched copy clears the old one here). Best-effort.
    DeleteFileA((ExeDir() + "frostmod.exe.old").c_str());
    DeleteFileA((ExeDir() + "frostmod.exe.new").c_str());
    DeleteFileA((ExeDir() + "frostmod.dll.new").c_str());

    // --update is a one-shot: download + install the latest release, then exit.
    if (doUpdate) return DoUpdate();

    // --uninstall-startup is a one-shot: remove the login entry and exit.
    if (uninstallStartup) {
        bool ok = UninstallStartup();
        printf(ok ? "[+] FrostMod will no longer start automatically at login.\n"
                  : "[!] Could not remove the startup entry (registry error).\n");
        return ok ? 0 : 1;
    }

    // Launched by the login entry: tuck the console into the taskbar so it isn't in
    // your face every time you log in (it's still there if you want to watch it).
    if (startupMode) {
        if (HWND con = GetConsoleWindow()) ShowWindow(con, SW_MINIMIZE);
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

    printf("============== FrostMod v" FROSTMOD_VERSION " ==============\n");
    printf("[*] build : " __DATE__ " " __TIME__ "  (this exe)\n");
    printf("[*] DLL   : %s\n", dllPath.c_str());
    printf("[*] mods  : %s%s\n", modsPath.empty() ? "<unknown>" : modsPath.c_str(),
           (!modsPath.empty() && !modsExist) ? "  (not found - pass --mods \"...\")" : "");
    printf("[*] log   : %s\n", logPath.c_str());
    printf("=============================================\n");

    // Check GitHub for a newer release (off-thread so it never delays startup).
    // Disable with --no-update-check. Silent if offline.
    if (checkUpdate) {
        if (HANDLE h = CreateThread(nullptr, 0, UpdateCheckThread, nullptr, 0, nullptr)) CloseHandle(h);
    }

    // --probe-mount: leave a flag next to the dll so it hooks the pkz-mount fn and
    // logs its args (experimental RE probe). Remove it otherwise so it's not stale.
    std::string probeFlag = ExeDir() + "frostmod_probe.flag";
    if (probeMount) {
        if (FILE* f = nullptr; fopen_s(&f, probeFlag.c_str(), "w") == 0 && f) fclose(f);
        printf("[*] --probe-mount ON: DLL will hook the pkz-mount fn and log [mount] args.\n");
    } else {
        DeleteFileA(probeFlag.c_str());
    }
    std::string dumpFlag = ExeDir() + "frostmod_dumplist.flag";
    if (dumpList) {
        if (FILE* f = nullptr; fopen_s(&f, dumpFlag.c_str(), "w") == 0 && f) fclose(f);
        printf("[*] --dump-serverlist ON: DLL will dump the master server-list blob ([srvlist]).\n");
    } else {
        DeleteFileA(dumpFlag.c_str());
    }
    std::string switchFlag = ExeDir() + "frostmod_trackswitch.flag";
    if (switchLive) {
        if (FILE* f = nullptr; fopen_s(&f, switchFlag.c_str(), "w") == 0 && f) fclose(f);
        printf("[*] --switch-live ON: track switcher (F8>3) will REALLY load the picked track.\n"
               "    WARNING: only use it from the testing MENU - mid-ride it crashes the game.\n");
    } else {
        DeleteFileA(switchFlag.c_str());
    }
    std::string filterFlag = ExeDir() + "frostmod_filter.flag";
    if (filterSrv) {
        if (FILE* f = nullptr; fopen_s(&f, filterFlag.c_str(), "w") == 0 && f) fclose(f);
        printf("[*] server filter: ON - hides cheat/ad 'ghost' servers from the online browser\n"
               "    (edit frostmod_serverfilter.yaml to tune; pass --no-filter-servers to turn it off).\n");
    } else {
        printf("[*] server filter: OFF (--no-filter-servers).\n");
        DeleteFileA(filterFlag.c_str());
    }

    // --install-startup: register the login entry now, then keep running (so it's
    // active immediately AND every future login). It just runs THIS exe minimized.
    if (installStartup) {
        if (InstallStartup())
            printf("[+] auto-start ENABLED: FrostMod will launch at login and inject into\n"
                   "    MX Bikes whenever it starts - no need to run this manually again.\n"
                   "    (Undo any time with:  frostmod.exe --uninstall-startup)\n");
        else
            printf("[!] auto-start: could not write the login entry (registry error).\n");
    } else {
        printf("[*] auto-start: %s  (enable with:  frostmod.exe --install-startup)\n",
               IsStartupInstalled() ? "ON - runs at login" : "off");
    }

    // Tell the DLL where the mods folder is (it can't reliably know on its own), so
    // the in-game track library (F10) can find mods\tracks + the inactive-tracks store.
    if (!modsPath.empty()) {
        std::string modsInfo = ExeDir() + "frostmod_mods.txt";
        if (FILE* f = nullptr; fopen_s(&f, modsInfo.c_str(), "w") == 0 && f) {
            fputs(modsPath.c_str(), f); fclose(f);
        }
    }

    // cross-process triggers (the DLL watches these named events).
    HANDLE reloadEvent = CreateEventA(nullptr, FALSE /*auto-reset*/, FALSE, "Local\\FrostModReload");
    HANDLE dumpEvent   = CreateEventA(nullptr, FALSE /*auto-reset*/, FALSE, "Local\\FrostModDumpNow");

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
                printf("[*] %s detected (pid=%lu) - injecting EARLY to catch the startup scan...\n",
                       processName, pid);
            }
            // Small warmup so OpenProcess/CreateRemoteThread succeed, but short so
            // we're loaded before the game's one-time mods scan. The DLL then waits
            // for SteamStub to decrypt before hooking, so injecting early is safe.
            // If early injection ever destabilizes the game, use --wait 2000.
            if ((long)(GetTickCount64() - pendingSince) < warmupMs) {
                if (_kbhit()) { int c = _getch(); if (c == 'q' || c == 'Q') break; }
                Sleep(50);
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
                // A freshly-launched process may briefly reject injection - retry
                // quietly for a few seconds before giving up.
                bool ok = false;
                for (int attempt = 0; attempt < 20 && g_running; ++attempt) {
                    if (InjectDll(pid, dllPath, /*quiet=*/attempt < 19)) { ok = true; break; }
                    if (!FindProcess(processName)) break;   // game died during startup
                    Sleep(250);
                }
                if (!ok) {
                    printf("[!] injection failed after retries (elevation? run as admin).\n"
                           "    Will retry when the game is relaunched.\n");
                    injectedPid = pid; pendingPid = 0;      // don't spin on this pid
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
            printf("\nRELOAD: press R here, or F8 in-game (opens the FrostMod menu) -> 1.\n"
                   "  FrostMod re-runs the game's content-load so new tracks/skins register,\n"
                   "  with an on-screen progress bar (no freeze).\n");
            printf("\nIN-GAME: F8 = FrostMod menu (top-left). Press a number for an action.\n");
            printf("\n--- live log ---   [R] reload   [Q]/Ctrl+C quit\n");
            printf("    (with --dump-serverlist, opening the online browser auto-dumps the list)\n");
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

        // keyboard: R = reload, D = dump server list, Q = quit
        if (_kbhit()) {
            int c = _getch();
            if (c == 'r' || c == 'R') {
                if (reloadEvent) { SetEvent(reloadEvent); printf("[you] reload requested (R)\n"); }
            } else if (c == 'd' || c == 'D') {
                if (dumpEvent) { SetEvent(dumpEvent); printf("[you] dump server-list blob (D) - watch for [srvlist]\n"); }
            } else if (c == 'q' || c == 'Q') {
                break;
            }
        }

        Sleep(150);
    }

    if (reloadEvent) CloseHandle(reloadEvent);
    if (dumpEvent)   CloseHandle(dumpEvent);
    return 0;
}
