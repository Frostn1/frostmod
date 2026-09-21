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
        // Both or neither. One of the two derived is the dangerous middle: the DLL would
        // gate the content hooks on `content_derived()` being false while some other path
        // read the one address that is set, at `base + 0` - the PE header.
        CHECK((g->content_init != 0) == (g->scan_folder != 0),
              "%s has one content offset derived and not the other", g->id);
        CHECK(g->content_derived() == (g->content_init != 0), "%s: content_derived() lies", g->id);
        if (!g->content_derived())
            CHECK(!g->reload_steps,
                  "%s has a reload table but no content offsets - the table cannot be run "
                  "without the addresses it is replayed alongside", g->id);

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
    CHECK(std::strcmp(GAME_KRP.id, "krp") == 0, "Kart Racing Pro id changed");
    CHECK(std::strcmp(GAME_MXB.user_dir, "MX Bikes") == 0, "MX Bikes user_dir changed");
    CHECK(std::strcmp(GAME_GPB.user_dir, "GP Bikes") == 0, "GP Bikes user_dir changed");
    CHECK(std::strcmp(GAME_KRP.user_dir, "Kart Racing Pro") == 0, "Kart Racing Pro user_dir changed");
}

// The plugin handshake. The game asks for these three and silently DROPS the .dlo if any
// of them is not its own - no log line, no error, just a plugin that never loads. They are
// PiBoSo's numbers, one set per title (mxb_example.c / gpb_example.c / krp_example.c), and
// the only way to notice a wrong one is on the game itself, which is why they are pinned
// here. FrostMod answered "mxbikes"/8 to every host until v0.15; that is exactly why
// plugin mode did nothing on GP Bikes.
static void plugin_handshake_is_each_titles_own() {
    CHECK(std::strcmp(GAME_MXB.plugin_id, "mxbikes") == 0, "MX Bikes plugin id changed");
    CHECK(std::strcmp(GAME_GPB.plugin_id, "gpbikes") == 0, "GP Bikes plugin id changed");
    CHECK(std::strcmp(GAME_KRP.plugin_id, "krp") == 0, "Kart Racing Pro plugin id changed");
    CHECK(GAME_MXB.plugin_data_version == 8,  "MX Bikes data version changed");
    CHECK(GAME_GPB.plugin_data_version == 12, "GP Bikes data version changed");
    CHECK(GAME_KRP.plugin_data_version == 6,  "Kart Racing Pro data version changed");
    CHECK(kPluginInterfaceVersion == 9, "the shared interface version changed");

    for (const GameOffsets* a : ALL_GAMES) {
        CHECK(a->plugin_id && *a->plugin_id, "%s has no plugin id", a->id);
        for (const GameOffsets* b : ALL_GAMES)
            if (a != b)
                CHECK(std::strcmp(a->plugin_id, b->plugin_id) != 0,
                      "%s and %s claim the same plugin id '%s'", a->id, b->id, a->plugin_id);
    }
}

// Kart Racing Pro ships with the plugin ABI ported and nothing else. Asserting the
// *absence* is the point: a later edit that fills in content_init from somewhere other
// than a capture run on the title has to come past this.
static void krp_is_plugin_only_until_its_offsets_are_derived() {
    CHECK(std::strcmp(GAME_KRP.exe, "kart.exe") == 0, "Kart Racing Pro exe changed");
    CHECK(!GAME_KRP.content_derived(),
          "Kart Racing Pro has content offsets now - if they came from a capture run on the "
          "game, update tasks/kart-racing-pro-port.md and this check together");
    CHECK(!GAME_KRP.reload_steps, "Kart Racing Pro has a reload table but no derivation");
    CHECK(!GAME_KRP.reload_verified, "Kart Racing Pro cannot have a verified table");
    CHECK(!GAME_KRP.offsets_complete, "Kart Racing Pro's offsets are not complete");
}

