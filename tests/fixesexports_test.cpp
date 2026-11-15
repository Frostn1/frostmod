#include <windows.h>

#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    HMODULE module = LoadLibraryExA(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!module) {
        std::fprintf(stderr, "LoadLibraryEx failed for %s: %lu\n", argv[1], GetLastError());
        return 3;
    }
    const char* required[] = {
        "GetModID", "GetModDataVersion", "GetInterfaceVersion", "Startup", "Shutdown",
    };
    for (const char* name : required) {
        if (!GetProcAddress(module, name)) {
            std::fprintf(stderr, "missing export: %s\n", name);
            FreeLibrary(module);
            return 4;
        }
    }
    FreeLibrary(module);
    return 0;
}
