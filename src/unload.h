// Teardown order for when the game unloads the plugin.
//
// The game calls Shutdown() at exit and then FreeLibrary()s frostmod.dlo. Anything still
// pointing into the dll after that - a MinHook jump in a game or opengl32/ws2_32 function, a
// byte patch that jumps to a stub that calls us, a thread sleeping in our code, the unhandled
// exception filter or the vectored handler - is an access violation in a module that is no
// longer mapped. That is the 2026-10-08 quit crash: two faults inside an unloaded frostmod.dlo
// after a normal exit.
//
// The order matters and is fixed here, not left to the order the steps were registered in:
//   1. Threads   - stop and join ours first, so nothing of ours runs while hooks come out.
//   2. Hooks     - disable every MinHook hook (original bytes back at each target).
//   3. Patches   - put back the instruction bytes we rewrote by hand.
//   4. Handlers  - the exception filter and the vectored handler go last, so a fault during
//                  steps 1-3 is still reported.
//
// Runs once: Shutdown() and DllMain's detach can both get here. Pure, so it is tested on
// any host (tests/unload_test.cpp).
#pragma once

#include <atomic>
#include <cstddef>

namespace unload {

enum class Stage : int { Threads = 0, Hooks, Patches, Handlers, Count };

inline const char* StageName(Stage s) {
    switch (s) {
    case Stage::Threads:  return "threads";
    case Stage::Hooks:    return "hooks";
    case Stage::Patches:  return "patches";
    case Stage::Handlers: return "handlers";
    default:              return "?";
    }
}

// `canBlock` is false when we are under the loader lock (DllMain): waiting for a thread to
// exit there can deadlock, so a step only signals then.
using StepFn = void (*)(bool canBlock);

class Teardown {
public:
    static constexpr int kMaxSteps = 16;

    // False when full; the step is not kept.
    bool Add(Stage stage, const char* name, StepFn fn) {
        if (n_ >= kMaxSteps || !fn || stage < Stage::Threads || stage >= Stage::Count) return false;
        steps_[n_++] = {stage, name, fn};
        return true;
    }

    // Every step, stage by stage, registration order within a stage. Returns how many ran;
    // 0 on every call after the first. `onStep` (may be null) is told each step before it runs.
    int Run(bool canBlock, void (*onStep)(Stage, const char*) = nullptr) {
        if (done_.exchange(true)) return 0;
        int ran = 0;
        for (int s = 0; s < (int)Stage::Count; ++s)
            for (int i = 0; i < n_; ++i) {
                if ((int)steps_[i].stage != s) continue;
                if (onStep) onStep(steps_[i].stage, steps_[i].name);
                steps_[i].fn(canBlock);
                ++ran;
            }
        return ran;
    }

    bool Done() const { return done_.load(); }
    int  Size() const { return n_; }

private:
    struct Step { Stage stage; const char* name; StepFn fn; };
    Step steps_[kMaxSteps] = {};
    int  n_ = 0;
    std::atomic<bool> done_{false};
};

// DLL_PROCESS_DETACH's lpReserved: null is FreeLibrary (we are being unmapped and the process
// goes on, so tear down); non-null is process exit (every other thread is already gone and
// the memory goes with the process - touching hooks there only risks the exit).
inline bool DetachIsUnload(const void* reserved) { return reserved == nullptr; }

}  // namespace unload
