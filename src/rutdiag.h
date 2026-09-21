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
    Selecting,
    Armed,
    Injecting,
    Fired,
};

inline bool TryBeginPulseSelection(std::atomic<PulseState>& state) {
    PulseState expected = PulseState::Waiting;
    return state.compare_exchange_strong(expected, PulseState::Selecting,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire);
}

inline void ArmPulse(std::atomic<PulseState>& state) {
    state.store(PulseState::Armed, std::memory_order_release);
}

inline bool TryClaimPulseInjection(std::atomic<PulseState>& state) {
    PulseState expected = PulseState::Armed;
    return state.compare_exchange_strong(expected, PulseState::Injecting,
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

enum class SerializationDecision : uint8_t {
    NotTarget,
    Inject,
    RefuseInvalid,
    RefuseDirty,
    RefuseNonzero,
};

enum class ApplyResult : uint8_t {
    Exact,
    Mismatch,
    Missing,
};

inline ApplyResult ClassifyApplyResult(int32_t delta) {
    if (delta == kNegativePulseDelta) return ApplyResult::Exact;
    if (delta == 0) return ApplyResult::Missing;
    return ApplyResult::Mismatch;
}

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

inline bool SpanContainsCell(const void* nextIn, size_t available, const int32_t* cell) {
    if (!nextIn || !cell || available < sizeof(*cell)) return false;
    const uintptr_t first = reinterpret_cast<uintptr_t>(nextIn);
    const uintptr_t target = reinterpret_cast<uintptr_t>(cell);
    if (first > UINTPTR_MAX - available || target > UINTPTR_MAX - sizeof(*cell)) return false;
    return target >= first && target + sizeof(*cell) <= first + available;
}

// Called only after the serializer hook has established that this is the target row. The
// selected block must still be the clean block we armed: its dirty byte is set only to make
// the stock sender visit it, while every cell remains zero until this exact boundary.
inline SerializationDecision ClassifySerializationInjection(
    const Geometry& g, const PulseCandidate& candidate,
    const int32_t* outgoing, size_t outgoingCount,
    const uint8_t* dirty, size_t dirtyCount,
    const void* nextIn, size_t available) {
    if (!candidate.valid || !outgoing || !dirty || candidate.cell >= outgoingCount ||
        candidate.block < 0 || static_cast<size_t>(candidate.block) >= dirtyCount)
        return SerializationDecision::RefuseInvalid;
    if (!SpanContainsCell(nextIn, available, outgoing + candidate.cell))
        return SerializationDecision::NotTarget;
    const BlockShape shape = MapBlock(g, candidate.block);
    if (!shape.valid || candidate.x < shape.x || candidate.x >= shape.x + shape.width ||
        candidate.y < shape.y || candidate.y >= shape.y + shape.height ||
        candidate.cell != static_cast<uint64_t>(candidate.y) * g.width + candidate.x)
        return SerializationDecision::RefuseInvalid;
    if (dirty[candidate.block] != 1) return SerializationDecision::RefuseDirty;
    for (int row = 0; row < shape.height; ++row) {
        const uint64_t start = static_cast<uint64_t>(shape.y + row) * g.width + shape.x;
        if (start + static_cast<uint64_t>(shape.width) > outgoingCount)
            return SerializationDecision::RefuseInvalid;
        for (int col = 0; col < shape.width; ++col)
            if (outgoing[start + col] != 0) return SerializationDecision::RefuseNonzero;
    }
    return SerializationDecision::Inject;
}

inline SerializationDecision InjectAtSerialization(
    const Geometry& g, const PulseCandidate& candidate,
    int32_t* outgoing, size_t outgoingCount,
    const uint8_t* dirty, size_t dirtyCount,
    const void* nextIn, size_t available) {
    const SerializationDecision decision = ClassifySerializationInjection(
        g, candidate, outgoing, outgoingCount, dirty, dirtyCount, nextIn, available);
    if (decision == SerializationDecision::Inject)
        outgoing[candidate.cell] = kNegativePulseDelta;
    return decision;
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
