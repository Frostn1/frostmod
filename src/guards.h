#pragma once

#include <atomic>
#include <cstdint>

struct GameOffsets;
namespace protection { enum class Kind : int; }

namespace guards {

using LogFn = void (*)(const char*, ...);
using ProtectionCatchFn = void (*)(protection::Kind);

struct Host {
    LogFn log = nullptr;
    uintptr_t game_base = 0;
    intptr_t sig_delta = 0;
    const GameOffsets* game = nullptr;
    ProtectionCatchFn protection_catch = nullptr;
};

extern std::atomic<bool> mapguard_on;
extern std::atomic<int> mapguard_test_every;
extern std::atomic<bool> shockguard_on;
extern std::atomic<bool> bikelistguard_on;
extern std::atomic<bool> wakeguard_on;
extern std::atomic<bool> listboxguard_on;
extern std::atomic<bool> ptr64fix_on;
extern std::atomic<bool> riderinfoguard_on;
extern std::atomic<bool> mipmapguard_on;
extern std::atomic<bool> eventcfgguard_on;
extern std::atomic<bool> lookupguard_on;
extern std::atomic<bool> animguard_on;
extern std::atomic<bool> nanrecover_on;

void Configure(const Host& host);
void SetUnloading(bool unloading);

void InstallGhsGuard();
void InstallTerrainGuard();
void InstallTrainerGuard();
void InstallTyreFallback();
void InstallMapGuard();
void InstallShockGuard();
void InstallSessionGuards();
void Tick();

bool NanRecoverInstall(const uint8_t* sim_step);
const uint8_t* ResolveSimStep(intptr_t delta, const char* log_tag);
bool InstallSimStepHook(const uint8_t* sim_step, void* detour, void** original);
bool InstallNanRecoverHook(intptr_t delta);
void NanRecoverCheck(int32_t slot, bool before);
void NanRecoverSetLive(bool live);
bool NanRecoverLive();
unsigned TerrainGuardedCount();

void RevertTyreFallback();
void RevertLookupGuard();
void RevertMapGuard();
void RevertShockGuard();

}  // namespace guards
