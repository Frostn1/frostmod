// ============================================================================
//  FrostMod injector - loads frostmod.dll into a running mxbikes.exe.
//
//  Usage:
//     injector.exe                       (finds mxbikes.exe, loads .\frostmod.dll)
//     injector.exe C:\path\frostmod.dll  (explicit DLL path)
//     injector.exe --process gpbikes.exe C:\path\frostmod.dll
//
//  Classic CreateRemoteThread + LoadLibraryA technique. Run as the same user
//  as the game (and elevated if the game is elevated).
// ============================================================================

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>

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

int main(int argc, char** argv) {
    const char* processName = "mxbikes.exe";
    std::string dllPath;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--process" && i + 1 < argc) processName = argv[++i];
        else dllPath = a;
    }

    // default DLL path: frostmod.dll next to the injector
    if (dllPath.empty()) {
        char exeDir[MAX_PATH];
        GetModuleFileNameA(nullptr, exeDir, sizeof(exeDir));
        if (char* slash = strrchr(exeDir, '\\')) *(slash + 1) = 0;
        dllPath = std::string(exeDir) + "frostmod.dll";
    }

    // make it absolute (LoadLibrary in the target runs from the game's cwd)
    char full[MAX_PATH];
    if (GetFullPathNameA(dllPath.c_str(), sizeof(full), full, nullptr))
        dllPath = full;

    if (GetFileAttributesA(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        printf("[!] DLL not found: %s\n", dllPath.c_str());
        return 1;
    }

    DWORD pid = FindProcess(processName);
    if (!pid) {
        printf("[!] %s is not running.\n", processName);
        return 1;
    }
    printf("[*] %s pid=%lu\n", processName, pid);
    printf("[*] injecting %s\n", dllPath.c_str());

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                              PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                              FALSE, pid);
    if (!proc) { printf("[!] OpenProcess failed (%lu). Run elevated?\n", GetLastError()); return 1; }

    // write the DLL path into the target
    SIZE_T len = dllPath.size() + 1;
    void* remote = VirtualAllocEx(proc, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { printf("[!] VirtualAllocEx failed (%lu)\n", GetLastError()); CloseHandle(proc); return 1; }
    if (!WriteProcessMemory(proc, remote, dllPath.c_str(), len, nullptr)) {
        printf("[!] WriteProcessMemory failed (%lu)\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE); CloseHandle(proc); return 1;
    }

    auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!thread) { printf("[!] CreateRemoteThread failed (%lu)\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE); CloseHandle(proc); return 1; }

    WaitForSingleObject(thread, 5000);
    DWORD loaded = 0; GetExitCodeThread(thread, &loaded);
    printf(loaded ? "[+] injected (module handle 0x%lX in target).\n"
                  : "[!] remote LoadLibrary returned 0 - DLL failed to load.\n", loaded);

    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(thread);
    CloseHandle(proc);
    return loaded ? 0 : 1;
}
