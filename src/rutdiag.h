#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cmath>

namespace frostmod::rutdiag {

struct Config {
    bool enabled = false;
    uint64_t burst = 48;
    uint64_t sampleEvery = 256;
};

inline bool ShouldLog(uint64_t call, const Config& cfg) {
    if (!cfg.enabled || call == 0) return false;
    if (call <= cfg.burst) return true;
    return cfg.sampleEvery != 0 && call % cfg.sampleEvery == 0;
}

struct Geometry {
    int width = 0;
    int height = 0;
    int blockWidth = 0;
    int blockHeight = 0;
    int blocksPerRow = 0;
    int blockCount = 0;
    float originX = 0;
    float originY = 0;
    float sizeX = 0;
    float sizeY = 0;
};

struct Footprint {
    bool valid = false;
    int x[4]{};
    int y[4]{};
    uint64_t cell[4]{};
    int block[4]{};
};

struct BlockShape {
    bool valid = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

constexpr int32_t kNegativePulseDelta = -0x00040000;

enum class PulseState : uint8_t {
    Waiting,
    Claimed,
    Fired,
};

inline bool TryClaimPulse(std::atomic<PulseState>& state) {
    PulseState expected = PulseState::Waiting;
    return state.compare_exchange_strong(expected, PulseState::Claimed,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire);
}

inline void ReleasePulse(std::atomic<PulseState>& state) {
    state.store(PulseState::Waiting, std::memory_order_release);
}

inline void CompletePulse(std::atomic<PulseState>& state) {
    state.store(PulseState::Fired, std::memory_order_release);
}

struct PulseCandidate {
    bool valid = false;
    int x = 0;
    int y = 0;
    uint64_t cell = 0;
    int block = 0;
};

inline BlockShape MapBlock(const Geometry& g, int blockIndex) {
    BlockShape out;
    if (g.width <= 0 || g.height <= 0 || g.blockWidth <= 0 || g.blockHeight <= 0 ||
        g.blocksPerRow <= 0 || g.blockCount <= 0 || blockIndex < 0 ||
        blockIndex >= g.blockCount)
        return out;
    const int blocksPerCol = (g.height + g.blockHeight - 1) / g.blockHeight;
    if (static_cast<int64_t>(blockIndex) >=
        static_cast<int64_t>(g.blocksPerRow) * blocksPerCol) return out;
    const int blockX = blockIndex % g.blocksPerRow;
    const int blockY = blockIndex / g.blocksPerRow;
    out.x = blockX * g.blockWidth;
    out.y = blockY * g.blockHeight;
    out.width = g.width - out.x < g.blockWidth ? g.width - out.x : g.blockWidth;
    out.height = g.height - out.y < g.blockHeight ? g.height - out.y : g.blockHeight;
    out.valid = out.width > 0 && out.height > 0;
    return out;
}

// Mirror only the writer's coordinate-to-four-cells arithmetic. This does not decide
// whether the game accepts the material or magnitude; the pre/post snapshot reveals that.
inline Footprint MapFootprint(const Geometry& g, float worldX, float worldY) {
    Footprint out;
    if (g.width <= 0 || g.height <= 0 || g.blockWidth <= 0 || g.blockHeight <= 0 ||
        g.blocksPerRow <= 0 || g.blockCount <= 0 || !std::isfinite(worldX) || !std::isfinite(worldY) ||
        !std::isfinite(g.originX) || !std::isfinite(g.originY) ||
        !std::isfinite(g.sizeX) || !std::isfinite(g.sizeY) ||
        g.sizeX <= 0.0f || g.sizeY <= 0.0f ||
        worldX < g.originX || worldY < g.originY ||
        worldX > g.originX + g.sizeX || worldY > g.originY + g.sizeY)
        return out;

    const float gx = (worldX - g.originX) / g.sizeX * static_cast<float>(g.width - 1);
    const float gy = (worldY - g.originY) / g.sizeY * static_cast<float>(g.height - 1);
    int x0 = static_cast<int>(gx);
    int y0 = static_cast<int>(gy);
    if (x0 < 0 || y0 < 0 || x0 >= g.width || y0 >= g.height) return out;
    int x1 = x0 + 1 < g.width ? x0 + 1 : x0;
    int y1 = y0 + 1 < g.height ? y0 + 1 : y0;
    out.x[0] = x0; out.y[0] = y0;
    out.x[1] = x1; out.y[1] = y0;
    out.x[2] = x0; out.y[2] = y1;
    out.x[3] = x1; out.y[3] = y1;
    for (int i = 0; i < 4; ++i) {
        out.cell[i] = static_cast<uint64_t>(out.y[i]) * static_cast<uint64_t>(g.width) +
                      static_cast<uint64_t>(out.x[i]);
        out.block[i] = (out.y[i] / g.blockHeight) * g.blocksPerRow +
                       (out.x[i] / g.blockWidth);
        if (out.block[i] < 0 || out.block[i] >= g.blockCount) return Footprint{};
    }
    out.valid = true;
    return out;
}

// Select one cardinally adjacent terrain cell outside the stock four-cell footprint. The
// accepted stock writer has already dirtied its own block, so a pulse candidate must live in
// a different block which is still clear and entirely zero. This stronger whole-block check
// prevents the manual signed test from sharing a byte-wise network merge with pending data.
inline PulseCandidate SelectNegativePulseCandidate(const Geometry& g, const Footprint& f,
                                                    const int32_t* outgoing,
                                                    size_t outgoingCount,
                                                    const uint8_t* dirty,
                                                    size_t dirtyCount) {
    PulseCandidate none;
    if (!f.valid || !outgoing || !dirty || g.width <= 2 || g.height <= 2 ||
        g.blockWidth <= 0 || g.blockHeight <= 0 || g.blocksPerRow <= 0 ||
        g.blockCount <= 0 || outgoingCount < static_cast<size_t>(g.width) * g.height ||
        dirtyCount < static_cast<size_t>(g.blockCount))
        return none;

    static constexpr int kDirection[4][2] = {
        {-1, 0}, {1, 0}, {0, -1}, {0, 1},
    };
    uint64_t considered[16]{};
    int consideredCount = 0;
    for (int source = 0; source < 4; ++source) {
        for (const auto& direction : kDirection) {
            const int x = f.x[source] + direction[0];
            const int y = f.y[source] + direction[1];
            if (x <= 0 || y <= 0 || x >= g.width - 1 || y >= g.height - 1) continue;
            const uint64_t cell = static_cast<uint64_t>(y) * g.width + x;
            bool duplicate = false;
            for (int i = 0; i < consideredCount; ++i)
                if (considered[i] == cell) duplicate = true;
            if (duplicate) continue;
            considered[consideredCount++] = cell;

            bool inFootprint = false;
            for (int i = 0; i < 4; ++i)
                if (f.cell[i] == cell) inFootprint = true;
            if (inFootprint) continue;

            const int block = (y / g.blockHeight) * g.blocksPerRow + (x / g.blockWidth);
            if (block < 0 || block >= g.blockCount || dirty[block] != 0 || outgoing[cell] != 0)
                continue;
            bool stockBlock = false;
            for (int i = 0; i < 4; ++i)
                if (f.block[i] == block) stockBlock = true;
            if (stockBlock) continue;

            const BlockShape shape = MapBlock(g, block);
            if (!shape.valid) continue;
            bool blockIsZero = true;
            for (int row = 0; row < shape.height && blockIsZero; ++row) {
                const uint64_t start = static_cast<uint64_t>(shape.y + row) * g.width + shape.x;
                for (int col = 0; col < shape.width; ++col) {
                    if (outgoing[start + col] != 0) {
                        blockIsZero = false;
                        break;
                    }
                }
            }
            if (!blockIsZero) continue;
            return {true, x, y, cell, block};
        }
    }
    return none;
}

} // namespace frostmod::rutdiag
