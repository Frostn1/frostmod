// Race mode's mod list (src/racefilter.h): what the game is shown while it scans mods/.
//
// A wrong answer here is quiet and expensive: a track the race needs goes missing from the
// list, or the filter hides a rider's stock-bike paints. Pure, so CI runs it.

#include "../src/racefilter.h"

#include <cstdio>
#include <string>

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

using frostmod::RaceFilter;

static const char* kMods = "C:\\Users\\Me\\Documents\\PiBoSo\\MX Bikes\\mods";

int main() {
    // No file / nothing usable: no filtering anywhere.
    {
        RaceFilter f;
        CHECK(f.Load("") == 0, "empty");
        CHECK(!f.Active(), "empty is inactive");
        CHECK(f.Scope(std::string(kMods) + "\\tracks", kMods).empty(), "no scope when inactive");
        CHECK(f.Load("# comment\n\ntracks\nrider/foo\n../x\n") == 0, "nothing usable");
        CHECK(!f.Active(), "rider/ and a bare root do not arm it");
    }

    RaceFilter f;
    CHECK(f.Load("\xEF\xBB\xBFtracks/Red Bud\r\ntracks/motocross/Club MX.pkz\n  bikes/KTM 450.pkz \n") == 3,
          "three lines");
    CHECK(f.Active(), "active");

    // tracks/ root: only listed mods and the folders leading to them.
    std::string s = f.Scope(std::string(kMods) + "\\tracks", kMods);
    CHECK(s == "tracks", "scope tracks, got '%s'", s.c_str());
    CHECK(f.Allows(s, "Red Bud"), "listed folder");
    CHECK(f.Allows(s, "RED BUD.pkz"), "folder listed, pkz on disk, any case");
    CHECK(f.Allows(s, "motocross"), "ancestor kept");
    CHECK(!f.Allows(s, "Muddy Creek"), "unlisted hidden");
    CHECK(!f.Allows(s, "Muddy Creek.pkz"), "unlisted pkz hidden");
    CHECK(f.Allows(s, ".") && f.Allows(s, ".."), "dot entries pass");

    s = f.Scope(std::string(kMods) + "\\tracks/motocross", kMods);
    CHECK(s == "tracks/motocross", "mixed separators, got '%s'", s.c_str());
    CHECK(f.Allows(s, "Club MX.pkz") && f.Allows(s, "club mx"), "pkz listed, either form");
    CHECK(!f.Allows(s, "Other"), "sibling hidden");

    // Inside an allowed mod everything passes (no scope at all).
    CHECK(f.Scope(std::string(kMods) + "\\tracks\\Red Bud", kMods).empty(), "inside allowed");
    CHECK(f.Scope(std::string(kMods) + "\\tracks\\Red Bud\\sub", kMods).empty(), "deeper inside allowed");
    CHECK(f.Scope(std::string(kMods) + "\\tracks\\motocross\\Club MX", kMods).empty(), "inside allowed pkz stem");

    // bikes/ is filtered too, since the file names a bike.
    s = f.Scope(std::string(kMods) + "\\bikes", kMods);
    CHECK(s == "bikes", "scope bikes");
    CHECK(f.Allows(s, "KTM 450.pkz") && !f.Allows(s, "Honda 450"), "bikes filtered");

    // Outside mods, or a root the file does not name: untouched.
    CHECK(f.Scope("tracks/motocross", kMods).empty(), "stock game tracks untouched");
    CHECK(f.Scope(std::string(kMods) + "\\rider", kMods).empty(), "rider untouched");
    CHECK(f.Scope(std::string(kMods) + "\\tyres", kMods).empty(), "tyres untouched");

    // A tracks-only list leaves bikes alone.
    RaceFilter t;
    CHECK(t.Load("tracks/Red Bud\n") == 1, "one");
    CHECK(t.Scope(std::string(kMods) + "\\bikes", kMods).empty(), "bikes not filtered by a tracks-only list");

    // Mods folder unknown: fall back to the "/mods/" segment.
    CHECK(f.Scope(std::string(kMods) + "\\tracks", "") == "tracks", "fallback");

    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("racefilter: all passed\n");
    return 0;
}
