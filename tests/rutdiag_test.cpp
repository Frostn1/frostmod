#include "../src/rutdiag.h"
#include <cstdio>
#include <vector>

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

static void negative_pulse_requires_an_adjacent_zero_cell_in_a_clean_zero_block() {
    frostmod::rutdiag::Geometry g{130, 130, 64, 64, 3, 9, 0, 0, 129, 129};
    frostmod::rutdiag::Footprint f;
    f.valid = true;
    const int x[4] = {62, 63, 62, 63};
    const int y[4] = {10, 10, 11, 11};
    for (int i = 0; i < 4; ++i) {
        f.x[i] = x[i];
        f.y[i] = y[i];
        f.cell[i] = static_cast<uint64_t>(y[i]) * g.width + x[i];
        f.block[i] = 0;
    }
    std::vector<int32_t> outgoing(static_cast<size_t>(g.width) * g.height, 0);
    std::vector<uint8_t> dirty(g.blockCount, 0);
    dirty[0] = 1; // the accepted stock footprint's block

    auto candidate = frostmod::rutdiag::SelectNegativePulseCandidate(
        g, f, outgoing.data(), outgoing.size(), dirty.data(), dirty.size());
    CHECK(candidate.valid && candidate.x == 64 && candidate.y == 10 &&
          candidate.block == 1 && candidate.cell == 10u * 130u + 64u,
          "wrong clean adjacent pulse candidate: valid=%d cell=%llu block=%d",
          candidate.valid ? 1 : 0, (unsigned long long)candidate.cell, candidate.block);

    dirty[candidate.block] = 1;
    CHECK(!frostmod::rutdiag::SelectNegativePulseCandidate(
              g, f, outgoing.data(), outgoing.size(), dirty.data(), dirty.size()).valid,
          "dirty target block was accepted");
    dirty[candidate.block] = 0;
    outgoing[12u * 130u + 65u] = 7;
    CHECK(!frostmod::rutdiag::SelectNegativePulseCandidate(
              g, f, outgoing.data(), outgoing.size(), dirty.data(), dirty.size()).valid,
          "nonzero peer in the target block was accepted");
    outgoing[12u * 130u + 65u] = 0;
    outgoing[candidate.cell] = 1;
    CHECK(!frostmod::rutdiag::SelectNegativePulseCandidate(
              g, f, outgoing.data(), outgoing.size(), dirty.data(), dirty.size()).valid,
          "nonzero target cell was accepted");
    outgoing[candidate.cell] = 0;
    g.blockWidth = 0;
    CHECK(!frostmod::rutdiag::SelectNegativePulseCandidate(
              g, f, outgoing.data(), outgoing.size(), dirty.data(), dirty.size()).valid,
          "invalid block geometry was accepted");
}

static void negative_pulse_latch_fires_once_and_can_release_a_refused_claim() {
    using frostmod::rutdiag::PulseState;
    std::atomic<PulseState> state{PulseState::Waiting};
    CHECK(frostmod::rutdiag::TryClaimPulse(state), "waiting pulse could not be claimed");
    CHECK(!frostmod::rutdiag::TryClaimPulse(state), "claimed pulse was claimed twice");
    frostmod::rutdiag::ReleasePulse(state);
    CHECK(frostmod::rutdiag::TryClaimPulse(state), "refused claim did not return to waiting");
    frostmod::rutdiag::CompletePulse(state);
    CHECK(!frostmod::rutdiag::TryClaimPulse(state), "completed pulse fired twice");
    CHECK(state.load() == PulseState::Fired, "completed pulse did not stay fired");
    CHECK(frostmod::rutdiag::kNegativePulseDelta == -262144, "pulse magnitude changed");
}

int main() {
    defaults_are_off_and_logging_is_bounded();
    footprint_maps_four_cells_and_blocks();
    footprint_rejects_bad_inputs_and_clamps_the_upper_edge();
    block_shape_clamps_track_edges();
    negative_pulse_requires_an_adjacent_zero_cell_in_a_clean_zero_block();
    negative_pulse_latch_fires_once_and_can_release_a_refused_claim();
    if (failures) return 1;
    std::puts("rutdiag tests passed");
    return 0;
}
