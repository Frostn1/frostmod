#include "guards.h"

#include <windows.h>
#include <GL/gl.h>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "offsets.h"
#include "trainer.h"
#include "tyrefallback.h"
#include "mapguard.h"
#include "shockguard.h"
#include "sessionguards.h"
#include "nanrecover.h"
#include "tyrelog.h"
#include "crashreport.h"
#include "protection.h"
#include "hookutil.h"
#include "lookupguard.h"
#include "setupguard.h"

namespace guards {

using hookutil::AllocNear;
using hookutil::GetExecRange;
using hookutil::InstallHook;
using hookutil::MatchAt;
using hookutil::PatternScan;
using hookutil::SafeReadBytes;
using hookutil::SafeReadInt;
using hookutil::SafeCopyStr;

std::atomic<bool> mapguard_on{true};
std::atomic<int> mapguard_test_every{0};
std::atomic<bool> shockguard_on{true};
std::atomic<bool> bikelistguard_on{true};
std::atomic<bool> wakeguard_on{true};
std::atomic<bool> listboxguard_on{true};
std::atomic<bool> ptr64fix_on{true};
std::atomic<bool> riderinfoguard_on{true};
std::atomic<bool> setupguard_on{true};
std::atomic<bool> mipmapguard_on{true};
std::atomic<bool> eventcfgguard_on{true};
std::atomic<bool> lookupguard_on{true};
std::atomic<bool> animguard_on{true};
std::atomic<bool> nanrecover_on{true};

namespace {

Host g_host{};
uintptr_t g_base = 0;
intptr_t g_sigDelta = 0;
const GameOffsets* g_game = &GAME_MXB;
std::atomic<bool> g_unloading{false};
std::atomic<bool> g_nrLive{false};

#define Log(...) do { if (g_host.log) g_host.log(__VA_ARGS__); } while (0)

static void ProtCatch(protection::Kind kind) {
    if (g_host.protection_catch) g_host.protection_catch(kind);
}

// ===========================================================================
// GHS CLOSE GUARD  -  the one crash-to-desktop we can stop from in here
//
// A player's own crash line pinned it: closing an already-closed GHS handle is a
// null deref in mxbikes.exe and the game goes to desktop. offsets.h carries the
// derivation and, more to the point, the evidence that it is an oversight - the
// sibling function one along guards the identical read and returns instead.
//
// So this does what the sibling does. A handle naming a slot that holds nothing
// returns 1, the answer the function already gives for a handle out of range;
// anything else goes straight to the game's own code, untouched. A working close
// is unaffected and there is no state of ours to get wrong.
//
// Two conditions before a byte is written: the 27-byte signature must match, and
// the table decoded from the function's own `lea` must land inside the module. If
// either fails the guard stays off and says so - a detour at a wrong address is a
// jmp spliced into whatever happens to live there, which is how you turn one
// crash into a worse one.
// ===========================================================================
using GhsCloseFn = int (*)(unsigned);
static GhsCloseFn g_origGhsClose = nullptr;
static void**     g_ghsTable = nullptr;         // decoded from the lea, never from an RVA
static std::atomic<unsigned> g_ghsGuarded{0};   // crashes prevented, for the log

// SEH needs its own frame, and this one has no C++ objects in it on purpose.
static bool GhsSlotEmpty(void** table, unsigned idx) {
    __try { return table[idx] == nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static int hkGhsClose(unsigned handle) {
    const unsigned idx = handle - 1;            // the function's own `dec ecx`
    if (g_ghsTable && idx < (unsigned)mxb::GHS_SLOTS && GhsSlotEmpty(g_ghsTable, idx)) {
        const unsigned n = g_ghsGuarded.fetch_add(1, std::memory_order_relaxed) + 1;
        // The first few only: if something closes handles in a loop, this must not
        // become the thing that fills the disk.
        if (n <= 5)
            Log("[ghs] guarded a close of handle %u whose slot is already empty - this is "
                "the crash at mxbikes.exe+0x11D753. Prevented %u so far.", handle, n);
        frostmod::crash::Note("guarded a double close of GHS handle %u", handle);
        ProtCatch(protection::Kind::TrainerClose);
        return 1;   // the game's own answer for a handle that names no slot
    }
    return g_origGhsClose(handle);
}

// ===========================================================================
// TERRAIN SAMPLE GUARD  -  the crash that ruins races
//
// Reported from outside as about a fifth of all crashes, and the one that takes a
// race down with it: `mxbikes.exe+0x1F1923`, hit when a rider goes into a fence.
//
// offsets.h has the derivation. The short of it: that address is a corner read in
// a bilinear sample of the track's height grid, and the four bounds checks above
// it are `comiss` + `ja`, which a NaN passes - an unordered compare does not take
// `ja`. `cvttss2si` then turns the NaN into INT_MIN and the index walks gigabytes
// off the grid.
//
// So the fault is not the heightmap. It is a position that stopped being a number
// somewhere upstream, and this is just where it gets used. We cannot fix where the
// NaN comes from with a hook. We can decline to look up terrain for a query that is
// not a position, which is what the function already does for a position outside
// the map and for a grid that is not loaded: return 0, write nothing.
//
// Every catch is logged with the values, because that is the only way anyone learns
// whether the NaN theory is right and what the query looked like. If the first
// reports back show finite coordinates being refused, the theory is wrong and this
// comes out again.
// ===========================================================================
using TerrainSampleFn = int32_t (*)(void*, void*, float*, float, float);
static TerrainSampleFn g_origTerrain = nullptr;
static std::atomic<unsigned> g_terrainGuarded{0};

static int32_t hkTerrainSample(void* obj, void* a2, float* out, float x, float y) {
    // isfinite, not isnan: an infinity survives the same comparisons and lands on the
    // same cvttss2si.
    if (!std::isfinite(x) || !std::isfinite(y)) {
        const unsigned n = g_terrainGuarded.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 10)
            Log("[terrain] refused a height query at (%f, %f) - not a position. This is the "
                "crash at mxbikes.exe+0x%zx. Refused %u so far.",
                (double)x, (double)y, (size_t)mxb::RVA_TERRAIN_FAULT, n);
        // The one thing a crash dump of this fault can never contain.
        //
        // By the time the game faults, the position has been a NaN for a while and the
        // code that made it has long since returned. Here it has not: we are standing in
        // the call that was about to go wrong, with every frame above it still on the
        // stack. So the first few catches record who asked, which is the only way the
        // thing upstream - whatever produces the NaN - gets a name rather than a theory.
        //
        // First three only. This runs on the physics thread and the answer does not get
        // truer the fourth time.
        if (n <= 3) {
            char frames[12][160];
            const int got = frostmod::crash::CaptureStack(frames, 12);
            Log("[terrain] who asked (nearest first, %d frame(s)):", got);
            for (int i = 0; i < got; ++i) Log("[terrain]   #%-2d %s", i, frames[i]);
        }
        frostmod::crash::Note("refused a NaN height query (%u so far)", n);
        ProtCatch(protection::Kind::PhysicsNan);
        return 0;   // what the function itself returns for a query it cannot serve
    }
    return g_origTerrain(obj, a2, out, x, y);
}

// ===========================================================================
// The trainer record: the biggest crash in the game
//
// 39% of all MX Bikes crash reports in the largest public sample - a read AV in the CRT at
// track load, before the rider is even on track. The full chain is in offsets.h above
// RVA_GHS_LOAD; the short version is that the saver which runs at session end never writes the
// series and tyre names, its record is an uncleared stack local, and the loader then hands that
// stack rubbish to sprintf as a `%s`.
//
// We cannot patch the game's two savers into agreement. We can make sure a record never reaches
// that sprintf with something in it that is not a string - on the way in, which fixes every
// damaged trainer already on disk, and on the way out, so no new one is written.
//
// Both hooks are pure buffer touch-ups: no globals, no locking, nothing that can throw. The
// file layer is not re-entrant on a slot as it stands, and this does not make it worse.
// ===========================================================================
using GhsLoadFn = int32_t (*)(int32_t, const char*, const char*, int32_t, void*, int32_t);
using GhsSaveFn = int32_t (*)(int32_t, const char*, const char*, int32_t, const void*, int32_t);
static GhsLoadFn g_origGhsLoad = nullptr;
static GhsSaveFn g_origGhsSave = nullptr;
static std::atomic<unsigned> g_trainerFixedIn{0};
static std::atomic<unsigned> g_trainerFixedOut{0};

/// Whether this is the trainer record we know how to check, rather than some other GHS file.
static bool IsTrainerRecord(const char* tag, int32_t ver, int32_t size) {
    return ver == trainer::kVersion && size == (int32_t)trainer::kRecordSize && tag &&
           _stricmp(tag, "mxbikes") == 0;
}

static int32_t hkGhsLoad(int32_t pool, const char* path, const char* tag, int32_t ver, void* dest,
                         int32_t size) {
    const int32_t rc = g_origGhsLoad(pool, path, tag, ver, dest, size);
    // Only a record that loaded, and only the one we know. The legacy 0x53C layout reaches the
    // same reader and the game zeroes these fields itself on that path.
    if (rc == 0 && IsTrainerRecord(tag, ver, size)) {
        const int fixed = trainer::Sanitise(dest, (size_t)size);
        if (fixed > 0) {
            const unsigned n = g_trainerFixedIn.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 10)
                Log("[trainer] cleaned %d field(s) out of a trainer as it loaded: %s. Left as "
                    "written it goes to sprintf as a string and takes the game with it "
                    "(msvcr90+0x36EDE). Cleaned %u so far.",
                    fixed, path ? path : "(no path)", n);
            frostmod::crash::Note("cleaned a trainer record on load (%u so far)", n);
            ProtCatch(protection::Kind::BadText);
        }
    }
    return rc;
}

static int32_t hkGhsSave(int32_t pool, const char* path, const char* tag, int32_t ver,
                         const void* buf, int32_t size) {
    if (buf && IsTrainerRecord(tag, ver, size)) {
        // The record is the caller's stack temp and is dead after this returns, but it is not
        // ours: corrected on a copy, so nothing the game does afterwards sees a changed buffer.
        uint8_t copy[trainer::kRecordSize];
        memcpy(copy, buf, sizeof(copy));
        const int fixed = trainer::Sanitise(copy, sizeof(copy));
        if (fixed > 0) {
            const unsigned n = g_trainerFixedOut.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 10)
                Log("[trainer] cleaned %d field(s) out of a trainer before it was written: %s. "
                    "The saver that runs at session end never fills them, so what goes to disk "
                    "is whatever was on the stack. Cleaned %u so far.",
                    fixed, path ? path : "(no path)", n);
            return g_origGhsSave(pool, path, tag, ver, copy, size);
        }
    }
    return g_origGhsSave(pool, path, tag, ver, buf, size);
}

/// Place one of the two GHS hooks, by signature, refusing rather than guessing.
static bool PlaceGhsHook(intptr_t delta, uintptr_t rva, const char* sig, const char* mask,
                         void* detour, void** orig, const char* name) {
    uint8_t *begin, *end;
    if (!GetExecRange(g_base, &begin, &end)) return false;
    uint8_t* fn = (uint8_t*)(g_base + rva + delta);
    if (fn < begin || fn >= end || !MatchAt(fn, sig, mask)) {
        uint8_t* found = PatternScan(begin, end, sig, mask);
        if (!found) {
            Log("[trainer] %s not found by signature in this build's .text. If the game "
                "updated, re-derive it before trusting the RVA.", name);
            return false;
        }
        Log("[trainer] %s RELOCATED: found at RVA 0x%zx (offsets.h says 0x%zx).", name,
            (size_t)(found - g_base), (size_t)rva);
        fn = found;
    }
    // Somebody else got here first. Two detours on one function is how you make this worse.
    if (mxb::LooksDetoured(fn)) {
        Log("[trainer] %s is already hooked (%02X %02X %02X %02X %02X). Leaving it alone.", name,
            fn[0], fn[1], fn[2], fn[3], fn[4]);
        return false;
    }
    return InstallHook(fn, detour, orig, name);
}

static void InstallTrainerGuard(intptr_t delta) {
    if (!g_game->offsets_complete) {
        Log("[trainer] guard off for %s - the trainer record is MX Bikes' and has no twin here.",
            g_game->display);
        return;
    }
    const bool in = PlaceGhsHook(delta, mxb::RVA_GHS_LOAD, mxb::SIG_GHS_LOAD, mxb::SIG_GHS_LOAD_MASK,
                                 (void*)&hkGhsLoad, (void**)&g_origGhsLoad, "ghsLoad(0x11dab0)");
    const bool out = PlaceGhsHook(delta, mxb::RVA_GHS_SAVE, mxb::SIG_GHS_SAVE, mxb::SIG_GHS_SAVE_MASK,
                                  (void*)&hkGhsSave, (void**)&g_origGhsSave, "ghsSave(0x11de10)");
    if (in)
        Log("[trainer] load guard live. A trainer already damaged on disk now loads instead of "
            "taking the game down.");
    if (out)
        Log("[trainer] save guard live. No new trainer is written with the stack in it.");
}

// ===========================================================================
// TYRE FALLBACK  -  a bike naming a tyre that is not loaded crashes the bike list
//
// tyrefallback.h has the chain. The three "tyre not found" exits of the bike-list build load
// the tyre index from an uninitialised stack slot; the junk index then reaches sprintf as a
// string pointer (msvcr90+0x36EDE). An empty mods\tyres\p_mx folder (or one holding only a
// desktop.ini) is enough: it shadows the stock p_mx and every bike that uses p_mx crashes the
// menu. Each exit now jumps to a stub that hands back the first loaded tyre instead.
// ===========================================================================
static std::atomic<unsigned> g_tyreFallbacks{0};
static uintptr_t g_tyreSpan = 0;   // VA of the patched span, 0 = not patched

// Put the game's own bytes back. Called from Shutdown(): the game unloads the plugin at exit,
// and a jump into a stub that calls into an unloaded dll would be a crash of our own making.
static void TrainerTyreRevert();
static void TyreFallbackRevert() {
    TrainerTyreRevert();
    if (!g_tyreSpan) return;
    DWORD prot = 0;
    if (!VirtualProtect((void*)g_tyreSpan, tyrefallback::kSpanLen, PAGE_EXECUTE_READWRITE, &prot))
        return;
    memcpy((void*)g_tyreSpan, tyrefallback::kSpanBytes, tyrefallback::kSpanLen);
    VirtualProtect((void*)g_tyreSpan, tyrefallback::kSpanLen, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), (void*)g_tyreSpan, tyrefallback::kSpanLen);
    g_tyreSpan = 0;
}

