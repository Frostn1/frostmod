#pragma once

#include <array>
#include <cstddef>

namespace fixesguard {

enum class StandDownReason {
    None,
    FullPluginFile,
    FullPluginMutex,
    FullPluginModule,
};

struct StandDownDecision {
    bool install;
    StandDownReason reason;
    const char* message;
};

constexpr StandDownDecision DecideDoubleLoad(bool frostmod_dlo_exists,
                                              bool frostmod_mutex_exists,
                                              bool frostmod_module_loaded) {
    if (frostmod_dlo_exists)
        return {false, StandDownReason::FullPluginFile,
                "full FrostMod is installed (plugins\\frostmod.dlo); it already has every "
                "guard, so FrostMod Fixes stays off. Remove one of them."};
    if (frostmod_mutex_exists)
        return {false, StandDownReason::FullPluginMutex,
                "full FrostMod is already active in this process; it already has every "
                "guard, so FrostMod Fixes stays off. Remove one of them."};
    if (frostmod_module_loaded)
        return {false, StandDownReason::FullPluginModule,
                "a frostmod.dll/frostmod.dlo module is loaded; it already has every guard, "
                "so FrostMod Fixes stays off. Remove one of them."};
    return {true, StandDownReason::None, ""};
}

enum class Guard {
    Map,
    Terrain,
    Shock,
    Ghs,
    Trainer,
};

struct Overlap {
    Guard guard;
    const char* guard_name;
    const char* mxbfix_key;
};

inline constexpr std::array<Overlap, 5> kOverlaps{{
    {Guard::Map,     "mapguard",     "gate-vertex-pointer"},
    {Guard::Terrain, "terrainguard", "ground-nan-guard"},
    {Guard::Shock,   "shockguard",   "suspension-nan-guard"},
    {Guard::Ghs,     "ghsguard",     "trainer-cleanup-reset"},
    {Guard::Trainer, "trainerguard", "trainer-series-lookup"},
}};

constexpr const Overlap* FindOverlap(Guard guard) {
    for (const auto& overlap : kOverlaps)
        if (overlap.guard == guard) return &overlap;
    return nullptr;
}

// MXBFix treats a missing [fixes] key as enabled. Its presence only suppresses
// our matching guard when that key resolves nonzero.
constexpr bool UseFrostModGuard(Guard guard, bool mxbfix_present, int mxbfix_ini_value) {
    return !FindOverlap(guard) || !mxbfix_present || mxbfix_ini_value == 0;
}

}  // namespace fixesguard
