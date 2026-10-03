// coachterrain.h's in-process part: finding the game's height query in this process, and
// asking it. Reads only; see coachterrain.h for why and when.

#include "coachterrain.h"

#include <windows.h>
#include <intrin.h>

#include <cstdio>

namespace coachterrain {
namespace game {

namespace {

using SampleFn = int32_t (*)(const void* track, float* height, float* normal, float x, float z);

/// The module's .text, from its own section headers.
bool TextRange(const uint8_t* base, const uint8_t*& begin, const uint8_t*& end) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
        if (std::memcmp(s->Name, ".text", 6) == 0) {
            begin = base + s->VirtualAddress;
            end   = begin + s->Misc.VirtualSize;
            return true;
        }
    return false;
}

/// The call itself, on its own so __try has nothing to unwind. The game's code is not ours: a
/// heightfield freed under us is a fault here, which turns this off rather than the game.
int CallGuarded(SampleFn fn, const void* track, float x, float z, float* y) {
    float normal[3];
    __try {
        return fn(track, y, normal, x, z) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

}  // namespace

bool Locate(Terrain& t, std::string& why) {
    t = Terrain{};
    // The exe's base out of the PEB (x64: gs:[0x60], ImageBaseAddress at +0x10), not
    // GetModuleHandle: that goes through the loader's locks, and this runs on a game thread.
    const auto* base = *reinterpret_cast<const uint8_t* const*>(__readgsqword(0x60) + 0x10);
    const uint8_t *begin = nullptr, *end = nullptr;
    if (!base || !TextRange(base, begin, end)) {
        why = "can't read the game's sections";
        return false;
    }
    char buf[160];
    const uint8_t* acc = base + mxb::RVA_TRACK_SLOT_ACCESSOR;
    const size_t   acc_len = sizeof(mxb::SIG_TRACK_SLOT_ACCESSOR) - 1;
    if (acc < begin || acc + acc_len > end || !MatchAt(acc, mxb::SIG_TRACK_SLOT_ACCESSOR, mxb::SIG_TRACK_SLOT_ACCESSOR_MASK)) {
        std::snprintf(buf, sizeof(buf), "the track-slot accessor is not at RVA 0x%zx: not the build it was read from",
                      size_t(mxb::RVA_TRACK_SLOT_ACCESSOR));
        why = buf;
        return false;
    }
    const uint8_t* slots = DecodeSlots(acc);
    if (slots != base + mxb::RVA_TRACK_SLOTS) {
        std::snprintf(buf, sizeof(buf), "the track slots decode to RVA 0x%zx, not 0x%zx: not the build it was read from",
                      size_t(slots - base), size_t(mxb::RVA_TRACK_SLOTS));
        why = buf;
        return false;
    }
    const uint8_t* fn     = base + mxb::RVA_TERRAIN_SAMPLE;
    const size_t   fn_len = sizeof(mxb::SIG_TERRAIN_SAMPLE) - 1;
    const Sampler  s      = fn >= begin && fn + fn_len <= end ? CheckSampler(fn) : Sampler::NONE;
    if (s == Sampler::NONE) {
        std::snprintf(buf, sizeof(buf), "the height query is not at RVA 0x%zx: not the build it was read from",
                      size_t(mxb::RVA_TERRAIN_SAMPLE));
        why = buf;
        return false;
    }
    t.ok          = true;
    t.behind_hook = s == Sampler::BEHIND_HOOK;
    t.sampler     = fn;
    t.slots       = slots;
    return true;
}

int Ask(const Terrain& t, const uint8_t* slot, float x, float z, float& y) {
    if (!t.ok || !slot || !std::isfinite(x) || !std::isfinite(z)) return 0;
    const Header h = ReadHeader(slot);
    if (!Plausible(h) || x < h.origin_x || z < h.origin_z || x > h.origin_x + h.size_x || z > h.origin_z + h.size_z)
        return 0;
    return CallGuarded(reinterpret_cast<SampleFn>(const_cast<void*>(t.sampler)), slot, x, z, &y);
}

}  // namespace game
}  // namespace coachterrain
