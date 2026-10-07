// Which content refresh to run, and when it is safe to start one.
//
// Kinds. Each is a set of rows of the verified MX Bikes reload table (offsets.h,
// kReloadSteps); a smaller kind never rebuilds a list a bigger one would leave alone:
//   Paints  the six paint lists (kPaintReloadRvas), then the live-paints pass
//   Gear    the rider gear model lists and every paint list (kGearReloadRvas, a superset
//           of Paints), then the live-paints pass and the gear rebuild check
//   Tracks  the track list only (kTrackReloadRvas): no other row reads it
//   Bikes   the bike list, the series built from its categories, and the bike paints that
//           index into it (kBikeReloadRvas), then the bike state fix-up and live paints
//   Full    every row
// Which rows each kind needs is the STEP MAP in offsets.h (static RE, beta21e).
//
// When. A crash seen three times in one player's log (FrostMod 0.49.7) faults in the game's
// _stricmp, called from the bike chooser's page handler (RVA 0x91570, the function right
// before bike_choose's enter 0x944F0 in .pdata; one of the four bus-0x365 chooser pages),
// comparing the selected bike's name against a pointer that is no longer a string. Each came
// 2.6-6.4 s after a full reload finished, on a menu page. The chooser keeps its filtered list
// (a count, 0x4C2F18, and bike indexes in the per-bike state array) from when it was built;
// the bikes row replaces that array with an uninitialised one, so the walk reads garbage
// indexes (offsets.h, RVA_BIKE_CHOOSER_N). So a refresh never starts
// while a page that lists content is on the page stack: it waits, coalesced, and runs on the
// first frame after the player leaves that page. The game rebuilds a chooser from the current
// lists when it is entered again.
//
// Pure - no Win32, no game - so tests/refreshgate_test.cpp runs it in CI.
#pragma once

namespace frostmod {

enum class RefreshKind { None = 0, Paints = 1, Gear = 2, Tracks = 3, Bikes = 4, Full = 5 };

// One request on top of another still waiting: the result rebuilds everything both asked
// for. Paints nests in Gear and everything nests in Full; any other pair (tracks + bikes,
// bikes + gear, ...) has no kind that covers exactly both, so it becomes Full.
inline RefreshKind MergeRefresh(RefreshKind a, RefreshKind b) {
    if (a == b || b == RefreshKind::None) return a;
    if (a == RefreshKind::None) return b;
    if ((a == RefreshKind::Paints && b == RefreshKind::Gear) ||
        (a == RefreshKind::Gear && b == RefreshKind::Paints))
        return RefreshKind::Gear;
    return RefreshKind::Full;
}

inline const char* RefreshKindName(RefreshKind k) {
    switch (k) {
    case RefreshKind::Paints: return "paints";
    case RefreshKind::Gear:   return "gear";
    case RefreshKind::Tracks: return "tracks";
    case RefreshKind::Bikes:  return "bikes";
    case RefreshKind::Full:   return "full";
    default:                  return "none";
    }
}

// "paints" / "gear" / "tracks" / "bikes" / "full" (also "all", "mods") -> kind; anything
// else -> None.
inline RefreshKind ParseRefreshKind(const char* s) {
    if (!s) return RefreshKind::None;
    auto eq = [](const char* a, const char* b) {
        while (*a && *b) {
            char x = *a, y = *b;
            if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
            if (x != y) return false;
            ++a; ++b;
        }
        return *a == 0 && *b == 0;
    };
    if (eq(s, "paints") || eq(s, "paint")) return RefreshKind::Paints;
    if (eq(s, "gear")) return RefreshKind::Gear;
    if (eq(s, "tracks") || eq(s, "track")) return RefreshKind::Tracks;
    if (eq(s, "bikes") || eq(s, "bike")) return RefreshKind::Bikes;
    if (eq(s, "full") || eq(s, "all") || eq(s, "mods")) return RefreshKind::Full;
    return RefreshKind::None;
}

// Pages (ui_pages.c names, page table 0x3954F0) that list content the reload rebuilds:
// the bike and paint choosers (bike_choose, bike_info and the three pit pages that carry the
// same chooser), the rider profile (gear), and the event setups that pick a track and bike.
inline bool PageListsContent(const char* s) {
    static const char* const kPages[] = {
        "bike_choose", "bike_info", "multi_pit", "testingday_pit", "straightrhythm_pit",
        "profiles", "hostsetup", "time_trial", "testing_setup", "multi_racesetup",
        "setup", "setup_test",
    };
    if (!s) return false;
    for (const char* want : kPages) {
        const char* a = s; const char* b = want;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == 0 && *b == 0) return true;
    }
    return false;
}

// Safe to start a refresh with this page stack on screen? An unreadable stack (n < 0) is
// not safe: the caller waits for a frame it can read.
inline bool RefreshSafeOnPages(const char* const* names, int n) {
    if (n < 0) return false;
    for (int i = 0; i < n; ++i)
        if (!names || PageListsContent(names[i])) return false;
    return true;
}

}  // namespace frostmod