// Called from the stub on the game thread, with the game's frame. Returns the index the game
// continues with. No C++ objects with destructors here: SafeCopyStr does the SEH.
static int TyreFallbackPick(int site, const uint8_t* gameRsp, int bikeIndex) {
    using namespace tyrefallback;
    const int gameValue = SafeReadInt((const int*)(gameRsp + kJunkSlot));
    const int count     = SafeReadInt((const int*)(g_base + kRvaTyreCount));
    uint8_t* table = nullptr;
    SafeReadBytes((const char*)(g_base + kRvaTyreTable), (char*)&table, sizeof(table));
    const int pick = table ? PickIndex(count, gameValue) : gameValue;

    const unsigned n = g_tyreFallbacks.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 20) return pick;   // a whole bike list can miss; the first lines say enough

    char bike[64] = "?", want[64] = "?", used[64] = "?";
    const int bikes = SafeReadInt((const int*)(g_base + kRvaBikeCount));
    uint8_t* btab = nullptr;
    SafeReadBytes((const char*)(g_base + kRvaBikeTable), (char*)&btab, sizeof(btab));
    if (btab && bikeIndex >= 0 && bikeIndex < bikes)
        SafeCopyStr(btab + (size_t)bikeIndex * kBikeStride, bike, sizeof(bike));
    if (site == kCfgTyreMissing) SafeCopyStr(gameRsp + kCfgTyreSlot, want, sizeof(want));
    else if (site == kPickedTyreMissing) SafeCopyStr((const void*)(g_base + kRvaPickedTyre), want, sizeof(want));

    if (count <= 0 || !table) {
        Log("[tyres] bike %s: no tyres are loaded at all, so there is no tyre to fall back on "
            "(game's own value kept).", bike);
        return pick;
    }
    SafeCopyStr(table + (size_t)pick * kTyreStride, used, sizeof(used));
    if (site == kNoTyres)
        Log("[tyres] bike %s: tyre list is empty; using '%s'.", bike, used);
    else
        Log("[tyres] bike %s asked for tyre '%s' which isn't loaded; using '%s'.", bike, want, used);
    frostmod::crash::Note("tyre fallback for bike %s (%u so far)", bike, n);
    ProtCatch(protection::Kind::BadText);
    return pick;
}

// ---- the same three exits in the trainer loader (tyrefallback.h, namespace trainer) --------
static std::atomic<unsigned> g_trnTyreFallbacks{0};
static uintptr_t g_trnTyreSpan = 0;   // VA of the patched span, 0 = not patched

static void TrainerTyreRevert() {
    if (!g_trnTyreSpan) return;
    using namespace tyrefallback;
    DWORD prot = 0;
    if (!VirtualProtect((void*)g_trnTyreSpan, trainerload::kSpanLen, PAGE_EXECUTE_READWRITE, &prot))
        return;
    memcpy((void*)g_trnTyreSpan, trainerload::kSpanBytes, trainerload::kSpanLen);
    VirtualProtect((void*)g_trnTyreSpan, trainerload::kSpanLen, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), (void*)g_trnTyreSpan, trainerload::kSpanLen);
    g_trnTyreSpan = 0;
}

// Same contract as TyreFallbackPick; the third argument is the loader's r15 (the trainer).
static int TrainerTyrePick(int site, const uint8_t* gameRsp, const uint8_t* rec) {
    using namespace tyrefallback;
    const int gameValue = SafeReadInt((const int*)(gameRsp + kJunkSlot));
    const int count     = SafeReadInt((const int*)(g_base + kRvaTyreCount));
    uint8_t* table = nullptr;
    SafeReadBytes((const char*)(g_base + kRvaTyreTable), (char*)&table, sizeof(table));
    const int pick = table ? PickIndex(count, gameValue) : gameValue;

    const unsigned n = g_trnTyreFallbacks.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 20) return pick;

    char bike[64] = "?", want[64] = "?", used[64] = "?";
    if (rec) SafeCopyStr(rec + trainerload::kRecBike, bike, sizeof(bike));
    if (site == kCfgTyreMissing) SafeCopyStr(gameRsp + trainerload::kCfgTyreSlot, want, sizeof(want));
    else if (site == kPickedTyreMissing && rec) SafeCopyStr(rec + trainerload::kRecTyre, want, sizeof(want));

    if (count <= 0 || !table) {
        Log("[tyres] trainer for %s: no tyres are loaded at all, so there is no tyre to fall "
            "back on (game's own value kept).", bike);
        return pick;
    }
    SafeCopyStr(table + (size_t)pick * kTyreStride, used, sizeof(used));
    Log("[tyres] trainer for %s names tyre '%s', which isn't loaded; using '%s' instead of "
        "crashing at msvcr90+0x36EDE (from mxbikes.exe+0x%llX). %u so far.", bike, want, used,
        (unsigned long long)trainerload::kRvaCall, n);
    frostmod::crash::Note("trainer tyre fallback for %s (%u so far)", bike, n);
    ProtCatch(protection::Kind::BadText);
    return pick;
}

static void InstallTrainerTyreFallback() {
    using namespace tyrefallback;
    if (g_trnTyreSpan) return;
    const uintptr_t span = g_base + trainerload::kRvaSpan;
    uint8_t *tb, *te;
    if (!GetExecRange(g_base, &tb, &te) || (uint8_t*)span < tb ||
        (uint8_t*)span + trainerload::kSpanLen > te || !trainerload::SpanMatches((const uint8_t*)span)) {
        Log("[tyres] trainer fallback not applied: the bytes at +0x%llX are not the trainer's "
            "tyre lookup we expect (game updated?). Nothing patched.",
            (unsigned long long)trainerload::kRvaSpan);
        return;
    }
    uint8_t* stubs = (uint8_t*)AllocNear(span, kSiteCount * 0x60, PAGE_EXECUTE_READWRITE);
    if (!stubs) { Log("[tyres] trainer fallback not applied: no free page within reach"); return; }
    uint8_t patch[kSiteCount][kSiteLen];
    for (int i = 0; i < kSiteCount; ++i) {
        uint8_t* stub = stubs + i * 0x60;
        const uintptr_t site = g_base + trainerload::kRvaSites[i];
        // Back to the instruction after the mov: the game's own path from there on.
        BuildStub(stub, i, (uint64_t)&TrainerTyrePick, (uint64_t)(site + kSiteLen), Arg3::R15,
                  Dest::Ebx);
        if (!BuildSitePatch(site, (uintptr_t)stub, patch[i])) {
            VirtualFree(stubs, 0, MEM_RELEASE);
            Log("[tyres] trainer fallback not applied: stub out of rel32 reach");
            return;
        }
    }
    FlushInstructionCache(GetCurrentProcess(), stubs, kSiteCount * 0x60);
    DWORD prot = 0;
    if (!VirtualProtect((void*)span, trainerload::kSpanLen, PAGE_EXECUTE_READWRITE, &prot)) {
        VirtualFree(stubs, 0, MEM_RELEASE);
        Log("[tyres] trainer fallback not applied: VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    for (int i = 0; i < kSiteCount; ++i)
        memcpy((void*)(g_base + trainerload::kRvaSites[i]), patch[i], kSiteLen);
    VirtualProtect((void*)span, trainerload::kSpanLen, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), (void*)span, trainerload::kSpanLen);
    g_trnTyreSpan = span;
    Log("[tyres] trainer fallback live: a trainer naming a tyre that isn't loaded gets the "
        "first loaded tyre instead of crashing the track load (+0x%llX).",
        (unsigned long long)trainerload::kRvaSpan);
}

static void InstallTyreFallback(intptr_t delta) {
    using namespace tyrefallback;
    if (g_game != &GAME_MXB) {
        Log("[tyres] fallback off for %s - the offsets are MX Bikes' only.", g_game->display);
        return;
    }
    InstallTrainerTyreFallback();   // its own exact-bytes check; independent of the bike list's
    const uintptr_t span = g_base + kRvaSpan + delta;
    uint8_t *tb, *te;
    if (!GetExecRange(g_base, &tb, &te) || (uint8_t*)span < tb || (uint8_t*)span + kSpanLen > te ||
        !SpanMatches((const uint8_t*)span)) {
        Log("[tyres] fallback not applied: the bytes at +0x%llX are not the tyre lookup we "
            "expect (game updated?). Nothing patched.", (unsigned long long)(kRvaSpan + delta));
        return;
    }
    uint8_t* stubs = (uint8_t*)AllocNear(span, kSiteCount * 0x60, PAGE_EXECUTE_READWRITE);
    if (!stubs) { Log("[tyres] fallback not applied: no free page within reach"); return; }

    uint8_t patch[kSiteCount][kSiteLen];
    for (int i = 0; i < kSiteCount; ++i) {
        uint8_t* stub = stubs + i * 0x60;
        BuildStub(stub, i, (uint64_t)&TyreFallbackPick, (uint64_t)(g_base + kRvaJoin + delta));
        const uintptr_t site = g_base + kRvaSites[i] + delta;
        if (!BuildSitePatch(site, (uintptr_t)stub, patch[i])) {
            VirtualFree(stubs, 0, MEM_RELEASE);
            Log("[tyres] fallback not applied: stub out of rel32 reach");
            return;
        }
    }
    DWORD prot = 0;
    if (!VirtualProtect((void*)span, kSpanLen, PAGE_EXECUTE_READWRITE, &prot)) {
        VirtualFree(stubs, 0, MEM_RELEASE);
        Log("[tyres] fallback not applied: VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    for (int i = 0; i < kSiteCount; ++i)
        memcpy((void*)(g_base + kRvaSites[i] + delta), patch[i], kSiteLen);
    VirtualProtect((void*)span, kSpanLen, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), (void*)span, kSpanLen);
    g_tyreSpan = span;
    Log("[tyres] fallback live: a bike naming a tyre that isn't loaded gets the first loaded "
        "tyre instead of crashing the bike list (+0x%llX).", (unsigned long long)(kRvaSpan + delta));
}

// ===========================================================================
// MAP GUARD  -  the quad-batch flush no longer writes through a bad vertex-buffer pointer
//
// mapguard.h has the chain. The flush at 0x2409D0 gets its batch's vertex pointer from
// 0x22AC50 and writes through it unchecked; on one RTX 5080 + ReShade setup that pointer is
// junk (the ram path's shadow pointer, in the 0.49.17 dumps) and the first store faults at
// mxbikes.exe+0x24085C. The flush's map and unmap calls now go through MgMap / MgUnmap: a
// pointer that is committed and writable is handed back untouched, anything else gets a
// scratch buffer of ours on every path, uploaded at the unmap (or kept, with no GL buffer).
// Only those two call instructions change; every other caller of the map function, and
// glMapBuffer itself, are left alone. mapguard=0 turns it off; mapguardtest=N fakes a bad
// pointer on every Nth map so the fallback can be watched in game.
// ===========================================================================
using MgMapFn         = void* (*)(void* vb);
using MgUnmapFn       = int (*)(void* vb, int first, int count);
using MgBindFn        = void (*)(void* vb);
using MgSubDataFn     = void(APIENTRY*)(GLenum, ptrdiff_t, ptrdiff_t, const void*);
using MgUnmapBufferFn = GLboolean(APIENTRY*)(GLenum);
using MgGetBufParamFn = void(APIENTRY*)(GLenum, GLenum, GLint*);

// Render thread only, except the counter (read at teardown for the summary line).
struct MgSlot {
    const void* vb; uint8_t* buf; size_t cap;
    size_t live;              // bytes lent, 0 = none
    mapguard::Path path;      // the path the game was on when we lent it
    bool stopped;             // the walker was stopped this flush (MgWalkerFault)
};
static MgSlot                g_mgSlots[mapguard::kMaxSlots] = {};
static uint64_t              g_mgMaps = 0;              // maps seen at this site (mapguardtest)
static std::atomic<unsigned> g_mgFired{0};
static unsigned              g_mgKindFired[mapguard::kKinds] = {};   // per (path, verdict), for the log
static MgGetBufParamFn       g_mgGetBufParam = nullptr;
static bool                  g_mgGetBufParamLooked = false;
static uintptr_t             g_mgSpan = 0;              // VA of the map call, 0 = not patched
static uint8_t*              g_mgStubs = nullptr;       // kept after revert: a thread may be on its way through

static mapguard::Verdict MgClassify(const void* p, size_t size, mapguard::Region* bad) {
    return mapguard::Classify((uintptr_t)p, size, [](uintptr_t a, mapguard::Region* r) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((const void*)a, &mbi, sizeof(mbi))) return false;
        r->base = (uintptr_t)mbi.BaseAddress; r->size = mbi.RegionSize;
        r->state = mbi.State; r->protect = mbi.Protect;
        return true;
    }, bad);
}

// Which of 0x22AC50's paths this record takes right now (same three ints the game tests).
static mapguard::Path MgPath(const uint8_t* rec) {
    return mapguard::PathOf(*(const int32_t*)(g_base + mapguard::kRvaGlOk),
                            *(const int32_t*)(g_base + mapguard::kRvaUseMap),
                            *(const int32_t*)(rec + mapguard::kVbGlName));
}

// The scratch kept for this record, grown to `size`. nullptr when there is no room for one.
static MgSlot* MgSlotFor(const void* vb, size_t size) {
    MgSlot* s = nullptr;
    for (MgSlot& x : g_mgSlots) if (x.vb == vb) { s = &x; break; }
    if (!s) for (MgSlot& x : g_mgSlots) if (!x.vb) { s = &x; s->vb = vb; break; }
    if (!s) return nullptr;
    if (s->cap < size) {
        uint8_t* nb = (uint8_t*)VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!nb) return nullptr;
        if (s->buf) VirtualFree(s->buf, 0, MEM_RELEASE);
        s->buf = nb; s->cap = size;
    }
    return s;
}

// Called by the flush instead of 0x22AC50, with the flush's own frame.
static void* MgMap(void* vb) {
    void* p = ((MgMapFn)(g_base + mapguard::kRvaMapFn))(vb);
    const uint8_t* rec = (const uint8_t*)vb;
    const int every = mapguard_test_every.load(std::memory_order_relaxed);
    const bool forced = every > 0 && mapguard::ForceThis(++g_mgMaps, every);
    const size_t size = mapguard::BufferBytes(*(const int32_t*)(rec + mapguard::kVbCount),
                                              *(const int32_t*)(rec + mapguard::kVbStride));
    mapguard::Region bad{};
    const mapguard::Verdict v = forced ? mapguard::Verdict::Forced : MgClassify(p, size, &bad);
    if (v == mapguard::Verdict::Ok) return p;

    // Whatever path the game is on, a bad pointer is never handed to the flush: it writes
    // into our scratch, and MgUnmap does the right thing with that for the path.
    const mapguard::Path path = MgPath(rec);
    MgSlot* s = size ? MgSlotFor(vb, size) : nullptr;
    const unsigned n = g_mgFired.fetch_add(1, std::memory_order_relaxed) + 1;
    const unsigned k = ++g_mgKindFired[mapguard::KindIndex(path, v)];
    if (mapguard::ShouldLog(k)) {
        if (!s)
            Log("[mapguard] %s: the quad batch's map gave %p (%s) for a %zu-byte buffer and "
                "there is no scratch to stand in (%s) - the game's pointer is kept. This kind "
                "%u, all kinds %u so far.",
                mapguard::PathName(path), p, mapguard::VerdictName(v), size,
                !size ? "record size out of range" : "out of memory", k, n);
        else if (v == mapguard::Verdict::Forced)
            Log("[mapguard] %s: mapguardtest=%d: map %llu treated as bad (real pointer %p) - this "
                "batch is written to FrostMod's copy. This kind %u, all kinds %u so far.",
                mapguard::PathName(path), every, (unsigned long long)g_mgMaps, p, k, n);
        else
            Log("[mapguard] %s: the quad batch's map gave %p, %s (region %p +0x%llx state 0x%x "
                "protect 0x%x), for a %zu-byte buffer - written to FrostMod's copy instead of "
                "crashing at mxbikes.exe+0x24085C. This kind %u, all kinds %u so far.",
                mapguard::PathName(path), p, mapguard::VerdictName(v), (void*)bad.base,
                (unsigned long long)bad.size, bad.state, bad.protect, size, k, n);
        frostmod::crash::Note("quad-batch map guard fired (%s, %s, %u of this kind, %u in all)",
                              mapguard::PathName(path), mapguard::VerdictName(v), k, n);
    }
    if (!(path == mapguard::Path::NoBuffer && v == mapguard::Verdict::Null) &&
        v != mapguard::Verdict::Forced)
        ProtCatch(protection::Kind::RenderBatch);   // a NULL on the no-buffer path is an empty batch, never a crash
    if (!s) return p;
    s->live = size;
    s->path = path;
    return s->buf;
}

