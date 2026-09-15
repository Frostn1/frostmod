// Windows only: the built mxbcoach.dlo carries every voice clip, where the plugin looks for it
// (RCDATA 100 + cue kind), in the format it plays. Loads the DLL as data, so nothing runs.

#include <windows.h>

#include <cstdio>
#include <vector>

#include "../src/coachvoice.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: coachvoice_res_test <mxbcoach.dll>\n");
        return 2;
    }
    HMODULE mod = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!mod) {
        std::printf("FAIL: can't load %s (%lu)\n", argv[1], GetLastError());
        return 1;
    }
    int failures = 0;
    for (int kind = coachcue::BRAKE; kind <= coachcue::SIT; ++kind) {
        HRSRC res = FindResourceA(mod, MAKEINTRESOURCEA(coachvoice::kResourceBase + kind), MAKEINTRESOURCEA(10));
        HGLOBAL data = res ? LoadResource(mod, res) : nullptr;
        const void* p = data ? LockResource(data) : nullptr;
        std::vector<int16_t> pcm;
        const char* name = coachvoice::kClips[coachvoice::ClipFor(uint8_t(kind))].name;
        if (!p || !coachvoice::ParseWav(static_cast<const uint8_t*>(p), SizeofResource(mod, res), pcm)) {
            std::printf("FAIL: clip for kind %d (%s) missing or unreadable\n", kind, name);
            ++failures;
        } else {
            std::printf("kind %2d %-10s %zu samples\n", kind, name, pcm.size());
        }
    }
    FreeLibrary(mod);
    if (failures) return 1;
    std::printf("coachvoice_res: all clips embedded\n");
    return 0;
}
