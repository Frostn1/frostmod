// MXB Coach recorder: how long the game waits in RunTelemetry, measured against a built .dlo.
//
// Not a test: `telemetry_bench <mxbcoach.dlo> <scratch dir>` loads the plugin, starts a practice
// stint in <scratch dir> (so the recorder writes and flushes its session file), and calls
// RunTelemetry 3000 times at 50 Hz pace-free (back to back, with a 2 ms gap every 10th so a worker
// gets to run), printing p50 / p99 / max of the callback. Run it on two builds to compare them.

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using StartupFn   = int (*)(char*);
using VoidFn      = void (*)();
using DataFn      = void (*)(void*, int);
using TelemetryFn = void (*)(void*, int, float, float);

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: telemetry_bench <mxbcoach.dlo> <scratch dir>\n");
        return 2;
    }
    HMODULE mod = LoadLibraryA(argv[1]);
    if (!mod) {
        std::printf("cannot load %s\n", argv[1]);
        return 1;
    }
    auto startup   = reinterpret_cast<StartupFn>(GetProcAddress(mod, "Startup"));
    auto shutdown  = reinterpret_cast<VoidFn>(GetProcAddress(mod, "Shutdown"));
    auto event     = reinterpret_cast<DataFn>(GetProcAddress(mod, "EventInit"));
    auto run_init  = reinterpret_cast<DataFn>(GetProcAddress(mod, "RunInit"));
    auto run_start = reinterpret_cast<VoidFn>(GetProcAddress(mod, "RunStart"));
    auto run_deinit = reinterpret_cast<VoidFn>(GetProcAddress(mod, "RunDeinit"));
    auto telemetry = reinterpret_cast<TelemetryFn>(GetProcAddress(mod, "RunTelemetry"));
    if (!startup || !shutdown || !event || !run_init || !run_start || !run_deinit || !telemetry) {
        std::printf("missing exports\n");
        return 1;
    }
    std::string dir = argv[2];
    if (dir.back() != '\\') dir += '\\';
    CreateDirectoryA(dir.c_str(), nullptr);
    startup(dir.data());
    std::vector<uint8_t> ev(512, 0), ri(512, 0), sample(512, 0);
    ev[0] = 1;  // testing: practice from the start
    std::strcpy(reinterpret_cast<char*>(ev.data()) + 4, "bench");
    event(ev.data(), int(ev.size()));
    run_init(ri.data(), int(ri.size()));
    run_start();

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    std::vector<double> us;
    for (int i = 0; i < 3000; ++i) {
        float* fl = reinterpret_cast<float*>(sample.data());
        fl[5]     = 20.0f;                   // speed
        fl[6]     = float(i) * 0.4f;         // x
        fl[8]     = float(i % 100) * 0.1f;   // z
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        telemetry(sample.data(), int(sample.size()), float(i) * 0.02f, float(i % 1000) / 1000.0f);
        QueryPerformanceCounter(&b);
        us.push_back(double(b.QuadPart - a.QuadPart) * 1e6 / double(f.QuadPart));
        if (i % 10 == 9) Sleep(2);
    }
    run_deinit();
    shutdown();
    std::sort(us.begin(), us.end());
    std::printf("RunTelemetry over %zu calls: p50 %.1f us  p99 %.1f us  max %.1f us\n", us.size(), us[us.size() / 2],
                us[us.size() * 99 / 100], us.back());
    return 0;
}