// Called by the flush instead of 0x22ACC0. Anything we did not lend goes to the game's own.
static void MgClampStopped(uint8_t* batch, MgSlot* s, bool onOurs);   // below, with the walker guard

static int MgUnmap(void* vb, int first, int count) {
    MgSlot* s = nullptr;
    for (MgSlot& x : g_mgSlots) if (x.vb == vb) { s = &x; break; }
    if (s && s->stopped) {
        // The walker was stopped this flush (MgWalkerFault): draw only what is whole and ours.
        s->stopped = false;
        uint8_t* batch = (uint8_t*)vb - mapguard::kBatchVb;
        MgClampStopped(batch, s, s->live != 0);
        if (!s->live) count = 0;
    }
    if (!s || !s->live)
        return ((MgUnmapFn)(g_base + mapguard::kRvaUnmapFn))(vb, first, count);

    const size_t n = s->live;
    s->live = 0;
    const mapguard::UnmapAction act = mapguard::UnmapFor(s->path, MgPath((const uint8_t*)vb));
    if (act == mapguard::UnmapAction::Game) {
        // No GL buffer: the game's unmap returns 1 without touching GL. The flush's writes
        // went into our scratch, which stays allocated, so nothing is left pointing at junk.
        return mapguard::UnmapResult(act, ((MgUnmapFn)(g_base + mapguard::kRvaUnmapFn))(vb, first, count));
    }
    auto subData = *(MgSubDataFn*)(g_base + mapguard::kRvaGlBufferSubData);
    auto unmapBuf = *(MgUnmapBufferFn*)(g_base + mapguard::kRvaGlUnmapBuffer);
    // The buffer the map bound; re-bound through the game's cache so the cache stays right.
    ((MgBindFn)(g_base + mapguard::kRvaBindFn))(vb);
    if (act == mapguard::UnmapAction::CloseThenUpload) {
        // SubData on a buffer GL still has mapped is an error, so a mapping that "succeeded"
        // with a junk pointer (or a forced one) is closed first. If GL says it is not mapped,
        // leave it.
        if (!g_mgGetBufParamLooked) {
            g_mgGetBufParamLooked = true;
            g_mgGetBufParam = (MgGetBufParamFn)wglGetProcAddress("glGetBufferParameteriv");
        }
        GLint mapped = 1;
        if (g_mgGetBufParam) g_mgGetBufParam(mapguard::kGlArrayBuffer, mapguard::kGlBufferMapped, &mapped);
        if (mapped && unmapBuf) unmapBuf(mapguard::kGlArrayBuffer);
    }
    // On the ram path this is the game's own upload, from our copy instead of its bad RAM
    // pointer. An empty batch uploads nothing (as the game's unmap does); any other goes up
    // whole, since the four arrays sit at fixed offsets across all of it.
    const size_t up = mapguard::UploadBytes(n, count);
    if (subData && up) subData(mapguard::kGlArrayBuffer, 0, (ptrdiff_t)up, s->buf);
    return mapguard::UnmapResult(act, 0);
}

// ---- the second line: a walker store that faults anyway (mapguard.h) ------------------
// MgMap checks the pointer once. v0.51.0 still crashed at +0x24085C with the check passed,
// so the walker's own stores are watched too: a write fault on one of them moves the batch
// onto our scratch (the same one MgMap lends) and the store runs again, now into our copy.
// MgUnmap then uploads that copy exactly as if MgMap had lent it. A fault on our own scratch
// means the walk ran off the end of a whole buffer: that walker frame leaves through its own
// epilogue and the draw is clamped to whole quads. Only the faulting thread's frame changes,
// and only for an rip inside the walker's quad loop.
static PVOID                 g_mgVeh = nullptr;
static uintptr_t             g_mgBatchTable = 0;      // VA of the ten batch pointers
static std::atomic<unsigned> g_mgMoved{0}, g_mgStopped{0};

static MgSlot* MgSlotOf(const void* vb) {
    for (MgSlot& x : g_mgSlots) if (x.vb == vb) return &x;
    return nullptr;
}

static LONG CALLBACK MgWalkerFault(EXCEPTION_POINTERS* ep) {
    using namespace mapguard;
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    CONTEXT* c = ep->ContextRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 ||
        er->ExceptionInformation[0] != 1 /*write*/ || !g_mgSpan || !g_mgBatchTable)
        return EXCEPTION_CONTINUE_SEARCH;
    if (c->Rip < g_base || !InWalkerStores(c->Rip - g_base)) return EXCEPTION_CONTINUE_SEARCH;

    uint8_t* b = (uint8_t*)c->Rsi;
    bool known = false;
    for (int i = 0; i < kBatches && !known; ++i)
        known = b && *(uint8_t* const*)(g_mgBatchTable + 8 * i) == b;
    const void* vb = known ? b + kBatchVb : nullptr;
    const size_t size = known ? BufferBytes(*(const int32_t*)(b + kBatchVb + kVbCount),
                                            *(const int32_t*)(b + kBatchVb + kVbStride)) : 0;
    uint32_t offs[4] = {};
    for (int k = 0; known && k < 4; ++k) offs[k] = *(const uint32_t*)(b + kArrayOffs[k]);
    MgSlot* s = known ? MgSlotOf(vb) : nullptr;
    const bool onScratch = s && s->live && s->buf &&
        *(uint8_t**)(b + kBatchArrays) >= s->buf && *(uint8_t**)(b + kBatchArrays) < s->buf + s->cap;
    if (known && !onScratch && size && OffsetsFit(offs, size)) s = MgSlotFor(vb, size);
    const bool canMove = s && s->buf && s->cap >= size && size && OffsetsFit(offs, size);
    const FaultAction act = OnWalkerFault(known, onScratch, canMove);
    if (act == FaultAction::Pass) return EXCEPTION_CONTINUE_SEARCH;

    const uintptr_t target = (uintptr_t)er->ExceptionInformation[1];
    if (act == FaultAction::Move) {
        memset(s->buf, 0, size);   // the quads already written are dropped, not drawn stale
        DWORD64* cur[4] = {&c->R10, &c->R11, &c->Rdx, &c->Rcx};
        uint64_t oldBase[4];
        for (int k = 0; k < 4; ++k) {
            uint8_t** arr = (uint8_t**)(b + kBatchArrays + 8 * k);
            oldBase[k] = (uint64_t)*arr;
            *arr = s->buf + offs[k];
            *cur[k] = Rebase(*cur[k], oldBase[k], (uint64_t)*arr);
        }
        s->live = size;
        s->path = MgPath(b + kBatchVb);
        s->stopped = false;
        const unsigned n = g_mgMoved.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ShouldLog(n)) {
            MEMORY_BASIC_INFORMATION mbi{};
            VirtualQuery((const void*)target, &mbi, sizeof(mbi));
            Log("[mapguard] %s: the quad walker's store at +0x%llX faulted writing %p (positions "
                "%p, %d vertices in; region %p +0x%llx state 0x%x protect 0x%x) - the batch was "
                "moved onto FrostMod's copy instead of crashing at mxbikes.exe+0x24085C. %u so far.",
                PathName(s->path), (unsigned long long)(c->Rip - g_base), (void*)target,
                (void*)oldBase[0], *(const int32_t*)(b + kBatchCount), mbi.BaseAddress,
                (unsigned long long)mbi.RegionSize, (unsigned)mbi.State, (unsigned)mbi.Protect, n);
            frostmod::crash::Note("quad walker store faulted, batch moved to FrostMod's copy (%u so far)", n);
        }
        ProtCatch(protection::Kind::RenderBatch);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // Stop: leave this frame through the walker's own epilogue.
    if (s) s->stopped = true;
    const uintptr_t storeRva = c->Rip - g_base;
    c->Rip = g_base + kRvaWalkExit;
    const unsigned n = g_mgStopped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ShouldLog(n)) {
        Log("[mapguard] the quad walker ran past its buffer (store at +0x%llX writing %p, %d "
            "vertices, %s) - the walk was ended there and only whole quads are drawn. %u so far.",
            (unsigned long long)storeRva, (void*)target, *(const int32_t*)(b + kBatchCount),
            onScratch ? "on FrostMod's copy" : "on the game's memory", n);
        frostmod::crash::Note("quad walker stopped at the end of its buffer (%u so far)", n);
    }
    ProtCatch(protection::Kind::RenderBatch);
    return EXCEPTION_CONTINUE_EXECUTION;
}

/// After a stop the batch's count can be past the buffer; clamp what the flush draws. A stop
/// on the game's own memory draws nothing (that memory is what failed).
static void MgClampStopped(uint8_t* batch, MgSlot* s, bool onOurs) {
    int32_t* count = (int32_t*)(batch + mapguard::kBatchCount);
    int32_t* tris  = (int32_t*)(batch + mapguard::kBatchTris);
    if (!onOurs) { *count = 0; *tris = 0; return; }
    const int32_t stride = *(const int32_t*)(batch + mapguard::kBatchVb + mapguard::kVbStride);
    mapguard::ClampDraw(count, tris, stride > 0 ? (int32_t)(s->live / (size_t)stride) : 0);
}

static void InstallWalkerFaultGuard() {
    using namespace mapguard;
    if (!g_mgSpan || g_mgVeh) return;
    // The table the flush indexes, decoded from its own `lea r13, [rip+disp]` at 0x240A89.
    const uintptr_t table = (uintptr_t)((int64_t)(kRvaMapWin + 7) +
        (int32_t)((uint32_t)kMapWin[3] | (uint32_t)kMapWin[4] << 8 | (uint32_t)kMapWin[5] << 16 |
                  (uint32_t)kMapWin[6] << 24));
    if (table != kRvaBatchTable ||
        !Matches((const uint8_t*)(g_base + kRvaWalkLoLoop), kWalkHead, kWalkHeadLen) ||
        !Matches((const uint8_t*)(g_base + kRvaWalkExit), kWalkExit, kWalkExitLen)) {
        Log("[mapguard] walker fault guard not applied: the walker's bytes are not what we expect.");
        return;
    }
    g_mgBatchTable = g_base + table;
    g_mgVeh = AddVectoredExceptionHandler(1 /*first*/, MgWalkerFault);
    if (!g_mgVeh) { g_mgBatchTable = 0; Log("[mapguard] walker fault guard not applied: no handler"); return; }
    Log("[mapguard] walker fault guard on: a quad-walker store that faults (+0x%llX..+0x%llX) "
        "moves the batch onto FrostMod's copy.", (unsigned long long)kRvaWalkLoLoop,
        (unsigned long long)kRvaWalkHiLoop);
}

