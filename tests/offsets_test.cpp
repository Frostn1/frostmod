// Invariants over the per-title offset tables in offsets.h.
//
// This exists because of a shipped bug, not in the abstract. FrostMod v0.10.0 attached to
// GP Bikes correctly and then ran MX Bikes' reload table inside it - calling arbitrary
// functions and zeroing arbitrary globals in gpbikes.exe, which took the game down on the
// first reload. Nothing caught it, because nothing here asserted that a title's offsets
// belong to that title.
//
// It is pure constants, so unlike the rest of the project it builds and runs anywhere -
// no Win32, no MSVC, no game. Keep it that way: it is the only part of FrostMod that CI
// can actually execute.

#include "../src/offsets.h"

#include <cstdio>
#include <cstring>

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

// The MX table as it shipped, transcribed from fcn.1400ef210. Duplicated here on purpose:
// the point is to notice when someone edits the real one, so this copy must be independent.
static const RLStep kMxExpected[] = {
    {0,0,0,0,0x2460}, {0,0,0,0,0x1CE00},
    {1,0xF3DC80,0x109DEC4,0xF3DC48,0x1B790},
    {0,0,0,0,0x3100}, {0,0,0,0,0x3FA0}, {0,0,0,0,0x171D0},
    {1,0x109DE88,0xF3DC40,0xF4EDF8,0x17320},
    {1,0xF3DB9C,0xF3DC9C,0xF4EDA0,0x17950},
    {0,0,0,0,0x17F80},
    {1,0xF48620,0xF3DB64,0x109E090,0x18360},
    {1,0x10A30F4,0xF4EDDC,0xF3DC28,0x189C0},
    {1,0xF4EE00,0x109DE90,0xF48610,0x19060},
    {0,0,0,0,0x1BDD0},
    {1,0xF3DB50,0x109DEA8,0xF3DC90,0x19330},
    {1,0x109DEA4,0xF3DC8C,0xF48658,0x1AE10},
    {0,0,0,0,0x1B420}, {0,0,0,0,0x19DA0},
    {1,0xF48660,0xF4EDE0,0xF432A0,0x1A110},
    {1,0xF3DC58,0xF432B4,0xF48208,0x1A770},
    {1,0x106BB28,0x109DEB0,0xF432A8,0x1C140},
    {1,0xF432C0,0xF48608,0xF4EDD0,0x1C450},
};
static const int kMxExpectedCount = (int)(sizeof(kMxExpected) / sizeof(kMxExpected[0]));

static bool same(const RLStep& a, const RLStep& b) {
    return a.dir == b.dir && a.z1 == b.z1 && a.z2 == b.z2 && a.z3 == b.z3 && a.rva == b.rva;
}

// The MX reload path is the one that works today; the GP port refactored it. Any drift
// here is a regression in the live feature, not a GP problem.
static void mx_table_is_unchanged() {
    CHECK(mxb::kReloadStepCount == kMxExpectedCount,
          "MX step count is %d, expected %d", mxb::kReloadStepCount, kMxExpectedCount);
    if (mxb::kReloadStepCount != kMxExpectedCount) return;
    for (int i = 0; i < kMxExpectedCount; ++i)
        CHECK(same(mxb::kReloadSteps[i], kMxExpected[i]), "MX step %d differs", i);

    CHECK(mxb::RVA_RELOAD_STR == 0x3333EB, "MX reload_str moved");
    CHECK(mxb::RVA_RELOAD_MODS == 0xE54B44, "MX reload_mods moved");
}

// GP's loaders each clear their own lists and scan both directories, so every step is SC.
// A DIR step appearing here would need z-globals and the two operands, which GP has not
// had derived - so it would run with zeros. Fail loudly rather than let that ship.
static void gp_table_is_all_self_contained() {
    CHECK(gpb::kReloadStepCount == 13, "GP step count is %d, expected 13", gpb::kReloadStepCount);
    for (int i = 0; i < gpb::kReloadStepCount; ++i) {
        const RLStep& s = gpb::kReloadSteps[i];
        CHECK(s.dir == 0, "GP step %d is DIR; GP's loaders are self-contained", i);
        CHECK(s.z1 == 0 && s.z2 == 0 && s.z3 == 0, "GP step %d carries z-globals", i);
        CHECK(s.rva != 0, "GP step %d has a null RVA", i);
    }
    CHECK(gpb::kReloadSteps[0].rva == 0x139A0, "GP's first loader should be tracks (0x139A0)");
}

