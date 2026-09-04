// What FrostMod tells MXB App about the session, minus the operating system.
//
// The app needs two things it cannot get for itself: **which server the game is on**, and
// **where every rider is**. Both arrive in this process through the sanctioned plugin API
// and nowhere else — the app is a separate program with no view into the game — so this is
// the channel between them.
//
// It carries no commands and takes no input. FrostMod writes; the app reads. That is the
// whole contract, and it is why this can be a shared block rather than a protocol.
//
// ## Why the server name and not an address
//
// `EventInit` hands the client `m_szServerName`, and it is the same string for everyone who
// joined that server. (On MX Bikes. GP Bikes' and Kart Racing Pro's events have no such
// field, so there the same string is taken from `RaceEvent`'s `m_szName` — same key, one
// callback further along.) An address is not: a rider who picked the server from the game's own
// browser never sees one, and the app only knows an address when it launched the game
// itself. A key that only some riders can compute puts them in different rooms, which is
// the one failure that makes voice chat look broken while working perfectly.
//
// (Two servers sharing a name would share a room. That is the same class of collision as
// two riders sharing a rider name — real, rare, and not worth a worse key to avoid.)
//
// ## Why a seqlock
//
// The writer is a game thread that must never block, and the reader is another process that
// may be killed mid-read. A mutex across a process boundary can be abandoned; a lock-free
// counter cannot. The writer marks the block odd while it changes it and even when it is
// whole again, so a reader that sees the same even count either side of its copy knows
// nothing moved underneath it — and one that doesn't simply reads again, 30 ms later, when
// it will not matter that it did.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace frostmod {
namespace session {

// Bumped if anything below changes shape. The app refuses a version it doesn't know rather
// than reading a field that has moved — the two ship separately and update independently.
constexpr uint32_t kVersion = 1;

// A full grid is around forty. Sized past that so a rider is never invisible for being
// late to the list.
constexpr int kMaxRiders = 64;

// `Local\` rather than `Global\`: both processes run as the same user on the same machine,
// and a global name would need privileges the app does not have and should not want.
constexpr const char kMappingName[] = "Local\\FrostModSession";

// ## Why there are two blocks
//
// The server name arrives through `EventInit`, and the game only calls that on a plugin it
// loaded itself from `plugins\*.dlo`. FrostMod injected as a `.dll` is never asked — so for
// every player the app drives, `serverName` above stayed empty forever and the app could not
// tell which server anyone was on.
//
// The fix is a second copy of this binary sitting in the game's plugins folder purely to
// receive those callbacks. It publishes here, in its own block, and the two never share a
// writer: one seqlock with two independent writers in it would corrupt under exactly the
// interleaving that is hardest to reproduce. The app reads both and takes the server name
// from this one and the grid from the other.
constexpr const char kPluginMappingName[] = "Local\\FrostModPluginSession";

// The file name that puts this binary in session-only mode.
//
// Mode is taken from the module's own name rather than a flag file beside it: there is
// nothing to lose, nothing to get out of step with the binary, and no window in which the
// mode is not yet known. The game loads every `*.dlo` in its plugins folder, so a copy
// under this name is loaded and asked for callbacks like any other plugin — and answers
// only the three that name the session.
//
// A hand-installed `frostmod.dlo` is deliberately *not* this name and keeps full plugin
// mode, which is what someone who installed it by hand asked for.
constexpr const char kSessionPluginFileName[] = "frostmod_session.dlo";

// One rider, as the game reports them.
struct Rider {
    int32_t raceNum;
    float x, y, z;
    // Degrees from north, exactly as the SDK gives it. Passed on unconverted: the reader
    // does its own trigonometry and a unit changed in transit is a bug waiting to be found
    // twice.
    float yawDeg;
    int32_t crashed;
    char name[32];
};

struct Block {
    uint32_t version;
    // Even and unchanged across a read means the copy is whole. Odd means a write is in
    // flight.
    uint32_t seq;

    // From EventInit. Empty when the game is not in an online session, which is how the app
    // tells "not on a server" from "on one we can't name".
    char serverName[64];
    char trackId[104];
    // Ours, not anyone else's — the plugin API exposes only the local player's GUID.
    char guid[104];
    char riderName[104];

    // Our own race number in this session, or -1 before the grid exists.
    int32_t raceNum;
    int32_t riderCount;
    Rider riders[kMaxRiders];
};

// Layout is a wire contract with a Rust reader in another program. Every field is 4-byte
// aligned and every array a multiple of four, so there is no padding for the two sides to
// disagree about — and if that ever stops being true, this fails the build rather than the
// race.
static_assert(sizeof(Rider) == 56, "Rider layout changed - update the app's reader");
static_assert(sizeof(Block) == 392 + 56 * kMaxRiders, "Block layout changed - update the app's reader");

// Copy a NUL-terminated string into a fixed field, always terminated, never overrun.
template <size_t N>
inline void SetField(char (&field)[N], const char* value) {
    std::memset(field, 0, N);
    if (!value) return;
    size_t n = std::strlen(value);
    if (n > N - 1) n = N - 1;
    std::memcpy(field, value, n);
}

// Writer: mark the block busy. Everything between this and `EndWrite` may be inconsistent.
inline void BeginWrite(Block& block) {
    block.version = kVersion;
    block.seq |= 1u;
    // A compiler is free to sink the stores below above this one without a barrier. The
    // reader is another process, so this is the cheapest fence that means anything here.
    std::atomic_thread_fence(std::memory_order_release);
}

inline void EndWrite(Block& block) {
    std::atomic_thread_fence(std::memory_order_release);
    block.seq = (block.seq + 1u) & ~1u;
}

// Reader: take a copy, or fail. Failure is ordinary — the writer was mid-update — and the
// answer is to try again rather than to report anything.
inline bool TryRead(const volatile Block& live, Block& out) {
    uint32_t before = live.seq;
    if (before & 1u) return false;
    std::atomic_thread_fence(std::memory_order_acquire);

    std::memcpy(&out, const_cast<const Block*>(&live), sizeof(Block));

    std::atomic_thread_fence(std::memory_order_acquire);
    if (live.seq != before) return false;
    return out.version == kVersion;
}

// Is this session one the app can put a voice room behind?
//
// A server name is the whole test. Testing, replays and single-player leave it empty, and
// there is nobody to talk to in any of them.
inline bool OnAServer(const Block& block) {
    return block.serverName[0] != '\0';
}

} // namespace session
} // namespace frostmod