// Put the two calls back exactly as they shipped. Called from Shutdown(): the stubs jump into
// this dll, which the game unloads next.
static void MgRevert() {
    if (g_mgVeh) { RemoveVectoredExceptionHandler(g_mgVeh); g_mgVeh = nullptr; }
    if (!g_mgSpan) return;
    using namespace mapguard;
    const uintptr_t mapSite = g_base + kRvaMapCall, unmapSite = g_base + kRvaUnmapCall;
    const size_t span = (unmapSite + kCallLen) - mapSite;
    DWORD prot = 0;
    if (!VirtualProtect((void*)mapSite, span, PAGE_EXECUTE_READWRITE, &prot)) {
        Log("[mapguard] could not revert: VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    memcpy((void*)mapSite, kMapWin + kMapCallInWin, kCallLen);
    memcpy((void*)unmapSite, kUnmapWin + kUnmapCallInWin, kCallLen);
    VirtualProtect((void*)mapSite, span, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), (void*)mapSite, span);
    g_mgSpan = 0;
    const unsigned n = g_mgFired.load();
    Log("[mapguard] off: the quad batch's own map/unmap calls are back%s",
        n ? "" : " (it never fired this run)");
    if (n) Log("[mapguard] fired %u time(s) this run", n);
}

static void InstallMapGuardImpl() {
    using namespace mapguard;
    if (g_game != &GAME_MXB) {
        Log("[mapguard] off for %s - the offsets are MX Bikes' only.", g_game->display);
        return;
    }
    if (!mapguard_on.load()) { Log("[mapguard] off by config (mapguard=0)"); return; }
    if (g_mgSpan) return;
    if (g_unloading.load()) return;
    const uintptr_t mapSite = g_base + kRvaMapCall, unmapSite = g_base + kRvaUnmapCall;
    uint8_t *tb, *te;
    if (!GetExecRange(g_base, &tb, &te) ||
        (uint8_t*)(g_base + kRvaMapFn) < tb || (uint8_t*)(g_base + kRvaUnmapWin + kUnmapWinLen) > te ||
        !Matches((const uint8_t*)(g_base + kRvaMapWin), kMapWin, kMapWinLen) ||
        !Matches((const uint8_t*)(g_base + kRvaUnmapWin), kUnmapWin, kUnmapWinLen) ||
        !Matches((const uint8_t*)(g_base + kRvaMapFn), kMapFnBytes, kMapFnLen) ||
        !Matches((const uint8_t*)(g_base + kRvaUnmapFn), kUnmapFnBytes, kUnmapFnLen)) {
        Log("[mapguard] not applied: the bytes at +0x%llX / +0x%llX are not the quad-batch map "
            "and unmap calls we expect (game updated?). Nothing patched.",
            (unsigned long long)kRvaMapCall, (unsigned long long)kRvaUnmapCall);
        return;
    }
    if (!g_mgStubs) g_mgStubs = (uint8_t*)AllocNear(mapSite, 0x40, PAGE_EXECUTE_READWRITE);
    if (!g_mgStubs) { Log("[mapguard] not applied: no free page within reach"); return; }
    BuildStub(g_mgStubs, (uint64_t)&MgMap);
    BuildStub(g_mgStubs + 0x20, (uint64_t)&MgUnmap);
    FlushInstructionCache(GetCurrentProcess(), g_mgStubs, 0x40);

    uint8_t mapPatch[kCallLen], unmapPatch[kCallLen];
    if (!BuildCall(mapSite, (uintptr_t)g_mgStubs, mapPatch) ||
        !BuildCall(unmapSite, (uintptr_t)(g_mgStubs + 0x20), unmapPatch)) {
        Log("[mapguard] not applied: stub out of rel32 reach");
        return;
    }
    const size_t span = (unmapSite + kCallLen) - mapSite;
    DWORD prot = 0;
    if (!VirtualProtect((void*)mapSite, span, PAGE_EXECUTE_READWRITE, &prot)) {
        Log("[mapguard] not applied: VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    // Unmap first: until the map side is ours nothing is lent, so a flush that runs between
    // the two writes still pairs the game's own map with the game's own unmap.
    memcpy((void*)unmapSite, unmapPatch, kCallLen);
    memcpy((void*)mapSite, mapPatch, kCallLen);
    VirtualProtect((void*)mapSite, span, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), (void*)mapSite, span);
    g_mgSpan = mapSite;

    // Prove both landed by decoding them back.
    if (CallTarget(mapSite, (const uint8_t*)mapSite) != (uintptr_t)g_mgStubs ||
        CallTarget(unmapSite, (const uint8_t*)unmapSite) != (uintptr_t)(g_mgStubs + 0x20)) {
        MgRevert();
        Log("[mapguard] not applied: the patch did not verify, reverted");
        return;
    }
    InstallWalkerFaultGuard();
    const int every = mapguard_test_every.load();
    Log("[mapguard] on: the quad batch's vertex pointer is checked before the game writes "
        "to it; a bad one is replaced by FrostMod's copy on any path (calls at +0x%llX / +0x%llX).",
        (unsigned long long)kRvaMapCall, (unsigned long long)kRvaUnmapCall);
    if (every > 0)
        Log("[mapguard] TEST MODE (mapguardtest=%d): every %dth map is treated as bad on purpose. "
            "Remove the line from frostmod_radar.cfg when done.", every, every);
}

// The sibling sampler 0x1F1D10 (see offsets.h): same NaN escape, other grid. It answers
// "no answer" with 1, so that is what a refused query returns here.
using TerrainSample2Fn = int32_t (*)(void*, void*, void*, float, float);
static TerrainSample2Fn g_origTerrain2 = nullptr;
static std::atomic<unsigned> g_terrain2Guarded{0};
static int32_t hkTerrainSample2(void* obj, void* a2, void* a3, float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        const unsigned n = g_terrain2Guarded.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 10)
            Log("[terrain] refused a grid query at (%f, %f) - not a position. This is the "
                "crash at mxbikes.exe+0x%zx. Refused %u so far.",
                (double)x, (double)y, (size_t)mxb::RVA_TERRAIN_FAULT2, n);
        if (n <= 3) {
            char frames[12][160];
            const int got = frostmod::crash::CaptureStack(frames, 12);
            Log("[terrain] who asked (nearest first, %d frame(s)):", got);
            for (int i = 0; i < got; ++i) Log("[terrain]   #%-2d %s", i, frames[i]);
        }
        frostmod::crash::Note("refused a NaN grid query (%u so far)", n);
        ProtCatch(protection::Kind::PhysicsNan);
        return 1;   // this function's own "no answer"
    }
    return g_origTerrain2(obj, a2, a3, x, y);
}

static void InstallTerrainGuard2(intptr_t delta, uint8_t* begin, uint8_t* end) {
    uint8_t* fn = (uint8_t*)(g_base + mxb::RVA_TERRAIN_SAMPLE2 + delta);
    if (fn < begin || fn >= end ||
        !MatchAt(fn, mxb::SIG_TERRAIN_SAMPLE2, mxb::SIG_TERRAIN_SAMPLE2_MASK)) {
        uint8_t* found =
            PatternScan(begin, end, mxb::SIG_TERRAIN_SAMPLE2, mxb::SIG_TERRAIN_SAMPLE2_MASK);
        if (!found) {
            Log("[terrain] second guard off: 0x1F1D10's signature is not in this build's .text.");
            return;
        }
        Log("[terrain] second sampler RELOCATED: found at RVA 0x%zx (offsets.h says 0x%zx).",
            (size_t)(found - g_base), (size_t)mxb::RVA_TERRAIN_SAMPLE2);
        fn = found;
    }
    if (InstallHook(fn, (void*)&hkTerrainSample2, (void**)&g_origTerrain2,
                    "terrainSample2(0x1f1d10)"))
        Log("[terrain] second height-query guard live @ RVA 0x%zx.", (size_t)(fn - g_base));
}

static void InstallTerrainGuard(intptr_t delta) {
    if (!g_game->offsets_complete) {
        Log("[terrain] guard off for %s - the sampler is MX Bikes' and has no twin here.",
            g_game->display);
        return;
    }
    uint8_t *begin, *end;
    if (!GetExecRange(g_base, &begin, &end)) {
        Log("[terrain] guard off: can't read the module's sections to place it.");
        return;
    }
    InstallTerrainGuard2(delta, begin, end);
    uint8_t* fn = (uint8_t*)(g_base + mxb::RVA_TERRAIN_SAMPLE + delta);
    if (fn < begin || fn >= end ||
        !MatchAt(fn, mxb::SIG_TERRAIN_SAMPLE, mxb::SIG_TERRAIN_SAMPLE_MASK)) {
        uint8_t* found =
            PatternScan(begin, end, mxb::SIG_TERRAIN_SAMPLE, mxb::SIG_TERRAIN_SAMPLE_MASK);
        if (!found) {
            Log("[terrain] guard off: the sampler's signature is not in this build's .text. "
                "If the game updated, re-derive RVA_TERRAIN_SAMPLE before trusting it.");
            return;
        }
        Log("[terrain] sampler RELOCATED: found at RVA 0x%zx (offsets.h says 0x%zx).",
            (size_t)(found - g_base), (size_t)mxb::RVA_TERRAIN_SAMPLE);
        fn = found;
    }
    if (InstallHook(fn, (void*)&hkTerrainSample, (void**)&g_origTerrain,
                    "terrainSample(0x1f1720)"))
        Log("[terrain] height-query guard live @ RVA 0x%zx. A query that is not a position "
            "now returns nothing instead of reading off the grid.", (size_t)(fn - g_base));
}

// ===========================================================================
// SHOCK GUARD  -  the rear shock's curve lookup no longer indexes with a NaN (+0x1A29AE)
//
// shockguard.h has the chain. The terrain guard above turns a NaN bike's height queries away,
// but the bike is still NaN, and the next thing to read it is the rear shock: 0x1A2970 looks
// up the shock curve with x = 1 - length / stroke, NaN passes both of its range checks, and
// the read at +0x1A29AE lands 8 GB below the curve. That is the top crash in the reports, and
// it is what OB's 2026-10-28 report is.
//
// The one call to 0x1A2970 now goes through a machine-code stub that makes a NaN x into 0 -
// the function's own "x <= 0" branch, curve[0] - and counts it. A finite x goes straight on.
// The stub keeps every register the game's leaf keeps (the caller relies on xmm5 surviving
// the call), so it does no logging of its own: ShockGuardTick() reads its counter once a
// frame, on the render thread, and writes the log line and the crash-trail note from there.
// shockguard=0 turns it off.
// ===========================================================================
static uintptr_t         g_sgSite   = 0;         // VA of the call we repointed, 0 = not patched
static uint8_t*          g_sgStub   = nullptr;   // kept after revert: a thread may be on its way through
static uint32_t          g_sgLogged = 0;         // the count at the last log line

static void ShockGuardTick() {
    if (!g_sgStub) return;
    const uint32_t n = *(volatile const uint32_t*)(g_sgStub + shockguard::kCountOff);
    if (n == g_sgLogged) return;
    ProtCatch(protection::Kind::PhysicsNan);
    const uint32_t prev = g_sgLogged;
    g_sgLogged = n;
    // Log when the count crosses a line the rate limit allows, and always the first.
    bool due = prev == 0;
    for (uint64_t k = (uint64_t)prev + 1; !due && k <= n && k <= (uint64_t)prev + 64; ++k)
        due = shockguard::ShouldLog(k);
    if (!due && n - prev > 64) due = true;
    if (!due) return;
    const uint8_t* bike = *(uint8_t* volatile const*)(g_sgStub + shockguard::kBikeOff);
    double len = 0, stroke = 0;
    if (bike) {
        SafeReadBytes((const char*)(bike + shockguard::kBikeShockLen), (char*)&len, sizeof(len));
        SafeReadBytes((const char*)(bike + shockguard::kBikeStroke), (char*)&stroke, sizeof(stroke));
    }
    Log("[shock] turned away a NaN shock-curve lookup (bike %p, shock length %g, stroke %g). "
        "This is the crash at mxbikes.exe+0x%zx; the game got curve[0] instead. %u so far.",
        (const void*)bike, len, stroke, (size_t)shockguard::kRvaFault, n);
    frostmod::crash::Note("refused a NaN shock lookup (%u so far)", n);
}

