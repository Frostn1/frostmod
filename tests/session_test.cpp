// The rules the MXB App session block runs on (src/session.h).
//
// This block is how the app learns which server the game is on and where every rider is —
// the two facts voice chat is built out of. It is written by a game thread that must never
// block and read by a separate process that may be killed mid-read, so the seqlock is the
// whole safety story, and a wrong answer here is either a room nobody can join or a rider
// heard from a position that was never real.
//
// Like offsets_test.cpp and command_channel_test.cpp this is pure — no Win32, no game, no
// shared memory — so CI can actually run it.

#include "../src/session.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n");                                                  \
        }                                                                       \
    } while (0)

using namespace frostmod::session;

static Block MakeBlock(const char* server, int riders) {
    Block b{};
    BeginWrite(b);
    SetField(b.serverName, server);
    SetField(b.trackId, "practice_track");
    SetField(b.guid, "abc-123");
    SetField(b.riderName, "Frost");
    b.raceNum = 7;
    b.riderCount = riders;
    for (int i = 0; i < riders; ++i) {
        b.riders[i].raceNum = i + 1;
        b.riders[i].x = (float)i;
        b.riders[i].yawDeg = 90.0f;
        SetField(b.riders[i].name, "Rider");
    }
    EndWrite(b);
    return b;
}

int main() {
    // ---- the copy is whole, or it is refused --------------------------------------
    {
        Block live = MakeBlock("Frost Racing EU", 3);
        Block out{};
        CHECK(TryRead(live, out), "a settled block should read");
        CHECK(std::strcmp(out.serverName, "Frost Racing EU") == 0, "server name lost");
        CHECK(out.riderCount == 3, "rider count lost");
        CHECK(out.riders[2].raceNum == 3, "rider table lost");
        CHECK(out.raceNum == 7, "our own race number lost");
    }
    {
        // Mid-write: the reader must refuse rather than take half an update.
        Block live = MakeBlock("Frost Racing EU", 3);
        BeginWrite(live);
        Block out{};
        CHECK(!TryRead(live, out), "a block being written must not read");
        EndWrite(live);
        CHECK(TryRead(live, out), "and must read again once it settles");
    }
    {
        // A version we don't know is a layout we can't trust.
        Block live = MakeBlock("Frost Racing EU", 1);
        live.version = kVersion + 1;
        Block out{};
        CHECK(!TryRead(live, out), "an unknown version must be refused");
    }

    // ---- what counts as being on a server ------------------------------------------
    {
        CHECK(OnAServer(MakeBlock("Frost Racing EU", 2)), "a named server is a server");
        CHECK(!OnAServer(MakeBlock("", 0)), "testing and replays are not");
    }

    // ---- fields are bounded and always terminated -----------------------------------
    {
        Block b{};
        std::string huge(500, 'x');
        SetField(b.serverName, huge.c_str());
        CHECK(b.serverName[sizeof(b.serverName) - 1] == '\0', "a long name must stay terminated");
        CHECK(std::strlen(b.serverName) == sizeof(b.serverName) - 1, "a long name should fill the field");
        SetField(b.trackId, nullptr);
        CHECK(b.trackId[0] == '\0', "a null value should clear the field");
        // A shorter value must not leave the tail of a longer one behind it — that is how a
        // stale server name outlives the session it belonged to.
        SetField(b.serverName, "Short");
        CHECK(std::strcmp(b.serverName, "Short") == 0, "a shorter value should replace, not overwrite");
        CHECK(b.serverName[6] == '\0', "the tail of the old value should be gone");
    }

    // ---- the layout the app's reader is compiled against ----------------------------
    {
        // Named explicitly rather than left to static_assert alone, so the numbers the Rust
        // side hard-codes are visible in a test that prints when it fails.
        CHECK(sizeof(Rider) == 56, "Rider is %zu bytes, the reader expects 56", sizeof(Rider));
        CHECK(sizeof(Block) == 3976, "Block is %zu bytes, the reader expects 3976", sizeof(Block));
        CHECK(offsetof(Block, serverName) == 8, "serverName moved to %zu", offsetof(Block, serverName));
        CHECK(offsetof(Block, riders) == 392, "riders moved to %zu", offsetof(Block, riders));
    }

    // ---- a real writer racing a real reader -----------------------------------------
    {
        // The property: a reader either gets a whole snapshot or nothing. It must never see
        // a rider count from one update beside a rider table from another, which is what a
        // torn read looks like in the field — a voice placed where nobody is.
        Block live = MakeBlock("Frost Racing EU", 1);
        std::atomic<bool> stop{false};
        std::thread writer([&] {
            for (int round = 1; !stop.load(); ++round) {
                int n = (round % kMaxRiders) + 1;
                BeginWrite(live);
                live.riderCount = n;
                for (int i = 0; i < n; ++i) {
                    live.riders[i].raceNum = n;  // every entry agrees with the count
                }
                EndWrite(live);
            }
        });

        int reads = 0, torn = 0;
        for (int i = 0; i < 200000; ++i) {
            Block out{};
            if (!TryRead(live, out)) continue;
            ++reads;
            for (int r = 0; r < out.riderCount; ++r) {
                if (out.riders[r].raceNum != out.riderCount) { ++torn; break; }
            }
        }
        stop.store(true);
        writer.join();

        CHECK(reads > 0, "the reader never got a settled copy in 200000 tries");
        CHECK(torn == 0, "%d of %d reads were torn", torn, reads);
    }

    if (g_failures == 0) {
        std::printf("session: all checks passed\n");
        return 0;
    }
    std::printf("session: %d check(s) failed\n", g_failures);
    return 1;
}
