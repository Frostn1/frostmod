// FrostMod Fixes: when it stands down next to full FrostMod, and which of its guards it leaves
// to MXBFix. Pure decisions from src/fixesguard.h; no Win32, no game.
#include "../src/fixesguard.h"

#include <cstdio>
#include <cstring>

// Not assert(): ctest runs the Release build, where NDEBUG compiles assert out.
static int g_fail = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

int main() {
    using namespace fixesguard;

    auto d = DecideDoubleLoad(false, false, false);
    CHECK(d.install && d.reason == StandDownReason::None);
    d = DecideDoubleLoad(true, true, true);
    CHECK(!d.install && d.reason == StandDownReason::FullPluginFile);
    CHECK(std::strstr(d.message, "plugins\\frostmod.dlo"));
    d = DecideDoubleLoad(false, true, true);
    CHECK(!d.install && d.reason == StandDownReason::FullPluginMutex);
    d = DecideDoubleLoad(false, false, true);
    CHECK(!d.install && d.reason == StandDownReason::FullPluginModule);

    CHECK(kOverlaps.size() == 5);
    for (const auto& overlap : kOverlaps) {
        CHECK(FindOverlap(overlap.guard) == &overlap);
        CHECK(UseFrostModGuard(overlap.guard, false, 1));   // no MXBFix: ours
        CHECK(UseFrostModGuard(overlap.guard, true, 0));    // MXBFix has it off: ours
        CHECK(!UseFrostModGuard(overlap.guard, true, 1));   // MXBFix has it on: theirs
        CHECK(!UseFrostModGuard(overlap.guard, true, -1));
    }
    CHECK(std::strcmp(FindOverlap(Guard::Map)->mxbfix_key, "gate-vertex-pointer") == 0);
    CHECK(std::strcmp(FindOverlap(Guard::Terrain)->mxbfix_key, "ground-nan-guard") == 0);
    CHECK(std::strcmp(FindOverlap(Guard::Shock)->mxbfix_key, "suspension-nan-guard") == 0);
    CHECK(std::strcmp(FindOverlap(Guard::Ghs)->mxbfix_key, "trainer-cleanup-reset") == 0);
    CHECK(std::strcmp(FindOverlap(Guard::Trainer)->mxbfix_key, "trainer-series-lookup") == 0);

    if (g_fail) return 1;
    std::printf("fixesguard: all checks passed\n");
    return 0;
}