// Put the call back exactly as it shipped. The stub only jumps into the game, never into this
// dll, but the game gets its bytes back on unload like every other patch.
static void SgRevert() {
    if (!g_sgSite) return;
    using namespace shockguard;
    DWORD prot = 0;
    if (!VirtualProtect((void*)g_sgSite, kCallLen, PAGE_EXECUTE_READWRITE, &prot)) {
        Log("[shock] could not revert: VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    // Same as the install: only the rel32 differs, and the physics thread may be running this
    // call, so it goes back with one aligned store where it can.
    int32_t orig = 0;
    memcpy(&orig, kWin + kCallInWin + 1, sizeof(orig));
    if (((g_sgSite + 1) & 3) == 0) InterlockedExchange((volatile LONG*)(g_sgSite + 1), (LONG)orig);
    else                           memcpy((void*)(g_sgSite + 1), &orig, sizeof(orig));
    VirtualProtect((void*)g_sgSite, kCallLen, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), (void*)g_sgSite, kCallLen);
    g_sgSite = 0;
    const uint32_t n = g_sgStub ? *(volatile const uint32_t*)(g_sgStub + kCountOff) : 0;
    if (n) Log("[shock] off: the rear shock's own call is back (turned away %u NaN lookup(s) this run)", n);
    else   Log("[shock] off: the rear shock's own call is back (it never fired this run)");
}

static void InstallShockGuardImpl() {
    using namespace shockguard;
    if (g_game != &GAME_MXB) {
        Log("[shock] guard off for %s - the offsets are MX Bikes' only.", g_game->display);
        return;
    }
    if (!shockguard_on.load()) { Log("[shock] guard off by config (shockguard=0)"); return; }
    if (g_sgSite) return;
    if (g_unloading.load()) return;
    const uintptr_t site = g_base + kRvaCall;
    uint8_t *tb, *te;
    if (!GetExecRange(g_base, &tb, &te) ||
        (uint8_t*)(g_base + kRvaCurveFn) < tb || (uint8_t*)(g_base + kRvaWin + kWinLen) > te ||
        !Matches((const uint8_t*)(g_base + kRvaWin), kWin, kWinLen) ||
        !Matches((const uint8_t*)(g_base + kRvaCurveFn), kFnBytes, kFnLen) ||
        !Matches((const uint8_t*)(g_base + kRvaLenLoad), kLenLoad, kLoadLen) ||
        !Matches((const uint8_t*)(g_base + kRvaStrokeLoad), kStrokeLoad, kLoadLen)) {
        Log("[shock] guard not applied: the bytes at +0x%llX are not the rear shock's curve "
            "lookup we expect (game updated?). Nothing patched.", (unsigned long long)kRvaCall);
        return;
    }
    if (!g_sgStub) g_sgStub = (uint8_t*)AllocNear(site, kPageBytes, PAGE_EXECUTE_READWRITE);
    if (!g_sgStub) { Log("[shock] guard not applied: no free page within reach"); return; }
    memset(g_sgStub, 0xCC, kPageBytes);
    memset(g_sgStub + kCountOff, 0, kPageBytes - kCountOff);
    BuildStub(g_sgStub, (uintptr_t)g_sgStub, (uint64_t)(g_base + kRvaCurveFn));
    FlushInstructionCache(GetCurrentProcess(), g_sgStub, kPageBytes);
    g_sgLogged = 0;

    int32_t rel = 0;
    if (!CallRel(site, (uintptr_t)g_sgStub, &rel)) {
        Log("[shock] guard not applied: stub out of rel32 reach");
        return;
    }
    DWORD prot = 0;
    if (!VirtualProtect((void*)site, kCallLen, PAGE_EXECUTE_READWRITE, &prot)) {
        Log("[shock] guard not applied: VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    // Only the rel32 changes, and it is 4-byte aligned in this build (0x1A2EF4), so the physics
    // thread sees the old call or the new one, never half of each.
    if (((site + 1) & 3) == 0) InterlockedExchange((volatile LONG*)(site + 1), (LONG)rel);
    else                       memcpy((void*)(site + 1), &rel, sizeof(rel));
    VirtualProtect((void*)site, kCallLen, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, kCallLen);
    g_sgSite = site;

    if (CallTarget(site, (const uint8_t*)site) != (uintptr_t)g_sgStub) {
        SgRevert();
        Log("[shock] guard not applied: the patch did not verify, reverted");
        return;
    }
    Log("[shock] guard on: a NaN rear-shock curve lookup gets curve[0] instead of reading 8 GB "
        "off the table (call at +0x%llX, the crash at +0x%llX).",
        (unsigned long long)kRvaCall, (unsigned long long)kRvaFault);
}

// ===========================================================================
// SESSION GUARDS  -  four crashes from thomas4f's beta21e list (src/sessionguards.h)
//
//   +0x45B0    bike list read with an index that is not a bike      bikelistguard=0
//   +0x3B940   wake effect update after the track freed its list    wakeguard=0
//   +0x1FED9E  list box line naming an item it does not have        listboxguard=0
//   +0x2A42F0  stack pointer passed through 32 bits (job 863)       ptr64fix=0
//
// sessionguards.h has the code for each. The first two are MinHook detours on the game's
// function that answer the way the function already answers an empty list. The last two are
// MinHook jumps in the middle of a function to a machine-code stub (as the server filter's
// loop-top is): MinHook holds the game's threads while it writes, which a hand-written
// multi-byte patch here could not promise. Every one checks its bytes first and stays off on
// a mismatch; the stubs only jump back into the game, and MH_DisableHook at unload puts the
// bytes back. The stubs count what they turn away and SessionGuardsTick() logs it from the
// frame, as the shock guard does; the detours log from the call, at most ~20 lines a session.
// ===========================================================================

/// The bytes at `rva` are `want`, inside .text. beta21e only: every guard below refuses
/// rather than looking elsewhere.
static bool SsgCodeIs(uintptr_t rva, const uint8_t* want, size_t n) {
    uint8_t *tb, *te;
    if (!GetExecRange(g_base, &tb, &te)) return false;
    const uint8_t* p = (const uint8_t*)(g_base + rva);
    return p >= tb && p + n <= te && sessionguards::Matches(p, want, n);
}

/// The data cell a rip-relative operand we have already matched names, if it is in the image.
static void* SsgCell(const uint8_t* insn, uintptr_t rva, size_t dispAt, size_t len, size_t bytes) {
    const uintptr_t cell = sessionguards::RipTarget(insn, rva, dispAt, len);
    auto* dos = (IMAGE_DOS_HEADER*)g_base;
    auto* nt  = (IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew);
    if (cell < 0x1000 || cell + bytes > nt->OptionalHeader.SizeOfImage) return nullptr;
    return (void*)(g_base + cell);
}

static bool SsgMxbOnly(const char* tag) {
    if (g_game == &GAME_MXB) return true;
    Log("[%s] guard off for %s - the offsets are MX Bikes' only.", tag, g_game->display);
    return false;
}

// ---- +0x45B0: the bike list ----------------------------------------------------------
using BikeCatFn = int (*)(int bike, int* cat);
static BikeCatFn                g_origBikeCat = nullptr;
static void* volatile*          g_blList      = nullptr;   // &bike list pointer
static volatile const int32_t*  g_blCount     = nullptr;
static void* volatile*          g_blCats      = nullptr;   // &category list pointer
static volatile const int32_t*  g_blCatCount  = nullptr;
static std::atomic<uint32_t>    g_blRefused{0};

static int hkBikeCat(int bike, int* cat) {
    const int32_t count = *g_blCount;
    if (sessionguards::bikelist::Admit(bike, count, *g_blList != nullptr, cat, *g_blCatCount,
                                       *g_blCats != nullptr))
        return g_origBikeCat(bike, cat);
    const uint32_t n = g_blRefused.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sessionguards::ShouldLog(n)) {
        Log("[bikelist] turned away a bike-list read for bike %d (the list has %d) - the crash at "
            "mxbikes.exe+0x45B0. The game got 'no names in common', as for a bike with none. "
            "%u so far.", bike, count, n);
        frostmod::crash::Note("refused bike-list index %d of %d (%u so far)", bike, count, n);
    }
    ProtCatch(protection::Kind::BikeList);
    return sessionguards::bikelist::kEmpty;
}

static void InstallBikeListGuard() {
    using namespace sessionguards::bikelist;
    if (!SsgMxbOnly("bikelist")) return;
    if (!bikelistguard_on.load()) { Log("[bikelist] guard off by config (bikelistguard=0)"); return; }
    if (g_origBikeCat || g_unloading.load()) return;
    if (!SsgCodeIs(kRvaFn, kFn, kFnLen) ||
        !SsgCodeIs(kRvaBikeCount, kBikeCount, sizeof(kBikeCount)) ||
        !SsgCodeIs(kRvaBikeTable, kBikeTable, sizeof(kBikeTable)) ||
        !SsgCodeIs(kRvaCatCount, kCatCount, sizeof(kCatCount)) ||
        !SsgCodeIs(kRvaCatTable, kCatTable, sizeof(kCatTable))) {
        Log("[bikelist] guard not applied: the bytes at +0x%llX are not the bike-list lookup we "
            "expect (game updated?). Nothing patched.", (unsigned long long)kRvaFn);
        return;
    }
    // The cells, decoded from the function's own loads; the counts from the loops that read
    // the same tables, which must decode to the same cells.
    void* list  = SsgCell(kFn + kBikeTableAt, kRvaFn + kBikeTableAt, kLoadDisp, kLoadLen, 8);
    void* cats  = SsgCell(kFn + kCatTableAt, kRvaFn + kCatTableAt, kLoadDisp, kLoadLen, 8);
    void* list2 = SsgCell(kBikeTable, kRvaBikeTable, 3, 7, 8);
    void* cats2 = SsgCell(kCatTable, kRvaCatTable, 3, 7, 8);
    void* count = SsgCell(kBikeCount, kRvaBikeCount, 2, 6, 4);
    void* catn  = SsgCell(kCatCount, kRvaCatCount, 2, 6, 4);
    if (!list || !cats || !count || !catn || list != list2 || cats != cats2) {
        Log("[bikelist] guard not applied: the list and count cells do not decode consistently.");
        return;
    }
    g_blList = (void* volatile*)list;  g_blCount = (volatile const int32_t*)count;
    g_blCats = (void* volatile*)cats;  g_blCatCount = (volatile const int32_t*)catn;
    if (!InstallHook((void*)(g_base + kRvaFn), (void*)&hkBikeCat, (void**)&g_origBikeCat,
                     "bikeListLookup(0x4580)")) {
        g_origBikeCat = nullptr;
        return;
    }
    Log("[bikelist] guard on: a bike index outside the bike list gets 'no names in common' "
        "instead of a read off the list (fn +0x%llX, the crash at +0x%llX; %d bikes loaded).",
        (unsigned long long)kRvaFn, (unsigned long long)kRvaFault, (int)*g_blCount);
}

// ---- +0x3B940: the wake effects ------------------------------------------------------
using WakeFn = int (*)(float dt);
static WakeFn                  g_origWake  = nullptr;
static void* volatile*         g_wkList    = nullptr;
static void* volatile*         g_wkVerts   = nullptr;
static volatile int32_t*       g_wkCount   = nullptr;
static std::atomic<uint32_t>   g_wkSkipped{0};

static int hkWake(float dt) {
    if (sessionguards::wake::Admit(*g_wkList, *g_wkVerts)) return g_origWake(dt);
    *g_wkCount = 0;   // what the update stores first; 0x5AA50 uploads this many vertices
    const uint32_t n = g_wkSkipped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sessionguards::ShouldLog(n)) {
        Log("[wake] skipped a wake-effect update: the track teardown already freed its list - the "
            "crash at mxbikes.exe+0x3B940. %u so far.", n);
        frostmod::crash::Note("skipped a wake update after teardown (%u so far)", n);
    }
    ProtCatch(protection::Kind::Wake);
    return 0;
}

static void InstallWakeGuard() {
    using namespace sessionguards::wake;
    if (!SsgMxbOnly("wake")) return;
    if (!wakeguard_on.load()) { Log("[wake] guard off by config (wakeguard=0)"); return; }
    if (g_origWake || g_unloading.load()) return;
    if (!SsgCodeIs(kRvaFn, kHead, kHeadLen) ||
        !SsgCodeIs(kRvaCountStore, kCountStore, sizeof(kCountStore)) ||
        !SsgCodeIs(kRvaLoop, kLoop, kLoopLen)) {
        Log("[wake] guard not applied: the bytes at +0x%llX are not the wake-effect update we "
            "expect (game updated?). Nothing patched.", (unsigned long long)kRvaFn);
        return;
    }
    void* list  = SsgCell(kHead + kListAt, kRvaFn + kListAt, 3, 7, 8);
    void* verts = SsgCell(kHead + kVertsAt, kRvaFn + kVertsAt, 3, 7, 8);
    void* count = SsgCell(kCountStore, kRvaCountStore, 3, 7, 4);
    if (!list || !verts || !count) {
        Log("[wake] guard not applied: the decoded cells are outside the module.");
        return;
    }
    g_wkList = (void* volatile*)list; g_wkVerts = (void* volatile*)verts;
    g_wkCount = (volatile int32_t*)count;
    if (!InstallHook((void*)(g_base + kRvaFn), (void*)&hkWake, (void**)&g_origWake,
                     "wakeUpdate(0x3b8e0)")) {
        g_origWake = nullptr;
        return;
    }
    Log("[wake] guard on: a wake-effect update after the track freed its list is skipped "
        "(fn +0x%llX, the crash at +0x%llX).", (unsigned long long)kRvaFn, (unsigned long long)kRvaFault);
}

// ---- +0x1FED9E: the list box line ----------------------------------------------------
static uint8_t* g_lbStub   = nullptr;   // kept after unload: a thread may be on its way through
static void*    g_lbTramp  = nullptr;   // MinHook's; unused, the stub jumps back itself
static uint32_t g_lbLogged = 0;

static void InstallListBoxGuard() {
    using namespace sessionguards::listbox;
    if (!SsgMxbOnly("listbox")) return;
    if (!listboxguard_on.load()) { Log("[listbox] guard off by config (listboxguard=0)"); return; }
    if (g_lbTramp || g_unloading.load()) return;
    if (!SsgCodeIs(kRvaWin, kWin, kWinLen) || !SsgCodeIs(kRvaTail, kTail, sizeof(kTail)) ||
        !SsgCodeIs(kRvaAdd, kAdd, kAddLen)) {
        Log("[listbox] guard not applied: the bytes at +0x%llX are not the list-box draw we "
            "expect (game updated?). Nothing patched.", (unsigned long long)kRvaSite);
        return;
    }
    const uintptr_t site = g_base + kRvaSite;
    if (!g_lbStub) g_lbStub = (uint8_t*)AllocNear(site, kPageBytes, PAGE_EXECUTE_READWRITE);
    if (!g_lbStub) { Log("[listbox] guard not applied: no page for the stub"); return; }
    memset(g_lbStub, 0xCC, kPageBytes);
    memset(g_lbStub + kCountOff, 0, kPageBytes - kCountOff);
    BuildStub(g_lbStub, (uintptr_t)g_lbStub, (uint64_t)(g_base + kRvaBack), (uint64_t)(g_base + kRvaSkip));
    FlushInstructionCache(GetCurrentProcess(), g_lbStub, kPageBytes);
    g_lbLogged = 0;
    if (!InstallHook((void*)site, g_lbStub, &g_lbTramp, "listBoxLine(0x1fed8c)")) {
        g_lbTramp = nullptr;
        return;
    }
    Log("[listbox] guard on: a list-box line naming an item the box does not have is skipped "
        "instead of read (at +0x%llX, the crash at +0x%llX).",
        (unsigned long long)kRvaSite, (unsigned long long)kRvaFault);
}

// ---- +0x2A42F0: the 32-bit pointer ---------------------------------------------------
static uint8_t* g_p64Stub   = nullptr;
static void*    g_p64Tramp  = nullptr;
static uint32_t g_p64Logged = 0;

static void InstallPtr64Fix() {
    using namespace sessionguards::ptr64;
    if (!SsgMxbOnly("ptr64")) return;
    if (!ptr64fix_on.load()) { Log("[ptr64] fix off by config (ptr64fix=0)"); return; }
    if (g_p64Tramp || g_unloading.load()) return;
    {
        uint8_t *fb, *fe;
        const uint8_t* fixedEntry = (const uint8_t*)(g_base + kRvaJumpTable + 4 * kFixedJob);
        if (SsgCodeIs(kRvaFixedSite, kFixedSite, kFixedLen) && GetExecRange(g_base, &fb, &fe) &&
            fixedEntry >= fb && fixedEntry + 4 <= fe &&
            (uint32_t)sessionguards::Disp32(fixedEntry) == (uint32_t)kRvaFixedSite) {
            Log("[ptr64] not needed: this build loads job %d's pointer in 64 bits itself (fixed by "
                "the game in beta21f, at +0x%llX). Nothing patched.", kFixedJob,
                (unsigned long long)kRvaFixedSite);
            return;
        }
    }
    bool ok = SsgCodeIs(kRvaSite, kSite, kSiteLen) && SsgCodeIs(kRvaCallee, kCallee, sizeof(kCallee)) &&
              SsgCodeIs(kRvaDispatch, kDispatch, kDispatchLen);
    // And the dispatcher's table sends job 863 here.
    uint8_t *tb, *te;
    const uint8_t* entry = (const uint8_t*)(g_base + kRvaJumpTable + 4 * kJob);
    ok = ok && GetExecRange(g_base, &tb, &te) && entry >= tb && entry + 4 <= te &&
         (uint32_t)sessionguards::Disp32(entry) == (uint32_t)kRvaSite;
    if (!ok) {
        Log("[ptr64] fix not applied: the bytes at +0x%llX are not job 863 as we expect it "
            "(game updated?). Nothing patched.", (unsigned long long)kRvaSite);
        return;
    }
    const uintptr_t site = g_base + kRvaSite;
    if (!g_p64Stub) g_p64Stub = (uint8_t*)AllocNear(site, kPageBytes, PAGE_EXECUTE_READWRITE);
    if (!g_p64Stub) { Log("[ptr64] fix not applied: no page for the stub"); return; }
    memset(g_p64Stub, 0xCC, kPageBytes);
    memset(g_p64Stub + kCountOff, 0, kPageBytes - kCountOff);
    BuildStub(g_p64Stub, (uintptr_t)g_p64Stub, (uint64_t)(g_base + kRvaBack));
    FlushInstructionCache(GetCurrentProcess(), g_p64Stub, kPageBytes);
    g_p64Logged = 0;
    if (!InstallHook((void*)site, g_p64Stub, &g_p64Tramp, "job863Pointer(0x127109)")) {
        g_p64Tramp = nullptr;
        return;
    }
    Log("[ptr64] fix on: job 863 hands +0x%llX its pointer in 64 bits, not 32 (at +0x%llX). "
        "Launches with the stack below 4 GB see no change; above, this is the crash.",
        (unsigned long long)kRvaCallee, (unsigned long long)kRvaSite);
}

// ---- +0x7A860: the host's unfilled rider lists ----------------------------------------
using RiderInfoFn = int (*)(int id, void* out);
static RiderInfoFn            g_origRiderInfo = nullptr;
static std::atomic<uint32_t>  g_riCleared{0};

static int hkRiderInfo(int id, void* out) {
    const int rc = g_origRiderInfo(id, out);
    if (!sessionguards::riderinfo::ShouldClear(rc, out)) return rc;
    memset(out, 0, sessionguards::riderinfo::kOutBytes);
    const uint32_t n = g_riCleared.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sessionguards::ShouldLog(n)) {
        Log("[riderinfo] job 633 asked for rider %d, who is not in the list - the game leaves its "
            "answer unwritten and the host then copies lists of any length (the crash at "
            "mxbikes.exe+0x7A860). The answer was cleared: every list empty. %u so far.", id, n);
        frostmod::crash::Note("cleared the details of unknown rider %d (%u so far)", id, n);
    }
    ProtCatch(protection::Kind::RiderInfo);
    return rc;
}