// The GHS close guard writes a detour into the game from constants in offsets.h, and it
// decodes a pointer out of the bytes those constants describe. Everything about that has
// to agree with itself or the guard points somewhere arbitrary - which is a worse crash
// than the one it exists to stop. These are the invariants the code assumes and cannot
// check for itself at runtime.
static void ghs_guard_constants_agree() {
    const size_t sigLen  = sizeof(mxb::SIG_GHS_CLOSE) - 1;       // minus the NUL
    const size_t maskLen = sizeof(mxb::SIG_GHS_CLOSE_MASK) - 1;
    CHECK(sigLen == maskLen,
          "the signature is %zu bytes and its mask covers %zu", sigLen, maskLen);

    // The wildcards are the lea's disp32 and nothing else: the fixed run must end exactly
    // where the disp starts, and the disp must be the last four bytes of the pattern.
    size_t fixed = 0;
    while (fixed < maskLen && mxb::SIG_GHS_CLOSE_MASK[fixed] == 'x') ++fixed;
    CHECK(fixed == mxb::GHS_LEA_DISP_OFF,
          "the fixed bytes run to %zu but the disp32 is said to start at 0x%zx",
          fixed, mxb::GHS_LEA_DISP_OFF);
    for (size_t i = fixed; i < maskLen; ++i)
        CHECK(mxb::SIG_GHS_CLOSE_MASK[i] == '?', "mask byte %zu after the fixed run is not a wildcard", i);
    CHECK(maskLen - fixed == 4, "a disp32 is 4 bytes; the mask wildcards %zu", maskLen - fixed);

    // RIP-relative: the displacement is added to the address of the NEXT instruction.
    CHECK(mxb::GHS_LEA_END_OFF == mxb::GHS_LEA_DISP_OFF + 4,
          "the lea ends at 0x%zx, which is not its disp32 (0x%zx) plus four",
          mxb::GHS_LEA_END_OFF, mxb::GHS_LEA_DISP_OFF);

    // `dec ecx; cmp ecx,9; ja` - ten slots, and the signature must carry that compare or
    // the guard's own range check is asserting something the code doesn't do.
    CHECK(mxb::GHS_SLOTS == 10, "the pool is ten slots, not %d", mxb::GHS_SLOTS);
    CHECK((unsigned char)mxb::SIG_GHS_CLOSE[6] == 0x83 &&
          (unsigned char)mxb::SIG_GHS_CLOSE[7] == 0xF9 &&
          (unsigned char)mxb::SIG_GHS_CLOSE[8] == (unsigned char)(mxb::GHS_SLOTS - 1),
          "the signature's `cmp ecx,%d` does not match GHS_SLOTS", mxb::GHS_SLOTS - 1);

    // Both RVAs are MX Bikes': they must sit inside the module the guard is placed in,
    // and the close must be in code while the table is not.
    CHECK(mxb::RVA_GHS_CLOSE > 0 && mxb::RVA_GHS_CLOSE < mxb::RVA_GHS_TABLE,
          "the close (0x%zx) should be in .text, well below the table (0x%zx)",
          mxb::RVA_GHS_CLOSE, mxb::RVA_GHS_TABLE);
}

