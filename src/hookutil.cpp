#include "hookutil.h"

#include <windows.h>
#include <cstring>

#include "MinHook.h"
#include "offsets.h"

namespace hookutil {
namespace {

LogFn g_log = nullptr;
const std::atomic<bool>* g_unloading = nullptr;
uintptr_t g_base = 0;
const GameOffsets* g_game = &GAME_MXB;

#define Log(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

uintptr_t ResolveScanner(intptr_t* out_delta) {
    *out_delta = 0;
    uint8_t* expected = reinterpret_cast<uint8_t*>(g_base + g_game->scan_folder);
    const size_t sig_len = std::strlen(mxb::SIG_SCAN_FOLDER_MASK);
    uint8_t *begin = nullptr, *end = nullptr;
    const bool have_range = GetExecRange(g_base, &begin, &end);

    if (!g_game->content_derived()) {
        uint8_t* found = have_range
            ? PatternScan(begin, end, mxb::SIG_SCAN_FOLDER, mxb::SIG_SCAN_FOLDER_MASK) : nullptr;
        if (!found) {
            Log("[sig] %s: no offsets derived for this title and the scanner signature is "
                "not in .text (yet). Content hooks stay off.", g_game->display);
            return 0;
        }
        Log("[sig] %s: scanner found at RVA 0x%zx by signature. This title has no derived "
            "offsets - RECORD THIS RVA (see tasks/kart-racing-pro-port.md).",
            g_game->display, static_cast<size_t>(reinterpret_cast<uintptr_t>(found) - g_base));
        return reinterpret_cast<uintptr_t>(found);
    }

    const bool in_range = have_range && expected >= begin && expected + sig_len <= end;
    if (in_range && MatchAt(expected, mxb::SIG_SCAN_FOLDER, mxb::SIG_SCAN_FOLDER_MASK)) {
        Log("[sig] scanner signature VERIFIED at RVA 0x%zx - offsets.h fits this build.",
            static_cast<size_t>(g_game->scan_folder));
        return reinterpret_cast<uintptr_t>(expected);
    }
    if (!have_range) {
        Log("[sig] WARNING: can't read module sections to validate; using the raw RVA "
            "0x%zx anyway (may be wrong).", static_cast<size_t>(g_game->scan_folder));
        return reinterpret_cast<uintptr_t>(expected);
    }
    Log("[sig] WARNING: bytes at scanner RVA 0x%zx do NOT match the signature - "
        "offsets.h looks stale for this mxbikes build. Scanning .text...",
        static_cast<size_t>(g_game->scan_folder));
    uint8_t* found = PatternScan(begin, end, mxb::SIG_SCAN_FOLDER, mxb::SIG_SCAN_FOLDER_MASK);
    if (!found) {
        Log("[sig] ERROR: scanner signature not found in .text. The reload can't work on "
            "this build until offsets.h/SIG_SCAN_FOLDER are updated. Skipping content hooks.");
        return 0;
    }
    *out_delta = static_cast<intptr_t>(reinterpret_cast<uintptr_t>(found) -
                                       (g_base + g_game->scan_folder));
    Log("[sig] scanner RELOCATED: found at RVA 0x%zx (delta %+lld from offsets.h).",
        static_cast<size_t>(reinterpret_cast<uintptr_t>(found) - g_base),
        static_cast<long long>(*out_delta));
    return reinterpret_cast<uintptr_t>(found);
}

}  // namespace

void SetLog(LogFn log) { g_log = log; }
void SetUnloadingFlag(const std::atomic<bool>* unloading) { g_unloading = unloading; }
void SetGameContext(uintptr_t game_base, const GameOffsets* game) {
    g_base = game_base;
    g_game = game ? game : &GAME_MXB;
}

int SafeReadInt(const int* p) {
    __try { return *p; } __except (EXCEPTION_EXECUTE_HANDLER) { return -0x7FFF; }
}

size_t SafeReadBytes(const char* p, char* out, size_t cap) {
    size_t n = 0;
    __try { while (n < cap) { out[n] = p[n]; ++n; } }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
    return n;
}

bool SafeCopyStr(const void* p, char* out, size_t cap) {
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

bool MatchAt(const uint8_t* p, const char* pat, const char* mask) {
    for (; *mask; ++mask, ++p, ++pat)
        if (*mask == 'x' && static_cast<uint8_t>(*pat) != *p) return false;
    return true;
}

bool GetExecRange(uintptr_t base, uint8_t** begin, uint8_t** end) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    uintptr_t lo = ~uintptr_t{0}, hi = 0;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            const uintptr_t s = base + sec->VirtualAddress;
            const uintptr_t e = s + sec->Misc.VirtualSize;
            if (s < lo) lo = s;
            if (e > hi) hi = e;
        }
    }
    if (hi <= lo) return false;
    *begin = reinterpret_cast<uint8_t*>(lo);
    *end = reinterpret_cast<uint8_t*>(hi);
    return true;
}