static void InstallRiderInfoGuard() {
    using namespace sessionguards::riderinfo;
    if (!SsgMxbOnly("riderinfo")) return;
    if (!riderinfoguard_on.load()) { Log("[riderinfo] guard off by config (riderinfoguard=0)"); return; }
    if (g_origRiderInfo || g_unloading.load()) return;
    bool ok = SsgCodeIs(kRvaFn, kFn, kFnLen) && SsgCodeIs(kRvaCase, kCase, kCaseLen) &&
              SsgCodeIs(kRvaCaller, kCaller, kCallerLen);
    // The tail jump goes to the fill, and the dispatcher's table sends job 0x279 to the case.
    ok = ok && sessionguards::RipTarget(kFn + kTailJmpAt, kRvaFn + kTailJmpAt, 1, 5) == kRvaFill;
    uint8_t *tb, *te;
    const uint8_t* entry = (const uint8_t*)(g_base + kRvaJumpTable + 4 * kJob);
    ok = ok && GetExecRange(g_base, &tb, &te) && entry >= tb && entry + 4 <= te &&
         (uint32_t)sessionguards::Disp32(entry) == (uint32_t)kRvaCase;
    if (!ok) {
        Log("[riderinfo] guard not applied: the bytes at +0x%llX are not job 633 as we expect it "
            "(game updated?). Nothing patched.", (unsigned long long)kRvaFn);
        return;
    }
    if (!InstallHook((void*)(g_base + kRvaFn), (void*)&hkRiderInfo, (void**)&g_origRiderInfo,
                     "riderInfo(0x1b44c0)")) {
        g_origRiderInfo = nullptr;
        return;
    }
    Log("[riderinfo] guard on: details asked for a rider who is not in the list come back empty "
        "instead of unwritten (fn +0x%llX, the crash at +0x%llX / +0x%llX).",
        (unsigned long long)kRvaFn, (unsigned long long)kRvaFault, (unsigned long long)kRvaFault2);
}

// ---- +0x255F1F: a mipmap level with no memory behind it ----------------------------
using MipFilterFn = int (*)(const uint8_t* src, uint8_t* dst, int w, int h, int bpp);
static MipFilterFn           g_origMip[3] = {};
static std::atomic<uint32_t> g_mmSkipped{0};

static int MipSkip(const uint8_t* src, uint8_t* dst, int w, int h) {
    const uint32_t n = g_mmSkipped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sessionguards::ShouldLog(n)) {
        Log("[mipmap] skipped a %dx%d mipmap level with a NULL %s - the game's allocator gave it "
            "no memory (the crash at mxbikes.exe+0x255F1F / +0x255EF0). %u so far.",
            w, h, !src ? "source" : "destination", n);
        frostmod::crash::Note("skipped a %dx%d mip level with no memory (%u so far)", w, h, n);
    }
    ProtCatch(protection::Kind::MipMemory);
    return sessionguards::mipmap::kDone;
}
static int hkMipBox(const uint8_t* s, uint8_t* d, int w, int h, int b) {
    return sessionguards::mipmap::Admit(s, d) ? g_origMip[0](s, d, w, h, b) : MipSkip(s, d, w, h);
}
static int hkMipColumn(const uint8_t* s, uint8_t* d, int w, int h, int b) {
    return sessionguards::mipmap::Admit(s, d) ? g_origMip[1](s, d, w, h, b) : MipSkip(s, d, w, h);
}
static int hkMipRow(const uint8_t* s, uint8_t* d, int w, int h, int b) {
    return sessionguards::mipmap::Admit(s, d) ? g_origMip[2](s, d, w, h, b) : MipSkip(s, d, w, h);
}

static void InstallMipmapGuard() {
    using namespace sessionguards::mipmap;
    if (!SsgMxbOnly("mipmap")) return;
    if (!mipmapguard_on.load()) { Log("[mipmap] guard off by config (mipmapguard=0)"); return; }
    if (g_origMip[0] || g_unloading.load()) return;
    if (!SsgCodeIs(kRvaBox, kBox, kHeadLen) || !SsgCodeIs(kRvaColumn, kColumn, kHeadLen) ||
        !SsgCodeIs(kRvaRow, kRow, kHeadLen)) {
        Log("[mipmap] guard not applied: the mipmap filters are not the bytes we expect (game "
            "updated?). Nothing patched.");
        return;
    }
    const struct { uintptr_t rva; void* hk; const char* name; } fns[3] = {
        {kRvaBox, (void*)&hkMipBox, "mipBox(0x255ea0)"},
        {kRvaColumn, (void*)&hkMipColumn, "mipColumn(0x255dc0)"},
        {kRvaRow, (void*)&hkMipRow, "mipRow(0x255e30)"},
    };
    for (int i = 0; i < 3; ++i) {
        if (!InstallHook((void*)(g_base + fns[i].rva), fns[i].hk, (void**)&g_origMip[i], fns[i].name)) {
            g_origMip[i] = nullptr;
            Log("[mipmap] guard only partly applied: %s did not hook.", fns[i].name);
        }
    }
    Log("[mipmap] guard on: a mipmap level the game got no memory for is skipped instead of "
        "written to address 0 (the crash at +0x%llX).", (unsigned long long)kRvaFaultWrite);
}

// ---- +0x715F3: event settings with no event --------------------------------------------
static uint8_t*        g_ecStub   = nullptr;   // kept after unload: a thread may be on its way through
static void*           g_ecTramp  = nullptr;   // MinHook's; unused, the stub jumps back itself
static uint32_t        g_ecLogged = 0;
static const int32_t*  g_ecMode   = nullptr;   // the event type the getter reads

static void InstallEventCfgGuard() {
    using namespace sessionguards::eventcfg;
    if (!SsgMxbOnly("eventcfg")) return;
    if (!eventcfgguard_on.load()) { Log("[eventcfg] guard off by config (eventcfgguard=0)"); return; }
    if (g_ecTramp || g_unloading.load()) return;
    if (!SsgCodeIs(kRvaFn, kFn, kFnLen) || !SsgCodeIs(kRvaCallWin, kCallWin, kCallWinLen)) {
        Log("[eventcfg] guard not applied: the bytes at +0x%llX are not the event-settings "
            "getter we expect (game updated?). Nothing patched.", (unsigned long long)kRvaFn);
        return;
    }
    g_ecMode = (const int32_t*)SsgCell((const uint8_t*)(g_base + kRvaFn + kModeLoadAt),
                                       kRvaFn + kModeLoadAt, 2, 6, sizeof(int32_t));
    const uintptr_t site = g_base + kRvaSite;
    if (!g_ecStub) g_ecStub = (uint8_t*)AllocNear(site, kPageBytes, PAGE_EXECUTE_READWRITE);
    if (!g_ecStub) { Log("[eventcfg] guard not applied: no page for the stub"); return; }
    memset(g_ecStub, 0xCC, kPageBytes);
    memset(g_ecStub + kCountOff, 0, kPageBytes - kCountOff);
    BuildStub(g_ecStub, (uintptr_t)g_ecStub, (uint64_t)(g_base + kRvaBack));
    FlushInstructionCache(GetCurrentProcess(), g_ecStub, kPageBytes);
    g_ecLogged = 0;
    if (!InstallHook((void*)site, g_ecStub, &g_ecTramp, "eventCfgNull(0x5ab31)")) {
        g_ecTramp = nullptr;
        return;
    }
    Log("[eventcfg] guard on: event settings asked for with no event set get an empty block "
        "instead of NULL (getter +0x%llX, the crash at +0x%llX).",
        (unsigned long long)kRvaFn, (unsigned long long)kRvaFault);
}

/// A stub's counter crossed a line the rate limit allows (or is the first).
static bool SsgDue(const uint8_t* page, size_t off, uint32_t* logged, uint32_t* now) {
    if (!page) return false;
    const uint32_t n = *(volatile const uint32_t*)(page + off);
    if (n == *logged) return false;
    const uint32_t prev = *logged;
    *logged = n;
    *now = n;
    bool due = prev == 0;
    for (uint64_t k = (uint64_t)prev + 1; !due && k <= n && k <= (uint64_t)prev + 64; ++k)
        due = sessionguards::ShouldLog(k);
    return due || n - prev > 64;
}

static void SessionGuardsTick() {
    uint32_t n = 0;
    if (g_lbStub && *(volatile const uint32_t*)(g_lbStub + sessionguards::listbox::kCountOff) != g_lbLogged)
        ProtCatch(protection::Kind::ListBox);
    if (SsgDue(g_lbStub, sessionguards::listbox::kCountOff, &g_lbLogged, &n)) {
        using namespace sessionguards::listbox;
        const uint32_t item = *(volatile const uint32_t*)(g_lbStub + kItemOff);
        const uint32_t text = *(volatile const uint32_t*)(g_lbStub + kTextOff);
        const void* obj = *(void* volatile const*)(g_lbStub + kObjOff);
        Log("[listbox] skipped a list-box line naming item %d (text offset %u) in box %p - the "
            "crash at mxbikes.exe+0x1FED9E. That line is not drawn this frame. %u so far.",
            (int)item, text, obj, n);
        frostmod::crash::Note("skipped a list-box line, item %d (%u so far)", (int)item, n);
    }
    if (g_p64Stub && *(volatile const uint32_t*)(g_p64Stub + sessionguards::ptr64::kCountOff) != g_p64Logged)
        ProtCatch(protection::Kind::StackPointer);
    if (SsgDue(g_p64Stub, sessionguards::ptr64::kCountOff, &g_p64Logged, &n)) {
        const uint64_t p = *(volatile const uint64_t*)(g_p64Stub + sessionguards::ptr64::kLastOff);
        Log("[ptr64] this launch's stack is above 4 GB: job 863 passed 0x%llX whole, where the "
            "game alone would have passed 0x%llX and crashed at mxbikes.exe+0x2A42F0. %u call(s) "
            "so far.", (unsigned long long)p, (unsigned long long)(uint32_t)p, n);
        if (n == 1) frostmod::crash::Note("job 863 pointer above 4 GB, passed whole");
    }
    if (g_ecStub && *(volatile const uint32_t*)(g_ecStub + sessionguards::eventcfg::kCountOff) != g_ecLogged)
        ProtCatch(protection::Kind::EventSettings);
    if (SsgDue(g_ecStub, sessionguards::eventcfg::kCountOff, &g_ecLogged, &n)) {
        const int mode = g_ecMode ? SafeReadInt(g_ecMode) : -1;
        Log("[eventcfg] the menu asked for the event settings with no event set (event type %d): "
            "it got an empty block instead of NULL - the crash at mxbikes.exe+0x715F3. %u so far.",
            mode, n);
        frostmod::crash::Note("event settings asked for with no event (%u so far)", n);
    }
}

// ===========================================================================
// LOOKUP GUARD  -  empty lookups, a NaN animation phase and two NULL objects (lookupguard.h)
//
// 45 "nothing found" exits whose index is clamped to the list (lookupguard=0), the animation
// sampler behind +0x2686C0 (animguard=0), and two NULL reads given the game's own skip and
// failure paths (lookupguard=0). Each site's bytes are checked before anything is written; a
// site that does not match is left alone and the rest still go in. Hand-written patches like
// the tyre fallback's, reverted at unload; the stubs only jump back into the game, so the page
// is kept after unload for a thread that may be on its way through.
// ===========================================================================
static uint8_t*  g_lgPage = nullptr;
static bool      g_lgOn[lookupguard::kSlots] = {};
static uint32_t  g_lgLogged[lookupguard::kSlots] = {};

/// Write `len` bytes of `patch` over the game at `rva` (or put `orig` back). One site.
static bool LgWrite(uint32_t rva, const uint8_t* bytes, size_t len) {
    DWORD prot = 0;
    void* p = (void*)(g_base + rva);
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &prot)) return false;
    memcpy(p, bytes, len);
    VirtualProtect(p, len, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    return true;
}

static void LookupGuardRevert() {
    using namespace lookupguard;
    for (size_t i = 0; i < kSlots; ++i) {
        if (!g_lgOn[i]) continue;
        if (i < kClampCount) LgWrite(kClamps[i].rva, kClamps[i].bytes, kClamps[i].len);
        else if (i == kAnimSlot) LgWrite(kAnimSite, kAnimWinBytes + (kAnimSite - kAnimWin), kAnimLen);
        else if (i == kNull1Slot) LgWrite(kNull1Site, kNull1Bytes, kNull1Len);
        else if (i == kNull2Slot) LgWrite(kNull2Site, kNull2Bytes, kNull2Len);
        g_lgOn[i] = false;
    }
}

static void InstallLookupGuard() {
    using namespace lookupguard;
    if (!SsgMxbOnly("lookup")) return;
    const bool lookups = lookupguard_on.load(), anim = animguard_on.load();
    if (!lookups) Log("[lookup] guard off by config (lookupguard=0)");
    if (!anim) Log("[anim] guard off by config (animguard=0)");
    if ((!lookups && !anim) || g_unloading.load()) return;
    for (size_t i = 0; i < kSlots; ++i) if (g_lgOn[i]) return;   // already in
    auto* dos = (IMAGE_DOS_HEADER*)g_base;
    auto* nt  = (IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew);
    const uint32_t image = nt->OptionalHeader.SizeOfImage;
    if (!g_lgPage) g_lgPage = (uint8_t*)AllocNear(g_base + 0x150000, kPageBytes, PAGE_EXECUTE_READWRITE);
    if (!g_lgPage) { Log("[lookup] guard not applied: no page for the stubs"); return; }
    memset(g_lgPage, 0xCC, kCountBase);
    memset(g_lgPage + kCountBase, 0, kPageBytes - kCountBase);
    memset(g_lgLogged, 0, sizeof(g_lgLogged));
    auto counter = [&](size_t slot) { return (uint64_t)(uintptr_t)(g_lgPage + kCountBase + 4 * slot); };
    auto stub = [&](size_t slot) { return g_lgPage + slot * kSlot; };
    auto place = [&](size_t slot, uint32_t rva, size_t len) {
        uint8_t patch[16];
        if (!BuildSitePatch(g_base + rva, (uintptr_t)stub(slot), len, patch)) return false;
        if (!LgWrite(rva, patch, len)) return false;
        g_lgOn[slot] = true;
        return true;
    };
    unsigned clamps = 0, missed = 0;
    if (lookups) {
        for (size_t i = 0; i < kClampCount; ++i) {
            const ClampSite& c = kClamps[i];
            bool ok = SsgCodeIs(c.rva, c.bytes, c.len) && c.count + 4 <= image;
            if (ok && c.cmpRva) {
                const uint8_t* cmp = (const uint8_t*)(g_base + c.cmpRva);
                ok = SsgCodeIs(c.cmpRva, cmp, c.cmpLen) &&
                     sessionguards::RipTarget(cmp, c.cmpRva, c.cmpLen - 4, c.cmpLen) == c.count;
            }
            if (!ok) { ++missed; continue; }
            BuildClamp(stub(i), (uintptr_t)stub(i), g_base, c, counter(i));
            if (place(i, c.rva, c.len)) ++clamps; else ++missed;
        }
        FlushInstructionCache(GetCurrentProcess(), g_lgPage, kCountBase);
        if (SsgCodeIs(kNull1Site, kNull1Bytes, kNull1Len) && SsgCodeIs(kNull1CallRva, kNull1Call, sizeof(kNull1Call))) {
            BuildNull1(stub(kNull1Slot), (uintptr_t)stub(kNull1Slot), g_base, counter(kNull1Slot));
            FlushInstructionCache(GetCurrentProcess(), stub(kNull1Slot), kSlot);
            place(kNull1Slot, kNull1Site, kNull1Len);
        }
        if (SsgCodeIs(kNull2Site, kNull2Bytes, kNull2Len) && SsgCodeIs(kNull2LoadRva, kNull2Load, sizeof(kNull2Load))) {
            BuildNull2(stub(kNull2Slot), (uintptr_t)stub(kNull2Slot), g_base, counter(kNull2Slot));
            FlushInstructionCache(GetCurrentProcess(), stub(kNull2Slot), kSlot);
            place(kNull2Slot, kNull2Site, kNull2Len);
        }
        if (clamps)
            Log("[lookup] guard on: %u name lookup(s) that find nothing now use the list's first entry "
                "instead of an index the game never set (the .roc track lookup at +0x62E66, tyres, "
                "series, bikes)%s.", clamps, missed ? "" : "; every site matched");
        if (missed)
            Log("[lookup] %u lookup site(s) are not the bytes we expect (game updated?) and were left "
                "alone.", missed);
        Log("[lookup] NULL checks: race-start item +0x2544A3 %s, interface call +0x290371 %s.",
            g_lgOn[kNull1Slot] ? "on" : "not applied", g_lgOn[kNull2Slot] ? "on" : "not applied");
    }
    if (anim) {
        if (SsgCodeIs(kAnimWin, kAnimWinBytes, kAnimWinLen)) {
            BuildAnim(stub(kAnimSlot), (uintptr_t)stub(kAnimSlot), g_base, counter(kAnimSlot));
            FlushInstructionCache(GetCurrentProcess(), stub(kAnimSlot), kSlot);
            if (place(kAnimSlot, kAnimSite, kAnimLen))
                Log("[anim] guard on: an animation channel with a NaN phase is sampled at its first "
                    "key instead of key INT_MIN (at +0x%X, the crash at +0x%X).", kAnimSite, kAnimFault);
        } else {
            Log("[anim] guard not applied: the bytes at +0x%X are not the animation sampler we "
                "expect (game updated?). Nothing patched.", kAnimWin);
        }
    }
}