// The terrain guard stands in front of a function whose signature we inferred from its
// prologue, and the inference is load-bearing: `int32 f(obj, a2, float* out, float x, float y)`
// puts `y` at [rsp+0x170] only because the frame is `push rbx` + `sub rsp,0x140`. If a future
// build changes that, the argument we test for NaN is no longer the argument the function
// reads, and the guard would be refusing queries at random. These tie the claim to the bytes,
// so a build that moves the frame fails the signature and turns the guard off instead.
static void terrain_guard_constants_agree() {
    const size_t sigLen  = sizeof(mxb::SIG_TERRAIN_SAMPLE) - 1;
    const size_t maskLen = sizeof(mxb::SIG_TERRAIN_SAMPLE_MASK) - 1;
    CHECK(sigLen == maskLen,
          "the signature is %zu bytes and its mask covers %zu", sigLen, maskLen);
    for (size_t i = 0; i < maskLen; ++i)
        CHECK(mxb::SIG_TERRAIN_SAMPLE_MASK[i] == 'x',
              "byte %zu is wildcarded, but this prologue has no relocated operand in it", i);

    // `mov [rsp+0x18], r8` - arg3 homed to its shadow slot, which is what makes the
    // out-pointer the third argument rather than something read off the stack.
    const unsigned char* sig = (const unsigned char*)mxb::SIG_TERRAIN_SAMPLE;
    CHECK(sig[0] == 0x4C && sig[1] == 0x89 && sig[2] == 0x44 && sig[3] == 0x24 && sig[4] == 0x18,
          "the signature does not start with `mov [rsp+0x18], r8`");

    // `sub rsp, 0x140`. The whole argument mapping rests on this number: arg5 lands at
    // [rsp+0x170] = entry [rsp+0x28] only for a frame of 0x140 plus the pushed rbx.
    CHECK(sig[10] == 0x53, "no `push rbx` where the frame calculation assumes one");
    CHECK(sig[11] == 0x48 && sig[12] == 0x81 && sig[13] == 0xEC &&
          sig[14] == 0x40 && sig[15] == 0x01 && sig[16] == 0x00 && sig[17] == 0x00,
          "the frame is not `sub rsp, 0x140`, so arg5 is not at [rsp+0x170]");

    // `mov r9, [rcx+0x750]` - the grid pointer this function null-checks, which is also
    // where OFF_TERRAIN_GRID says it is.
    CHECK(sig[18] == 0x4C && sig[19] == 0x8B && sig[20] == 0x89,
          "the signature does not load the grid into r9 where we said it does");
    CHECK((size_t)(sig[21] | (sig[22] << 8) | (sig[23] << 16) | (sig[24] << 24)) ==
              mxb::OFF_TERRAIN_GRID,
          "the grid offset in the bytes is not OFF_TERRAIN_GRID (0x%zx)", mxb::OFF_TERRAIN_GRID);

    // The reported faulting instruction has to be inside the function we are guarding, or
    // we are guarding the wrong one. fcn.1401f1720 is 1515 bytes.
    CHECK(mxb::RVA_TERRAIN_FAULT > mxb::RVA_TERRAIN_SAMPLE &&
              mxb::RVA_TERRAIN_FAULT - mxb::RVA_TERRAIN_SAMPLE < 0x600,
          "0x%zx is not inside the sampler at 0x%zx",
          mxb::RVA_TERRAIN_FAULT, mxb::RVA_TERRAIN_SAMPLE);

    // Width and height are adjacent dwords, and the grid pointer is past both.
    CHECK(mxb::OFF_TERRAIN_HEIGHT == mxb::OFF_TERRAIN_WIDTH + 4,
          "width and height are not adjacent dwords");
    CHECK(mxb::OFF_TERRAIN_GRID > mxb::OFF_TERRAIN_HEIGHT, "the grid should sit past the dims");
}

static void rut_diagnostic_constants_agree() {
    CHECK(mxb::MXB_BETA21E_TIMESTAMP == 0x6A21833D, "rut diagnostic build stamp changed");
    CHECK(mxb::RVA_TERRAIN_DEFORM_POINT == 0x1F5AC0, "rut writer RVA changed");
    CHECK(mxb::RVA_TERRAIN_APPLY_BLOCK == 0x1F60C0, "rut apply RVA changed");
    const size_t sigLen = sizeof(mxb::SIG_TERRAIN_DEFORM_POINT) - 1;
    const size_t maskLen = sizeof(mxb::SIG_TERRAIN_DEFORM_POINT_MASK) - 1;
    CHECK(sigLen == 32 && sigLen == maskLen, "rut signature/mask lengths disagree");
    for (size_t i = 0; i < maskLen; ++i)
        CHECK(mxb::SIG_TERRAIN_DEFORM_POINT_MASK[i] == 'x',
              "rut signature byte %zu is unexpectedly wildcarded", i);
    const auto* sig = reinterpret_cast<const unsigned char*>(mxb::SIG_TERRAIN_DEFORM_POINT);
    CHECK(sig[11] == 0x4C && sig[12] == 0x8B && sig[13] == 0xB9 &&
          static_cast<size_t>(sig[14] | (sig[15] << 8) | (sig[16] << 16) | (sig[17] << 24)) ==
              mxb::OFF_TERRAIN_OUTGOING_DELTA,
          "rut signature no longer loads obj+OFF_TERRAIN_OUTGOING_DELTA");
    CHECK(mxb::OFF_TERRAIN_DIRTY_OUTGOING == mxb::OFF_TERRAIN_OUTGOING_DELTA + 8,
          "outgoing grid and dirty array layout changed");
    CHECK(!mxb::LooksDetoured(reinterpret_cast<const uint8_t*>(mxb::SIG_TERRAIN_DEFORM_POINT)),
          "stored rut signature looks like a detour");
    CHECK(sizeof(mxb::SIG_TERRAIN_APPLY_BLOCK) - 1 == 32 &&
          sizeof(mxb::SIG_TERRAIN_APPLY_BLOCK_MASK) - 1 == 32,
          "rut apply signature/mask lengths disagree");
    CHECK(!mxb::LooksDetoured(reinterpret_cast<const uint8_t*>(mxb::SIG_TERRAIN_APPLY_BLOCK)),
          "stored rut apply signature looks like a detour");
}

