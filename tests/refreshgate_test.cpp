// Which refresh runs and when it may start (src/refreshgate.h). Pure, so CI runs it.

#include "../src/refreshgate.h"

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

using frostmod::MergeRefresh;
using frostmod::ParseRefreshKind;
using frostmod::RefreshKind;
using frostmod::RefreshSafeOnPages;

int main() {
    // Waiting requests coalesce to the one that covers both.
    CHECK(MergeRefresh(RefreshKind::None, RefreshKind::Paints) == RefreshKind::Paints, "none+paints");
    CHECK(MergeRefresh(RefreshKind::Paints, RefreshKind::Gear) == RefreshKind::Gear, "paints+gear");
    CHECK(MergeRefresh(RefreshKind::Gear, RefreshKind::Paints) == RefreshKind::Gear, "gear+paints");
    CHECK(MergeRefresh(RefreshKind::Full, RefreshKind::Paints) == RefreshKind::Full, "full wins");
    CHECK(MergeRefresh(RefreshKind::Paints, RefreshKind::Full) == RefreshKind::Full, "full wins (2)");
    CHECK(MergeRefresh(RefreshKind::None, RefreshKind::None) == RefreshKind::None, "nothing");
    CHECK(MergeRefresh(RefreshKind::Tracks, RefreshKind::Tracks) == RefreshKind::Tracks, "tracks+tracks");
    CHECK(MergeRefresh(RefreshKind::None, RefreshKind::Bikes) == RefreshKind::Bikes, "none+bikes");
    CHECK(MergeRefresh(RefreshKind::Bikes, RefreshKind::None) == RefreshKind::Bikes, "bikes+none");
    CHECK(MergeRefresh(RefreshKind::Tracks, RefreshKind::Bikes) == RefreshKind::Full, "tracks+bikes -> full");
    CHECK(MergeRefresh(RefreshKind::Bikes, RefreshKind::Paints) == RefreshKind::Full,
          "bikes+paints -> full (bikes rebuilds the bike paints only)");
    CHECK(MergeRefresh(RefreshKind::Gear, RefreshKind::Tracks) == RefreshKind::Full, "gear+tracks -> full");
    CHECK(MergeRefresh(RefreshKind::Full, RefreshKind::Bikes) == RefreshKind::Full, "full covers bikes");

    // Kind names from the command channel.
    CHECK(ParseRefreshKind("paints") == RefreshKind::Paints, "paints");
    CHECK(ParseRefreshKind("Paint") == RefreshKind::Paints, "paint, any case");
    CHECK(ParseRefreshKind("gear") == RefreshKind::Gear, "gear");
    CHECK(ParseRefreshKind("full") == RefreshKind::Full, "full");
    CHECK(ParseRefreshKind("ALL") == RefreshKind::Full, "all");
    CHECK(ParseRefreshKind("tracks") == RefreshKind::Tracks, "tracks");
    CHECK(ParseRefreshKind("Track") == RefreshKind::Tracks, "track, any case");
    CHECK(ParseRefreshKind("bikes") == RefreshKind::Bikes, "bikes");
    CHECK(ParseRefreshKind("BIKE") == RefreshKind::Bikes, "bike, any case");
    CHECK(ParseRefreshKind("") == RefreshKind::None, "empty");
    CHECK(ParseRefreshKind("paintsx") == RefreshKind::None, "no prefix match");
    CHECK(ParseRefreshKind(nullptr) == RefreshKind::None, "null");

    // The gate, on page stacks read live (ui_pages.c).
    {
        const char* mainMenu[] = {"background", "main_menu"};
        const char* chooser[] = {"background", "main_menu", "bike_choose"};
        const char* pits[] = {"chat", "pit", "multi_pit"};
        const char* riding[] = {"mtrackm"};
        const char* settings[] = {"background", "settings"};
        const char* profiles[] = {"background", "profiles"};
        const char* hostsetup[] = {"background", "hostsetup"};
        CHECK(RefreshSafeOnPages(mainMenu, 2), "main menu is safe");
        CHECK(RefreshSafeOnPages(riding, 1), "riding is safe (live paints run there)");
        CHECK(RefreshSafeOnPages(settings, 2), "settings is safe");
        CHECK(!RefreshSafeOnPages(chooser, 3), "the bike chooser holds the lists - wait");
        CHECK(!RefreshSafeOnPages(pits, 3), "the online pits carry the chooser - wait");
        CHECK(!RefreshSafeOnPages(profiles, 2), "the profile lists gear - wait");
        CHECK(!RefreshSafeOnPages(hostsetup, 2), "an event setup lists tracks and bikes - wait");
        CHECK(RefreshSafeOnPages(nullptr, 0), "empty stack is safe");
        CHECK(!RefreshSafeOnPages(nullptr, -1), "unreadable stack waits");
        CHECK(!frostmod::PageListsContent("bike_choosex"), "exact names only");
        CHECK(!frostmod::PageListsContent(nullptr), "null name");
    }

    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("refreshgate: all passed\n");
    return 0;
}