static void LookupGuardTick() {
    using namespace lookupguard;
    if (!g_lgPage) return;
    for (size_t i = 0; i < kSlots; ++i) {
        uint32_t n = 0;
        if (!SsgDue(g_lgPage, kCountBase + 4 * i, &g_lgLogged[i], &n)) continue;
        if (i < kClampCount) {
            ProtCatch(protection::Kind::BadText);
            Log("[lookup] a %s lookup at +0x%X found no match and its index was never set: used "
                "the first %s instead. %u so far.", ListName(kClamps[i].stride), kClamps[i].rva,
                ListName(kClamps[i].stride), n);
            frostmod::crash::Note("empty %s lookup at +0x%X (%u so far)", ListName(kClamps[i].stride),
                                  kClamps[i].rva, n);
        } else if (i == kAnimSlot) {
            ProtCatch(protection::Kind::PhysicsNan);
            Log("[anim] an animation channel had a NaN phase: sampled at its first key and the phase "
                "set to 0, instead of the crash at mxbikes.exe+0x%X. %u so far.", kAnimFault, n);
            frostmod::crash::Note("NaN animation phase reset (%u so far)", n);
        } else {
            ProtCatch(protection::Kind::NullObject);
            const uint32_t site = i == kNull1Slot ? kNull1Site : kNull2Site;
            Log("[lookup] a NULL object at +0x%X took the game's own %s path instead of being read. "
                "%u so far.", site, i == kNull1Slot ? "skip-this-item" : "failure", n);
            frostmod::crash::Note("NULL object skipped at +0x%X (%u so far)", site, n);
        }
    }
}

// ---- beta21f "Cheat - Bike Setup" on every joiner (setupguard.h) -------------------------
// Only runs when this game hosts (the setup check is the server's). Same rule as FrostServer:
// a setup the game rejects is re-checked field by field, and it passes only when every failure
// is a tyre size checked against a size list the host built empty.
using SgValidateFn = int (*)(const uint8_t* def, const uint8_t* setup);
using SgSetupJobFn = int (*)(int world, const uint8_t* setup);
static SgValidateFn g_origSgValidate = nullptr;
static SgSetupJobFn g_origSgSetupJob = nullptr;
static thread_local bool t_sgValidatorRan = false;
static std::atomic<uint32_t> g_sgPassed{0}, g_sgKept{0};

static void SgLogFailures(const setupguard::Failure* f, size_t n, const char* verdict) {
    char line[1024];
    int at = _snprintf_s(line, sizeof(line), _TRUNCATE, "[setupguard] host rejected a rider's setup (%s):", verdict);
    for (size_t i = 0; i < n && i < 12 && at > 0 && at < (int)sizeof(line) - 80; ++i) {
        const auto& fd = setupguard::kFields[f[i].field];
        const int w = f[i].gear >= 0
            ? _snprintf_s(line + at, sizeof(line) - at, _TRUNCATE, " gear%d(+0x%X)=%d max %d",
                          f[i].gear, fd.setup + 4 * f[i].gear, f[i].value, f[i].limit)
            : _snprintf_s(line + at, sizeof(line) - at, _TRUNCATE, " %s(+0x%X)=%d max %d [def+0x%X]",
                          fd.name, fd.setup, f[i].value, f[i].limit, fd.limit);
        if (w > 0) at += w;
    }
    Log("%s", line);
}

static size_t SgEvaluate(const uint8_t* def, const uint8_t* setup, setupguard::Failure* f, size_t cap, bool* ok) {
    __try { *ok = true; return setupguard::Evaluate(def, setup, f, cap); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *ok = false; return 0; }
}

static int hkSgValidate(const uint8_t* def, const uint8_t* setup) {
    t_sgValidatorRan = true;
    const int rc = g_origSgValidate(def, setup);
    if (rc == 0 || !def || !setup) return rc;
    setupguard::Failure f[48];
    bool ok = false;
    size_t n = SgEvaluate(def, setup, f, 48, &ok);
    if (!ok) { Log("[setupguard] could not read the setup or the bike; the game's ban stands"); return rc; }
    if (n > 48) n = 48;
    if (setupguard::OnlyBroken(f, n)) {
        const uint32_t k = g_sgPassed.fetch_add(1) + 1;
        if (sessionguards::ShouldLog(k)) SgLogFailures(f, n, "beta21f empty tyre-size list, let through");
        return 0;
    }
    const uint32_t k = g_sgKept.fetch_add(1) + 1;
    if (sessionguards::ShouldLog(k))
        SgLogFailures(f, n, n ? "out of range, the game's ban stands" : "no field out of range by our table");
    return rc;
}

static int hkSgSetupJob(int world, const uint8_t* setup) {
    t_sgValidatorRan = false;
    const int rc = g_origSgSetupJob(world, setup);
    if (rc != 0 && !t_sgValidatorRan)
        Log("[setupguard] no bike found for world %d; the game bans this rider for Bike Setup", world);
    return rc;
}

static void InstallSetupBanGuard() {
    using namespace setupguard;
    if (!SsgMxbOnly("setupguard")) return;
    if (!setupguard_on.load()) { Log("[setupguard] off by config (setupguard=0)"); return; }
    if (g_origSgValidate || g_unloading.load()) return;
    if (!SsgCodeIs(kRvaValidator, kValidator, kValidatorLen) || !SsgCodeIs(kRvaSetupJob, kSetupJob, kSetupJobLen)) {
        Log("[setupguard] not applied: the setup check is not beta21f's (another game build). Nothing patched.");
        return;
    }
    if (!InstallHook((void*)(g_base + kRvaValidator), (void*)&hkSgValidate, (void**)&g_origSgValidate,
                     "setupValidate(0x198a90)")) {
        g_origSgValidate = nullptr;
        return;
    }
    if (!InstallHook((void*)(g_base + kRvaSetupJob), (void*)&hkSgSetupJob, (void**)&g_origSgSetupJob,
                     "setupJob(0x1684d0)")) {
        g_origSgSetupJob = nullptr;   // the validator hook alone still applies the fix
    }
    Log("[setupguard] on: when this game hosts, a tyre size checked against an empty size list no "
        "longer bans the rider for \"Cheat - Bike Setup\" (beta21f, +0x%llX)", (unsigned long long)kRvaValidator);
}

static void InstallSessionGuardsImpl() {
    InstallBikeListGuard();
    InstallWakeGuard();
    InstallListBoxGuard();
    InstallPtr64Fix();
    InstallRiderInfoGuard();
    InstallMipmapGuard();
    InstallEventCfgGuard();
    InstallLookupGuard();
    InstallSetupBanGuard();
}

// ===========================================================================
// NANRECOVER  -  a bike whose physics state went NaN gets the game's own reset (on by default)
//
// src/nanrecover.h has the evidence and the reasoning. Short version: the physics NaN is 46% of
// the MX Bikes crashes FrostMod collects, the bike goes from calm to NaN inside one sim step,
// and the bike code reads it in the next one and faults (+0x1A29AE and friends). The terrain
// and shock guards each turn away one reader; this takes the NaN away from all of them.
//
// After each sim step (and once before it), every active bike in the slot is checked with
// integer tests on its floats. A finite bike costs the check and nothing else: no write, no
// call. A non-finite one is handed to place-bike 0x197060 - the function the game's own
// respawn runs - with the main body's position and heading from a snapshot taken while it was
// still finite and sane. Then it is checked again and the result is logged ("[nanrecover]")
// and noted in the crash trail, so a crash that still follows says what was tried.
//
// Runs on the physics thread, between steps, where the game's own respawn job also runs. Every
// read of the game's memory is under SEH; one fault turns this off for the session.
// ===========================================================================
static const uint8_t* g_nrWorlds = nullptr;   // the sim step's world slot array
static nanrecover::ResetFn g_nrReset = nullptr;
static nanrecover::Track g_nrTracks[8];        // per bike, most recently seen
static std::atomic<uint32_t> g_nrResets{0};
static std::atomic<uint32_t> g_nrFailed{0};
static constexpr unsigned kNrTimedSteps = 5000;

struct NrFound {
    uint8_t* bike;
    int32_t handle;
    nanrecover::Bad bad;
};

static nanrecover::Track* NrTrackFor(const uint8_t* bike, int32_t handle, float t) {
    nanrecover::Track* spare = nullptr;
    for (auto& tr : g_nrTracks) {
        if (tr.bike == bike) {
            // Another bike at the same address, or the same one spawned again (its clock went
            // back): start over. A clock that is not a number says nothing either way.
            if (tr.handle != handle || (t == t && t < tr.lastTime)) tr.Clear(bike, handle, t);
            if (t == t) tr.lastTime = t;
            return &tr;
        }
        if (!spare && !tr.bike) spare = &tr;
    }
    if (!spare) {   // a full table: drop the one with the least history
        spare = &g_nrTracks[0];
        for (auto& tr : g_nrTracks)
            if (tr.steps < spare->steps) spare = &tr;
    }
    spare->Clear(bike, handle, t == t ? t : 0.0f);
    return spare;
}

// No C++ objects with destructors: the caller wraps it in __try. Fills up to `max` bikes that
// failed the check; snapshots the ones that passed (after the step only).
static int NrScanRaw(int32_t slot, bool before, NrFound* found, int max) {
    if (slot < 1 || slot > 3 || !g_nrWorlds) return 0;
    const uint8_t* world = g_nrWorlds + (size_t)(slot - 1) * tyrelog::kWorldStride;
    const uint8_t* list = nullptr;
    memcpy(&list, world + tyrelog::kWorldBikeList, sizeof(list));
    const int n = tyrelog::ListCount(list);
    if (n <= 0 || n > tyrelog::kMaxBikes) return 0;
    int k = 0;
    for (int i = 0; i < n; ++i) {
        uint8_t* bike = (uint8_t*)tyrelog::ListItem(list, i);
        if (!bike) continue;
        int32_t on = 0;
        memcpy(&on, bike + nanrecover::kBikeActive, 4);
        if (on == 0) continue;   // the bike sim skips it, and so does this
        nanrecover::Bad bad;
        if (!nanrecover::Scan(bike, &bad)) {
            if (k < max) {
                found[k].bike = bike;
                memcpy(&found[k].handle, bike + nanrecover::kBikeHandle, 4);
                found[k].bad = bad;
                ++k;
            }
            continue;
        }
        if (before) continue;
        int32_t handle = 0;
        float t = 0;
        memcpy(&handle, bike + nanrecover::kBikeHandle, 4);
        memcpy(&t, bike + nanrecover::kBikeTime, 4);
        const uint8_t* body = nullptr;
        memcpy(&body, bike + nanrecover::kBodyMain, sizeof(body));
        if (!body) continue;
        float pos[3], lvel[3], avel[3], R[12];
        memcpy(pos, body + nanrecover::kBodyPos, sizeof(pos));
        memcpy(lvel, body + nanrecover::kBodyLvel, sizeof(lvel));
        memcpy(avel, body + nanrecover::kBodyAvel, sizeof(avel));
        memcpy(R, body + nanrecover::kBodyR, sizeof(R));
        nanrecover::Track* tr = NrTrackFor(bike, handle, t);
        tr->SawCounter(bike[nanrecover::kBikeResets]);
        tr->Passed(pos, lvel, avel, R);
    }
    return k;
}

static int NrScan(int32_t slot, bool before, NrFound* found, int max, bool* faulted) {
    *faulted = false;
    __try {
        return NrScanRaw(slot, before, found, max);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
        return 0;
    }
}

// The forces the step accumulated on the bike's bodies: zero between steps in a stock game
// (ODE clears them at the end of each step), so clearing them before a reset only matters when
// a NaN got into them from outside.
static void NrClearForcesRaw(uint8_t* bike) {
    const uint8_t* body[7];
    uint32_t off[7];
    nanrecover::Bodies(bike, body, off);
    for (const uint8_t* b : body)
        if (b) {
            memset((void*)(b + nanrecover::kBodyFacc), 0, 12);
            memset((void*)(b + nanrecover::kBodyTacc), 0, 12);
        }
}