uint8_t* PatternScan(uint8_t* begin, uint8_t* end, const char* pat, const char* mask) {
    const size_t len = std::strlen(mask);
    if (!len || static_cast<size_t>(end - begin) < len) return nullptr;
    for (uint8_t* p = begin; p <= end - len; ++p)
        if (MatchAt(p, pat, mask)) return p;
    return nullptr;
}

void* AllocNear(uintptr_t site, size_t size, unsigned long protect) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    constexpr uintptr_t limit = 0x70000000ull;
    for (uintptr_t d = gran; d < limit; d += gran) {
        for (int up = 0; up < 2; ++up) {
            if (!up && site < d) continue;
            const uintptr_t a = (up ? site + d : site - d) & ~(gran - 1);
            if (!a) continue;
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size,
                                       MEM_RESERVE | MEM_COMMIT, protect)) return p;
        }
    }
    return nullptr;
}

bool InstallHook(void* target, void* detour, void** original, const char* name) {
    if (g_unloading && g_unloading->load(std::memory_order_relaxed)) {
        Log("[hook] %s not hooked: the plugin is unloading", name);
        return false;
    }
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

uintptr_t WaitForScanner(intptr_t* out_delta, unsigned long timeout_ms) {
    *out_delta = 0;
    if (!g_game->content_derived()) {
        const DWORD start = GetTickCount();
        bool announced = false;
        for (;;) {
            uint8_t *begin = nullptr, *end = nullptr;
            if (GetExecRange(g_base, &begin, &end)) {
                if (uint8_t* found = PatternScan(begin, end, mxb::SIG_SCAN_FOLDER,
                                                  mxb::SIG_SCAN_FOLDER_MASK)) {
                    Log("[sig] %s: scanner found at RVA 0x%zx by signature (after %lums). "
                        "This title has no derived offsets - RECORD THIS RVA; the [stack] "
                        "lines that follow carry the rest (tasks/kart-racing-pro-port.md).",
                        g_game->display,
                        static_cast<size_t>(reinterpret_cast<uintptr_t>(found) - g_base),
                        static_cast<unsigned long>(GetTickCount() - start));
                    return reinterpret_cast<uintptr_t>(found);
                }
            }
            if (GetTickCount() - start > timeout_ms) break;
            if (!announced) {
                Log("[sig] %s: no derived offsets - sweeping .text for the scanner signature "
                    "(waiting for the code to decrypt)...", g_game->display);
                announced = true;
            }
            Sleep(100);
        }
        Log("[sig] %s: the scanner signature never appeared in .text. Content hooks stay off, "
            "so the capture that would derive this title's offsets has nothing to hook - its "
            "scanner prologue must differ. Nothing else is affected.", g_game->display);
        return 0;
    }

    uint8_t* expected = reinterpret_cast<uint8_t*>(g_base + g_game->scan_folder);
    const size_t sig_len = std::strlen(mxb::SIG_SCAN_FOLDER_MASK);
    const DWORD start = GetTickCount();
    bool announced = false;
    for (;;) {
        uint8_t *begin = nullptr, *end = nullptr;
        const bool have_range = GetExecRange(g_base, &begin, &end);
        const bool in_range = have_range && expected >= begin && expected + sig_len <= end;
        if (in_range && MatchAt(expected, mxb::SIG_SCAN_FOLDER, mxb::SIG_SCAN_FOLDER_MASK)) {
            if (announced)
                Log("[sig] scanner code decrypted after %lums; hooking now (before the scan, we hope).",
                    static_cast<unsigned long>(GetTickCount() - start));
            else
                Log("[sig] scanner signature VERIFIED at RVA 0x%zx - offsets.h fits this build.",
                    static_cast<size_t>(g_game->scan_folder));
            return reinterpret_cast<uintptr_t>(expected);
        }
        if (GetTickCount() - start > timeout_ms) break;
        if (!announced) {
            Log("[sig] game code not decrypted yet (SteamStub); waiting to install content hooks...");
            announced = true;
        }
        Sleep(2);
    }
    return ResolveScanner(out_delta);
}

#undef Log
}  // namespace hookutil
