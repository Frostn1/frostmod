// Windows only: every callback the game looks for is really exported from the built
// mxbcoach.dlo, findable by its undecorated name.
//
// A missing or mangled export is invisible from the outside. The game resolves what it can,
// calls what it finds, and says nothing about the rest — so a plugin that records laps
// perfectly well can have one whole feature that is never called, and it looks identical to a
// feature that runs and decides to do nothing. `Draw` and `DrawInit` are why this test exists:
// without them nothing is ever drawn, and no log line inside them would ever be written to say
// so. The rest of the set is asserted because it costs nothing.
//
// x64 has no name decoration for `extern "C"` functions, so these are the exact names PiBoSo's
// loader asks for (mxb_api.h).

#include <windows.h>

#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: coachexports_test <mxbcoach.dll>\n");
        return 2;
    }
    // A real load, not LOAD_LIBRARY_AS_DATAFILE: GetProcAddress is the question, and it is also
    // what the game does. mxbcoach has no DllMain, so nothing runs.
    HMODULE mod = LoadLibraryA(argv[1]);
    if (!mod) {
        std::printf("FAIL: can't load %s (%lu)\n", argv[1], GetLastError());
        return 1;
    }

    // Every export mxbcoach.cpp declares, in the order mxb_api.h lists them.
    static const char* const kExports[] = {
        "GetModID", "GetModDataVersion", "GetInterfaceVersion",
        "Startup", "Shutdown",
        "EventInit", "EventDeinit",
        "RunInit", "RunDeinit", "RunStart", "RunStop", "RunLap", "RunSplit", "RunTelemetry",
        "DrawInit", "Draw",
        "TrackCenterline",
        "RaceEvent", "RaceDeinit", "RaceAddEntry", "RaceRemoveEntry",
        "RaceTrackPosition", "RaceLap", "RaceSplit",
    };

    int failures = 0;
    for (const char* name : kExports) {
        if (GetProcAddress(mod, name) == nullptr) {
            std::printf("FAIL: %s is not exported\n", name);
            ++failures;
        }
    }

    // Said out loud, because these two are the ones whose absence has no other symptom.
    if (!failures) {
        std::printf("coachexports: %d exports present, Draw and DrawInit among them\n",
                    int(sizeof(kExports) / sizeof(kExports[0])));
    }
    FreeLibrary(mod);
    return failures ? 1 : 0;
}