// A step that kills the process is only fixable if the log says which one it was, and the
// log line is built from `what`. This is not cosmetic: every step is SEH-guarded, so the
// crash itself leaves nothing behind - the label written before the call is the evidence.
// Required on unconfirmed tables, where a crash is expected rather than hypothetical.
static void unconfirmed_tables_label_every_step() {
    for (const GameOffsets* g : ALL_GAMES) {
        if (!g->reload_steps || g->reload_verified) continue;
        for (int i = 0; i < g->reload_count; ++i)
            CHECK(g->reload_steps[i].what && *g->reload_steps[i].what,
                  "%s step %d has no label; a crash in it would be unattributable", g->id, i);
    }
}

// The state this file exists to protect. `reload_verified` is what stands between a player
// and a table that has never run on their title: v0.10.0 shipped GP Bikes MX's table and
// crashed it, v0.11.0 shipped GP its own and crashed it too. Flipping this to true is a
// claim that someone watched the reload complete on that game - not that it compiles.
static void only_confirmed_tables_run_unprompted() {
    CHECK(GAME_MXB.reload_verified, "MX Bikes' table is the shipping one - it must stay confirmed");
    CHECK(!GAME_GPB.reload_verified,
          "GP Bikes' table is marked confirmed, but it took a reporter's game down on "
          "v0.11.0. Only flip this after watching a reload finish on GP Bikes itself.");
    for (const GameOffsets* g : ALL_GAMES)
        if (!g->reload_steps)
            CHECK(!g->reload_verified, "%s has no table but claims a confirmed one", g->id);
}

// The actual v0.10.0 defect: one title running another title's addresses. No two titles may
// share a step table, and no table may be reachable from the wrong GameOffsets.
static void no_title_borrows_another_titles_offsets() {
    for (const GameOffsets* a : ALL_GAMES) {
        for (const GameOffsets* b : ALL_GAMES) {
            if (a == b) continue;
            CHECK(a->reload_steps != b->reload_steps || a->reload_steps == nullptr,
                  "%s and %s share a reload table", a->display, b->display);
            CHECK(std::strcmp(a->id, b->id) != 0, "duplicate game id '%s'", a->id);
            CHECK(std::strcmp(a->exe, b->exe) != 0, "duplicate exe '%s'", a->exe);
        }
    }
    CHECK(GAME_MXB.reload_steps == mxb::kReloadSteps, "MX Bikes is not pointing at its own table");
    CHECK(GAME_GPB.reload_steps == gpb::kReloadSteps, "GP Bikes is not pointing at its own table");
}

// A table is only usable with a matching count, and a DIR step is only meaningful when the
// title also supplies the two operands it passes. Null steps are fine - that means "reload
// unsupported for this title", which RequestReload refuses cleanly.
static void every_title_is_internally_consistent() {
    for (const GameOffsets* g : ALL_GAMES) {
        CHECK(g->id && *g->id, "a title has no id");
        CHECK(g->exe && *g->exe, "%s has no exe", g->id);
        CHECK(g->user_dir && *g->user_dir, "%s has no user_dir", g->id);
        CHECK(g->content_init != 0, "%s has no content_init", g->id);
        CHECK(g->scan_folder != 0, "%s has no scan_folder", g->id);

        if (!g->reload_steps) {
            CHECK(g->reload_count == 0, "%s has no table but a nonzero count", g->id);
            continue;
        }
        CHECK(g->reload_count > 0, "%s has a table but no steps", g->id);

        bool anyDir = false;
        for (int i = 0; i < g->reload_count; ++i) anyDir |= (g->reload_steps[i].dir != 0);
        if (anyDir) {
            CHECK(g->reload_str != 0, "%s has DIR steps but no reload_str operand", g->id);
            CHECK(g->reload_mods != 0, "%s has DIR steps but no reload_mods operand", g->id);
        }
    }
}

// These ids are a cross-repo contract with MXB App, which launches us as
// `frostmod.exe --game <id>`. Renaming one silently stops FrostMod attaching.
static void ids_match_what_mxb_app_sends() {
    CHECK(std::strcmp(GAME_MXB.id, "mxb") == 0, "MX Bikes id changed");
    CHECK(std::strcmp(GAME_GPB.id, "gpb") == 0, "GP Bikes id changed");
    CHECK(std::strcmp(GAME_MXB.user_dir, "MX Bikes") == 0, "MX Bikes user_dir changed");
    CHECK(std::strcmp(GAME_GPB.user_dir, "GP Bikes") == 0, "GP Bikes user_dir changed");
}

int main() {
    mx_table_is_unchanged();
    gp_table_is_all_self_contained();
    unconfirmed_tables_label_every_step();
    only_confirmed_tables_run_unprompted();
    no_title_borrows_another_titles_offsets();
    every_title_is_internally_consistent();
    ids_match_what_mxb_app_sends();

    if (g_failures) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("offsets: all checks passed (%d MX steps, %d GP steps)\n",
                mxb::kReloadStepCount, gpb::kReloadStepCount);
    return 0;
}