// A patched prologue must never be mistaken for "the game moved this function".
static void detours_are_recognised_before_the_signature_check() {
    const uint8_t real[]   = {0x48,0x83,0xEC,0x28,0xFF,0xC9,0x83,0xF9,0x09,0x77,0x43,0x48};
    const uint8_t rel32[]  = {0xE9,0x11,0x22,0x33,0x44,0xC9,0x83,0xF9,0x09,0x77,0x43,0x48};
    const uint8_t indir[]  = {0xFF,0x25,0x00,0x00,0x00,0x00,0x11,0x22,0x33,0x44,0x55,0x66};
    const uint8_t movjmp[] = {0x48,0xB8,1,2,3,4,5,6,7,8,0xFF,0xE0};

    CHECK(!mxb::LooksDetoured(real), "the real close prologue is not a detour");
    CHECK(mxb::LooksDetoured(rel32), "jmp rel32 is a detour");
    CHECK(mxb::LooksDetoured(indir), "jmp [rip+disp32] is a detour");
    CHECK(mxb::LooksDetoured(movjmp), "mov rax,imm64 + jmp rax is a detour");

    // The signature's own first bytes are the real prologue, so the two can never disagree.
    CHECK(!mxb::LooksDetoured((const uint8_t*)mxb::SIG_GHS_CLOSE),
          "the stored signature must not look like a detour");
}

// The server-browser reset writes to two `.data` addresses and calls the bus. Every one of
// those is a wild write if a constant drifts, so pin them here: this test is the only thing
// between a typo and a stray dword in the running game.
static void world_session_constants_agree() {
    CHECK(mxb::RVA_WORLD_STATE  == 0x3D7900, "world state moved");
    CHECK(mxb::RVA_WORLD_REASON == 0x3D7E6C, "world reason moved");
    CHECK(mxb::RVA_WORLD_PORT   == 0x3D78F0, "world port moved");
    CHECK(mxb::CMD_WORLD_CLOSE  == 0x386,    "the teardown bus command moved");

    // The three live in the same block the teardown zeroes — except the port, which is the
    // whole reason the reset has to clear it separately.
    CHECK(mxb::RVA_WORLD_REASON > mxb::RVA_WORLD_STATE &&
          mxb::RVA_WORLD_REASON < mxb::RVA_WORLD_STATE + 0x5D8,
          "reason must sit inside the 0x5D8 block the teardown zeroes");
    CHECK(mxb::RVA_WORLD_PORT < mxb::RVA_WORLD_STATE,
          "the port must sit outside that block, or clearing it separately is pointless");

    // 4 is 'logged in', the state the teardown demands before it will send LOGOUT. If this
    // ever stops being the top of the range, the reset stops sending one.
    CHECK(mxb::WORLD_STATE_MAX == 4, "the highest world state moved");
}

int main() {
    terrain_guard_constants_agree();
    rut_diagnostic_constants_agree();
    world_session_constants_agree();
    ghs_guard_constants_agree();
    detours_are_recognised_before_the_signature_check();
    mx_table_is_unchanged();
    gp_table_is_all_self_contained();
    unconfirmed_tables_label_every_step();
    only_confirmed_tables_run_unprompted();
    no_title_borrows_another_titles_offsets();
    every_title_is_internally_consistent();
    ids_match_what_mxb_app_sends();
    plugin_handshake_is_each_titles_own();
    krp_is_plugin_only_until_its_offsets_are_derived();

    if (g_failures) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("offsets: all checks passed (%d MX steps, %d GP steps)\n",
                mxb::kReloadStepCount, gpb::kReloadStepCount);
    return 0;
}
