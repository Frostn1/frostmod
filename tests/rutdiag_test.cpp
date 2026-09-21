#include "../src/rutdiag.h"
#include <cstdio>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); ++failures; } } while (0)

static frostmod::rutdiag::Geometry geometry() {
    return {2049, 2049, 64, 64, 33, 1089, 10.0f, 20.0f, 348.16f, 348.16f};
}

static void defaults_are_off_and_logging_is_bounded() {
    frostmod::rutdiag::Config cfg;
    CHECK(!cfg.enabled, "rut diagnostics must default off");
    CHECK(!frostmod::rutdiag::ShouldLog(1, cfg), "disabled diagnostics logged a call");
    cfg.enabled = true;
    CHECK(frostmod::rutdiag::ShouldLog(1, cfg), "first diagnostic call should log");
    CHECK(frostmod::rutdiag::ShouldLog(48, cfg), "last burst call should log");
    CHECK(!frostmod::rutdiag::ShouldLog(49, cfg), "call after burst should be suppressed");
    CHECK(frostmod::rutdiag::ShouldLog(256, cfg), "periodic sample should log");
    cfg.sampleEvery = 0;
    CHECK(!frostmod::rutdiag::ShouldLog(256, cfg), "zero sample period should disable periodic logs");
}

static void footprint_maps_four_cells_and_blocks() {
    auto g = geometry();
    auto f = frostmod::rutdiag::MapFootprint(g, 10.0f + 64.25f * (348.16f / 2048.0f),
                                                20.0f + 63.75f * (348.16f / 2048.0f));
    CHECK(f.valid, "valid terrain point was rejected");
    CHECK(f.x[0] == 64 && f.y[0] == 63 && f.x[3] == 65 && f.y[3] == 64,
          "wrong four-cell footprint: (%d,%d)..(%d,%d)", f.x[0], f.y[0], f.x[3], f.y[3]);
    CHECK(f.block[0] == 1 && f.block[1] == 1 && f.block[2] == 34 && f.block[3] == 34,
          "block-boundary dirty set was wrong: %d %d %d %d",
          f.block[0], f.block[1], f.block[2], f.block[3]);
}

static void footprint_rejects_bad_inputs_and_clamps_the_upper_edge() {
    auto g = geometry();
    CHECK(!frostmod::rutdiag::MapFootprint(g, 9.0f, 20.0f).valid, "off-map point was accepted");
    CHECK(!frostmod::rutdiag::MapFootprint(g, NAN, 20.0f).valid, "NaN point was accepted");
    auto edge = frostmod::rutdiag::MapFootprint(g, g.originX + g.sizeX, g.originY + g.sizeY);
    CHECK(edge.valid, "upper edge should be a valid clamped footprint");
    for (int i = 0; i < 4; ++i)
        CHECK(edge.x[i] == 2048 && edge.y[i] == 2048, "upper edge was not clamped");
}

static void block_shape_clamps_track_edges() {
    auto g = geometry();
    auto interior = frostmod::rutdiag::MapBlock(g, 34);
    CHECK(interior.valid && interior.x == 64 && interior.y == 64 &&
          interior.width == 64 && interior.height == 64,
          "interior block geometry was wrong");
    auto corner = frostmod::rutdiag::MapBlock(g, 1088);
    CHECK(corner.valid && corner.x == 2048 && corner.y == 2048 &&
          corner.width == 1 && corner.height == 1,
          "corner block was not clamped to one cell");
    CHECK(!frostmod::rutdiag::MapBlock(g, 1089).valid, "out-of-range block was accepted");
    g.blockCount = 100;
    CHECK(!frostmod::rutdiag::MapBlock(g, 100).valid, "stored block-count limit was ignored");
}

int main() {
    defaults_are_off_and_logging_is_bounded();
    footprint_maps_four_cells_and_blocks();
    footprint_rejects_bad_inputs_and_clamps_the_upper_edge();
    block_shape_clamps_track_edges();
    if (failures) return 1;
    std::puts("rutdiag tests passed");
    return 0;
}
