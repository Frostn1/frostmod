#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

struct GameOffsets;

namespace hookutil {

using LogFn = void (*)(const char*, ...);

void SetLog(LogFn log);
void SetUnloadingFlag(const std::atomic<bool>* unloading);
void SetGameContext(uintptr_t game_base, const GameOffsets* game);

int SafeReadInt(const int* p);
size_t SafeReadBytes(const char* p, char* out, size_t cap);
bool SafeCopyStr(const void* p, char* out, size_t cap);
bool MatchAt(const uint8_t* p, const char* pat, const char* mask);
bool GetExecRange(uintptr_t base, uint8_t** begin, uint8_t** end);
uint8_t* PatternScan(uint8_t* begin, uint8_t* end, const char* pat, const char* mask);
void* AllocNear(uintptr_t site, size_t size, unsigned long protect);
bool InstallHook(void* target, void* detour, void** original, const char* name);

// Waits for SteamStub to expose the scanner signature and reports the build-wide
// fixed-RVA delta. The game context and logger come from the owning plugin.
uintptr_t WaitForScanner(intptr_t* out_delta, unsigned long timeout_ms);

}  // namespace hookutil