// place-bike under SEH. -1: it faulted; otherwise what it returned (0 placed, 1 no such bike).
static int NrPlace(uint8_t* bike, int32_t handle, const nanrecover::Snap& s) {
    __try {
        NrClearForcesRaw(bike);
        // (handle, x, z, heading, y): y is where its ground query starts, so a little above the
        // body, never under the surface it was on.
        return g_nrReset(handle, s.pos[0], s.pos[2], s.heading, s.pos[1] + 1.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static bool NrRescan(uint8_t* bike, nanrecover::Bad* bad) {
    __try {
        return nanrecover::Scan(bike, bad);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void NrRecover(const NrFound& f, bool before) {
    using namespace nanrecover;
    float t = 0;
    SafeReadBytes((const char*)(f.bike + kBikeTime), (char*)&t, sizeof(t));
    Track* tr = NrTrackFor(f.bike, f.handle, t);
    const uint32_t minAge = tr->MinAgeForNext();
    const Snap* s = tr->Pick(minAge);
    const uint32_t n = g_nrResets.load(std::memory_order_relaxed) + 1;
    const char* what = FieldName(f.bad.field);
    char where[48] = "";
    if (f.bad.field >= Field::BodyPos && f.bad.field <= Field::BodyAvel)
        _snprintf_s(where, sizeof(where), _TRUNCATE, " of body [bike+0x%X]", f.bad.where);
    else if (f.bad.field >= Field::WheelSpin && f.bad.field <= Field::WheelDeflVel)
        _snprintf_s(where, sizeof(where), _TRUNCATE, " of wheel %u", f.bad.where);
    if (!s) {
        const uint32_t m = g_nrFailed.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ShouldLog(m))
            Log("[nanrecover] bike %p (handle %d) has a non-finite %s%s (%08X) %s the step, "
                "%.3f s after its spawn, and no finite position to put it back at yet. Left to "
                "the terrain and shock guards. %u so far.",
                (void*)f.bike, f.handle, what, where, f.bad.bits, before ? "before" : "after",
                (double)t, m);
        if (ShouldLog(m))
            frostmod::crash::Note("nanrecover: NaN %s%s, no good position to reset to", what, where);
        return;
    }
    const float age = (float)(tr->steps - s->step) * 0.002f;
    const int rc = NrPlace(f.bike, f.handle, *s);
    tr->DidReset();
    uint8_t counter = 0;
    if (SafeReadBytes((const char*)(f.bike + kBikeResets), (char*)&counter, 1) == 1)
        tr->PlacedByUs(counter);
    g_nrResets.store(n, std::memory_order_relaxed);
    ProtCatch(protection::Kind::PhysicsNan);
    Bad after;
    const bool clean = rc == 0 && NrRescan(f.bike, &after);
    const bool say = ShouldLog(n);
    if (say)
        Log("[nanrecover] bike %p (handle %d): non-finite %s%s (%08X) %s the step, %.3f s after "
            "its spawn. Reset with the game's place-bike to (%.2f, %.2f, %.2f) heading %.1f, "
            "where it was %.2f s before: %s. %u reset(s) this run, %u for this bike.",
            (void*)f.bike, f.handle, what, where, f.bad.bits, before ? "before" : "after",
            (double)t, (double)s->pos[0], (double)s->pos[1], (double)s->pos[2],
            (double)s->heading, (double)age,
            rc < 0 ? "place-bike FAULTED" : rc != 0 ? "place-bike did not know the bike"
            : clean ? "finite again" : "STILL non-finite",
            n, tr->resets);
    if (rc < 0) {
        // Whatever place-bike tripped on, it will trip on again: stop calling it.
        g_nrLive.store(false, std::memory_order_relaxed);
        Log("[nanrecover] off for the rest of this session (place-bike faulted). The terrain and "
            "shock guards still run.");
    } else if (say && !clean && rc == 0) {
        Log("[nanrecover] still non-finite after the reset: %s (%08X). It is tried again next "
            "step, from further back.", FieldName(after.field), after.bits);
    }
    // The trail holds 32 notes; a bike that needs a reset every step must not push out what
    // came before it.
    if (say || rc < 0)
        frostmod::crash::Note("nanrecover: NaN %s%s, bike reset to %.0f,%.0f,%.0f (%s, %u so far)",
                              what, where, (double)s->pos[0], (double)s->pos[1],
                              (double)s->pos[2], rc < 0 ? "faulted" : clean ? "ok" : "still NaN", n);
}

static void NrCheck(int32_t slot, bool before) {
    // The cost, measured on the player's own machine: both checks of the first few thousand
    // steps are timed and the mean goes in the log once.
    static unsigned timed = 0;
    static long long ticks = 0;
    LARGE_INTEGER a{}, b{};
    const bool timing = timed < kNrTimedSteps;
    if (timing) QueryPerformanceCounter(&a);
    NrFound found[4];
    bool faulted = false;
    const int k = NrScan(slot, before, found, 4, &faulted);
    if (timing) {
        QueryPerformanceCounter(&b);
        ticks += b.QuadPart - a.QuadPart;
        if (++timed == kNrTimedSteps) {
            LARGE_INTEGER fq{};
            QueryPerformanceFrequency(&fq);
            const double ns =
                fq.QuadPart ? (double)ticks * 1e9 / (double)fq.QuadPart / kNrTimedSteps : 0.0;
            Log("[nanrecover] the check costs %.0f ns (mean of %u; it runs twice a 2 ms step).",
                ns, kNrTimedSteps);
        }
    }
    if (faulted) {
        g_nrLive.store(false, std::memory_order_relaxed);
        Log("[nanrecover] a read of the bike list faulted - off for the rest of this session.");
        return;
    }
    for (int i = 0; i < k && g_nrLive.load(std::memory_order_relaxed); ++i) NrRecover(found[i], before);
}

// Before the sim step hook goes in. `fn` is the sim step as found. Every RVA here is beta21e's
// and is checked byte for byte; one mismatch and nanrecover stays off (the guards still run).
static bool NrInstall(const uint8_t* fn) {
    if (g_unloading.load()) return false;
    if (fn != (const uint8_t*)(g_base + mxb::RVA_SIM_STEP)) {
        Log("[nanrecover] off: the sim step moved, so this is not the build its offsets are for.");
        return false;
    }
    uint8_t *begin, *end;
    if (!GetExecRange(g_base, &begin, &end)) { Log("[nanrecover] off: no .text range."); return false; }
    auto check = [&](uint32_t rva, const char* bytes, int len, const char* what) {
        const uint8_t* p = (const uint8_t*)(g_base + rva);
        if (p < begin || p + len > end || memcmp(p, bytes, (size_t)len) != 0) {
            Log("[nanrecover] off: %s does not match at RVA 0x%x (game updated?). The terrain "
                "and shock guards still run.", what, rva);
            return false;
        }
        return true;
    };
    for (const nanrecover::Check& c : nanrecover::kChecks)
        if (!check(c.rva, c.bytes, c.len, c.what)) return false;
    // The bike list and world array, as tyrelog reads them.
    for (const tyrelog::Check& c : tyrelog::kChecks)
        if (!check(c.rva, c.bytes, c.len, c.what)) return false;
    int32_t rel = 0;
    memcpy(&rel, fn + tyrelog::kSimStepLeaAt + 3, 4);
    const uint8_t* worlds = fn + tyrelog::kSimStepLeaAt + 7 + rel;
    auto dos = (const IMAGE_DOS_HEADER*)g_base;
    auto nt = (const IMAGE_NT_HEADERS*)((const uint8_t*)g_base + dos->e_lfanew);
    const uint8_t* lo = (const uint8_t*)g_base;
    const uint8_t* hi = lo + nt->OptionalHeader.SizeOfImage;
    if (worlds < lo || worlds + 3 * tyrelog::kWorldStride > hi) {
        Log("[nanrecover] off: the world array the sim step points at is outside the exe.");
        return false;
    }
    g_nrWorlds = worlds;
    g_nrReset = (nanrecover::ResetFn)(g_base + nanrecover::kRvaReset);
    return true;
}

using SimStepFn = int32_t (*)(int32_t, float);
static SimStepFn g_origNanRecoverStep = nullptr;

static int32_t hkNanRecoverStep(int32_t slot, float dt) {
    const bool live = g_nrLive.load(std::memory_order_relaxed);
    if (live) NrCheck(slot, true);
    const int32_t rc = g_origNanRecoverStep(slot, dt);
    if (live) NrCheck(slot, false);
    return rc;
}

static const uint8_t* ResolveSimStepImpl(intptr_t delta, const char* tag) {
    uint8_t *begin = nullptr, *end = nullptr;
    if (!GetExecRange(g_base, &begin, &end)) {
        Log("[%s] off: can't read the module's sections to place it.", tag);
        return nullptr;
    }
    uint8_t* fn = reinterpret_cast<uint8_t*>(g_base + mxb::RVA_SIM_STEP + delta);
    if (fn < begin || fn >= end || !MatchAt(fn, mxb::SIG_SIM_STEP, mxb::SIG_SIM_STEP_MASK)) {
        uint8_t* found = PatternScan(begin, end, mxb::SIG_SIM_STEP, mxb::SIG_SIM_STEP_MASK);
        if (!found) {
            Log("[%s] off: the sim step's bytes are not in this build's .text (game "
                "updated?). Re-derive RVA_SIM_STEP before turning it on again.", tag);
            return nullptr;
        }
        Log("[%s] sim step RELOCATED: found at RVA 0x%zx (offsets.h says 0x%zx).",
            tag, static_cast<size_t>(reinterpret_cast<uintptr_t>(found) - g_base),
            static_cast<size_t>(mxb::RVA_SIM_STEP));
        fn = found;
    }
    if (mxb::LooksDetoured(fn)) {
        Log("[%s] off: the sim step is already hooked (%02X %02X %02X %02X %02X).",
            tag, fn[0], fn[1], fn[2], fn[3], fn[4]);
        return nullptr;
    }
    return fn;
}

// Verify, decode, hook. MX Bikes only: both the RVA and the signature are mxbikes.exe's.
static void InstallGhsGuard(intptr_t delta) {
    if (!g_game->offsets_complete) {
        Log("[ghs] guard off for %s - the close function is MX Bikes' and has no twin here.",
            g_game->display);
        return;
    }
    uint8_t *begin, *end;
    if (!GetExecRange(g_base, &begin, &end)) {
        Log("[ghs] guard off: can't read the module's sections to place it.");
        return;
    }
    uint8_t* fn = (uint8_t*)(g_base + mxb::RVA_GHS_CLOSE + delta);
    // Somebody hooked it before us - a second copy of FrostMod in this process, or another
    // mod. Say so and leave it alone: the signature cannot match a patched prologue, and
    // the scan below would then blame a game update for a detour sitting in front of us.
    if (fn >= begin && fn < end && mxb::LooksDetoured(fn)) {
        Log("[ghs] guard off: 0x%zx is already hooked (%02X %02X %02X %02X %02X). Another "
            "copy of FrostMod is loaded, or another mod got there first. Leaving their hook "
            "alone - two detours on one function is how you make this worse.",
            (size_t)(fn - g_base), fn[0], fn[1], fn[2], fn[3], fn[4]);
        return;
    }
    if (fn < begin || fn >= end || !MatchAt(fn, mxb::SIG_GHS_CLOSE, mxb::SIG_GHS_CLOSE_MASK)) {
        uint8_t* found = PatternScan(begin, end, mxb::SIG_GHS_CLOSE, mxb::SIG_GHS_CLOSE_MASK);
        if (!found) {
            Log("[ghs] guard off: the close signature is not in this build's .text. If the "
                "game updated, re-derive RVA_GHS_CLOSE before trusting the old address.");
            return;
        }
        Log("[ghs] close RELOCATED: found at RVA 0x%zx (offsets.h says 0x%zx).",
            (size_t)(found - g_base), (size_t)mxb::RVA_GHS_CLOSE);
        fn = found;
    }

    // The slot table from the function's own RIP-relative lea: a .data RVA does not move
    // by the .text delta, so decoding is the only way to still be right after an update.
    const int32_t disp = *(const int32_t*)(fn + mxb::GHS_LEA_DISP_OFF);
    uint8_t* table = fn + mxb::GHS_LEA_END_OFF + disp;
    auto* dos = (IMAGE_DOS_HEADER*)g_base;
    auto* nt  = (IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew);
    uint8_t* imageEnd = (uint8_t*)g_base + nt->OptionalHeader.SizeOfImage;
    if (table < (uint8_t*)g_base || table + mxb::GHS_SLOTS * sizeof(void*) > imageEnd) {
        Log("[ghs] guard off: the decoded slot table (RVA 0x%zx) is outside the module.",
            (size_t)(table - g_base));
        return;
    }
    if ((size_t)(table - g_base) != mxb::RVA_GHS_TABLE)
        Log("[ghs] NOTE: the slot table decoded to RVA 0x%zx, not the 0x%zx beta21e had. "
            "The decode wins - it came from the instruction we are guarding.",
            (size_t)(table - g_base), (size_t)mxb::RVA_GHS_TABLE);

    g_ghsTable = (void**)table;
    if (!InstallHook(fn, (void*)&hkGhsClose, (void**)&g_origGhsClose, "ghsClose(0x11d730)")) {
        g_ghsTable = nullptr;
        return;
    }
    Log("[ghs] close guard live @ RVA 0x%zx, slots @ RVA 0x%zx. A double close now returns "
        "instead of taking the game down.",
        (size_t)(fn - g_base), (size_t)(table - g_base));
}


}  // namespace

void Configure(const Host& host) {
    g_host = host;
    g_base = host.game_base;
    g_sigDelta = host.sig_delta;
    g_game = host.game ? host.game : &GAME_MXB;
}

void SetUnloading(bool unloading) { g_unloading.store(unloading); }

void InstallGhsGuard() { InstallGhsGuard(g_sigDelta); }
void InstallTerrainGuard() { InstallTerrainGuard(g_sigDelta); }
void InstallTrainerGuard() { InstallTrainerGuard(g_sigDelta); }
void InstallTyreFallback() { InstallTyreFallback(g_sigDelta); }
void InstallMapGuard() { InstallMapGuardImpl(); }
void InstallShockGuard() { InstallShockGuardImpl(); }
void InstallSessionGuards() { InstallSessionGuardsImpl(); }

void Tick() {
    ShockGuardTick();
    SessionGuardsTick();
    LookupGuardTick();
}

bool NanRecoverInstall(const uint8_t* sim_step) { return NrInstall(sim_step); }
const uint8_t* ResolveSimStep(intptr_t delta, const char* log_tag) {
    return ResolveSimStepImpl(delta, log_tag ? log_tag : "nanrecover");
}
bool InstallSimStepHook(const uint8_t* sim_step, void* detour, void** original) {
    return sim_step && InstallHook(const_cast<uint8_t*>(sim_step), detour, original,
                                   "simStep(0x1be3a0)");
}
bool InstallNanRecoverHook(intptr_t delta) {
    if (!nanrecover_on.load()) {
        Log("[nanrecover] off by config (nanrecover=0)");
        return false;
    }
    const uint8_t* fn = ResolveSimStepImpl(delta, "nanrecover");
    if (!fn || !NrInstall(fn) ||
        !InstallSimStepHook(fn, reinterpret_cast<void*>(&hkNanRecoverStep),
                            reinterpret_cast<void**>(&g_origNanRecoverStep))) return false;
    g_nrLive.store(true, std::memory_order_release);
    Log("[nanrecover] ON: after every physics step each bike's bodies, wheels and suspension "
        "are checked for a value that is not a number. One that is gets the game's own "
        "reset (place-bike 0x%zx) where it was a moment before, instead of the crash. A "
        "finite bike is never touched. nanrecover=0 turns it off.",
        static_cast<size_t>(nanrecover::kRvaReset));
    return true;
}
void NanRecoverCheck(int32_t slot, bool before) { NrCheck(slot, before); }
void NanRecoverSetLive(bool live) { g_nrLive.store(live, std::memory_order_release); }
bool NanRecoverLive() { return g_nrLive.load(std::memory_order_relaxed); }
unsigned TerrainGuardedCount() { return g_terrainGuarded.load(std::memory_order_relaxed); }

void RevertTyreFallback() { TyreFallbackRevert(); }
void RevertLookupGuard() { LookupGuardRevert(); }
void RevertMapGuard() { MgRevert(); }
void RevertShockGuard() { SgRevert(); }

#undef Log

}  // namespace guards
