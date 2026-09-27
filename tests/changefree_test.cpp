// After an accepted bike change: which vehicle record FrostMod frees (src/changefree.h).
//
// Freeing the wrong record takes a live rider out of the game, so every "no" case matters
// as much as the one "yes". Pure, so CI runs it.

#include "../src/changefree.h"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n  (%s)\n", #cond);                                   \
        }                                                                       \
    } while (0)

using frostmod::ChangeFree;
using frostmod::ChangeSnapshot;
using frostmod::DecideChangeFree;

int main() {
    const int kMax = 50;
    ChangeSnapshot s{3, 17};

    // The case it exists for: the game moved to record 7, record 3 still holds rider 17.
    CHECK(DecideChangeFree(s, 7, true, 17, 16, kMax) == ChangeFree::Free, "moved -> free old");

    // Not yet moved: wait, then give up at the timeout without touching anything.
    CHECK(DecideChangeFree(s, 3, true, 17, 100, kMax) == ChangeFree::Wait, "same index waits");
    CHECK(DecideChangeFree(s, 3, true, 17, 10000, kMax) == ChangeFree::Drop, "timeout drops");

    // Old record already gone, or taken by someone else: never free it.
    CHECK(DecideChangeFree(s, 7, false, 17, 16, kMax) == ChangeFree::Drop, "old zeroed");
    CHECK(DecideChangeFree(s, 7, true, 42, 16, kMax) == ChangeFree::Drop, "old reused");

    // Nonsense index now (none, out of range): wait, not free.
    CHECK(DecideChangeFree(s, -1, true, 17, 16, kMax) == ChangeFree::Wait, "no index yet");
    CHECK(DecideChangeFree(s, 50, true, 17, 16, kMax) == ChangeFree::Wait, "out of range");

    // A move seen only after the timeout is stale: drop, never free.
    CHECK(DecideChangeFree(s, 7, true, 17, 10000, kMax) == ChangeFree::Drop, "late move drops");

    // Nothing snapshotted (not in a session, or a rejoin): never arms.
    CHECK(DecideChangeFree(ChangeSnapshot{}, 7, true, 0, 16, kMax) == ChangeFree::Drop, "no snapshot");
    CHECK(DecideChangeFree(ChangeSnapshot{60, 17}, 7, true, 17, 16, kMax) == ChangeFree::Drop, "bad snapshot");

    // Riding by the page stack: "mtrack" exactly; the pits page "mtrackm" is not riding.
    {
        const char* track[] = {"mtrack"};
        const char* pits[] = {"mtrackm"};
        const char* menu[] = {"mmain", "mserver"};
        const char* over[] = {"mtrack", "mpause"};
        CHECK(frostmod::PagesShowRiding(track, 1), "mtrack is riding");
        CHECK(!frostmod::PagesShowRiding(pits, 1), "mtrackm is the pits");
        CHECK(!frostmod::PagesShowRiding(menu, 2), "menus are not riding");
        CHECK(frostmod::PagesShowRiding(over, 2), "a page over the track is still riding");
        CHECK(!frostmod::PagesShowRiding(nullptr, 0), "empty stack");
    }

    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("changefree: all passed\n");
    return 0;
}
