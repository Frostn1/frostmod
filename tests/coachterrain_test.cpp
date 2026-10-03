// The game's own ground under the line (src/coachterrain.h): the sampler's signature with and
// without FrostMod's guard in front of it, the track slots decoded out of the accessor's lea, a
// slot's heightfield header, the sampling plan, and the grid made a slice at a time from a fake
// height query. Pure C++: no game, no Win32, synthetic bytes only.

#include "../src/coachterrain.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            ++g_failures;                                    \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n  (%s)\n", #cond);                \
        }                                                    \
    } while (0)

using namespace coachterrain;

static std::vector<uint8_t> SamplerBytes() {
    const size_t n = sizeof(mxb::SIG_TERRAIN_SAMPLE) - 1;
    std::vector<uint8_t> b(n + 16, 0xCC);
    std::memcpy(b.data(), mxb::SIG_TERRAIN_SAMPLE, n);
    return b;
}

static void the_sampler_is_recognised_bare_and_behind_the_guard() {
    std::vector<uint8_t> b = SamplerBytes();
    CHECK(CheckSampler(b.data()) == Sampler::PLAIN, "the prologue as beta21e has it");
    // FrostMod's guard: `jmp rel32` over the five bytes of `mov [rsp+0x18], r8`.
    b[0] = 0xE9, b[1] = 0x10, b[2] = 0x20, b[3] = 0x30, b[4] = 0xF0;
    CHECK(CheckSampler(b.data()) == Sampler::BEHIND_HOOK, "behind a jmp, the rest unchanged");
    // A jmp and a different function after it is not the sampler.
    b[20] ^= 0xFF;
    CHECK(CheckSampler(b.data()) == Sampler::NONE, "a changed body is refused");
    // Somebody else's detour (`jmp [rip+disp32]`) is six bytes and is not ours to call through.
    b = SamplerBytes();
    b[0] = 0xFF, b[1] = 0x25;
    CHECK(CheckSampler(b.data()) == Sampler::NONE, "another kind of hook is refused");
    // A build that moves the frame (`sub rsp, 0x150`) moves the arguments: refused.
    b    = SamplerBytes();
    b[14] = 0x50;
    CHECK(CheckSampler(b.data()) == Sampler::NONE, "a different frame is refused");
}

static std::vector<uint8_t> AccessorBytes(int32_t disp) {
    const size_t n = sizeof(mxb::SIG_TRACK_SLOT_ACCESSOR) - 1;
    std::vector<uint8_t> b(n, 0);
    std::memcpy(b.data(), mxb::SIG_TRACK_SLOT_ACCESSOR, n);
    std::memcpy(b.data() + mxb::TRACK_SLOT_LEA_DISP_OFF, &disp, 4);
    return b;
}

static void the_slots_come_out_of_the_accessors_own_lea() {
    for (int32_t disp : {0x3ACF03, -0x1000, 0}) {
        std::vector<uint8_t> b = AccessorBytes(disp);
        CHECK(MatchAt(b.data(), mxb::SIG_TRACK_SLOT_ACCESSOR, mxb::SIG_TRACK_SLOT_ACCESSOR_MASK),
              "any displacement matches (disp %d)", disp);
        CHECK(DecodeSlots(b.data()) == b.data() + mxb::TRACK_SLOT_LEA_END_OFF + disp, "decoded from the lea's end");
    }
    // beta21e: the lea at RVA 0x1BB57E decodes to 0x568488.
    CHECK(mxb::RVA_TRACK_SLOT_ACCESSOR + mxb::TRACK_SLOT_LEA_END_OFF + 0x3ACF03 == mxb::RVA_TRACK_SLOTS,
          "the recorded RVAs agree with the recorded displacement");
    // Four slots, or a different stride, is a different array.
    std::vector<uint8_t> b = AccessorBytes(0);
    b[0x08]                = 3;
    CHECK(!MatchAt(b.data(), mxb::SIG_TRACK_SLOT_ACCESSOR, mxb::SIG_TRACK_SLOT_ACCESSOR_MASK), "slot count");
    b       = AccessorBytes(0);
    b[0x1E] = 0x70;
    CHECK(!MatchAt(b.data(), mxb::SIG_TRACK_SLOT_ACCESSOR, mxb::SIG_TRACK_SLOT_ACCESSOR_MASK), "stride");
}

static std::vector<uint8_t> Slot(int cols, int rows, const void* heights, float sx, float sz, float ox, float oz) {
    std::vector<uint8_t> s(mxb::TRACK_SLOT_STRIDE, 0);
    std::memcpy(&s[mxb::OFF_TERRAIN_WIDTH], &cols, 4);
    std::memcpy(&s[mxb::OFF_TERRAIN_HEIGHT], &rows, 4);
    std::memcpy(&s[mxb::OFF_TERRAIN_GRID], &heights, sizeof(void*));
    std::memcpy(&s[mxb::OFF_TERRAIN_SIZE_X], &sx, 4);
    std::memcpy(&s[mxb::OFF_TERRAIN_SIZE_Z], &sz, 4);
    std::memcpy(&s[mxb::OFF_TERRAIN_ORIGIN_X], &ox, 4);
    std::memcpy(&s[mxb::OFF_TERRAIN_ORIGIN_Z], &oz, 4);
    return s;
}

static void a_slot_is_read_as_the_sampler_reads_it() {
    static const int16_t cells[4] = {};
    const std::vector<uint8_t> s = Slot(2049, 2049, cells, 550, 550, 0, 0);
    const Header               h = ReadHeader(s.data());
    CHECK(h.cols == 2049 && h.rows == 2049 && h.heights == cells && h.size_x == 550 && h.origin_z == 0, "fields");
    CHECK(Plausible(h), "a loaded 550 m track");
    CHECK(!Plausible(ReadHeader(std::vector<uint8_t>(mxb::TRACK_SLOT_STRIDE, 0).data())), "an empty slot");
    CHECK(!Plausible(ReadHeader(Slot(2049, 2049, nullptr, 550, 550, 0, 0).data())), "no grid");
    CHECK(!Plausible(ReadHeader(Slot(2049, 2049, cells, NAN, 550, 0, 0).data())), "a NaN size");
    CHECK(!Plausible(ReadHeader(Slot(1, 2049, cells, 550, 550, 0, 0).data())), "one column");
    CHECK(!Plausible(ReadHeader(Slot(2049, 2049, cells, 550, 550, INFINITY, 0).data())), "an infinite origin");
    // A stadium track: small, off-centre.
    CHECK(Plausible(ReadHeader(Slot(513, 513, cells, 160, 120, -80, -60).data())), "a stadium track");
    Header other = h;
    other.heights = cells + 1;
    CHECK(other != h, "a reloaded grid is a different heightfield");
}

static void the_plan_samples_at_half_a_metre_within_the_bounds() {
    static const int16_t cells[4] = {};
    Header               h        = ReadHeader(Slot(2049, 2049, cells, 550, 550, 0, 0).data());
    Plan                 p        = PlanFor(h);
    CHECK(p.step == 0.5f && p.w == 1101 && p.h == 1101, "550 m at 0.5 m: %ux%u at %.3f", p.w, p.h, double(p.step));
    float x, z;
    SamplePoint(p, p.w - 1, p.h - 1, x, z);
    CHECK(x <= 550 && z <= 550 && x > 549.4f, "the last sample is on the far edge, not past it (%f)", double(x));
    // Never finer than the game's own grid.
    h = ReadHeader(Slot(101, 101, cells, 550, 550, 0, 0).data());
    p = PlanFor(h);
    CHECK(std::fabs(p.step - 5.5f) < 1e-4f && p.w == 101, "a coarse grid at its own step: %.3f, %u", double(p.step), p.w);
    // Never more than GroundGrid holds.
    h = ReadHeader(Slot(16385, 16385, cells, 4000, 3000, -2000, -1500).data());
    p = PlanFor(h);
    CHECK(p.w <= coachline::kGridMaxDim && p.h <= coachline::kGridMaxDim, "capped: %ux%u", p.w, p.h);
    SamplePoint(p, 0, 0, x, z);
    CHECK(x == -2000 && z == -1500, "starts at the origin");
    CHECK(PlanFor(Header{}).w == 0, "nothing planned for an empty slot");
}

// A ground with a slope and a dip, which bilinear sampling of the plan's lattice reproduces
// exactly on the lattice and closely between.
static float Ground(float x, float z) { return 3.0f + 0.1f * x - 0.05f * z; }

static Header Track550() {
    static const int16_t       cells[4] = {};
    static std::vector<uint8_t> s       = Slot(2049, 2049, cells, 550, 550, 0, 0);
    return ReadHeader(s.data());
}

static void the_grid_is_made_a_slice_at_a_time() {
    Builder b;
    CHECK(b.start(Track550()), "started");
    int    slices = 0;
    size_t calls  = 0;
    auto   ask    = [&](float x, float z, float& y) {
        ++calls;
        y = Ground(x, z);
        return 1;
    };
    while (b.state() == Builder::RUNNING && slices < 10000) {
        b.run(ask, 50000);
        ++slices;
    }
    CHECK(b.state() == Builder::DONE, "done");
    CHECK(slices == 25, "1101 x 1101 in slices of 50000: %d", slices);
    CHECK(calls == 1101u * 1101u, "each sample asked once: %zu", calls);
    coachline::GroundGrid g = b.take();
    CHECK(b.state() == Builder::IDLE && g.ready(), "handed over");
    float y;
    for (float x : {0.0f, 12.3f, 275.0f, 549.9f})
        for (float z : {0.0f, 7.77f, 400.0f}) {
            CHECK(g.at(x, z, y) && std::fabs(y - Ground(x, z)) < 1e-3f, "ground at (%f, %f): %f", double(x), double(z),
                  double(y));
        }
    CHECK(!g.at(-1, 10, y) && !g.at(10, 551, y), "nothing off the track");
    // And the rider, half a metre above it, lines up with it unshifted.
    coachline::AlignCheck a;
    for (int i = 0; i < coachline::kAlignSamples; ++i) {
        const float x = 20.0f + float(i) * 2.0f, z = 100.0f + float(i % 17) * 3.0f;
        a.add(g, x, Ground(x, z) + 0.5f, z);
    }
    CHECK(a.result().done && a.result().ok && a.result().dx == 0 && a.result().dz == 0 &&
              std::fabs(a.result().dy - 0.5f) < 0.01f,
          "aligned dx=%f dz=%f dy=%f", double(a.result().dx), double(a.result().dz), double(a.result().dy));
}

static void misses_stay_unknown_and_too_few_answers_are_no_ground() {
    Builder b;
    b.start(Track550());
    // A stadium in the corner of the map: only part of it answers.
    auto part = [](float x, float z, float& y) {
        if (x > 200 || z > 200) return 0;
        y = Ground(x, z);
        return 1;
    };
    while (b.state() == Builder::RUNNING) b.run(part, 1 << 20);
    CHECK(b.state() == Builder::DONE, "an eighth of the map answered is still ground");
    coachline::GroundGrid g = b.take();
    float                 y;
    CHECK(g.at(100, 100, y) && !g.at(300, 100, y), "known where it answered, unknown elsewhere");
    // Hardly any answers: not the ground under the track.
    b.start(Track550());
    auto few = [](float x, float z, float& y) {
        if (x > 10 || z > 10) return 0;
        y = 0;
        return 1;
    };
    while (b.state() == Builder::RUNNING) b.run(few, 1 << 20);
    CHECK(b.state() == Builder::FAILED && !b.take().ready(), "too little answered");
    // A NaN answer is no answer.
    b.start(Track550());
    auto nan = [](float, float, float& y) {
        y = NAN;
        return 1;
    };
    while (b.state() == Builder::RUNNING) b.run(nan, 1 << 20);
    CHECK(b.state() == Builder::FAILED, "NaN heights are not ground");
}

static void a_fault_stops_it_for_good() {
    Builder b;
    b.start(Track550());
    size_t calls = 0;
    auto   ask   = [&](float x, float z, float& y) {
        if (++calls == 777) return -1;
        y = Ground(x, z);
        return 1;
    };
    b.run(ask, 500);
    CHECK(b.state() == Builder::RUNNING, "still going");
    b.run(ask, 500);
    CHECK(b.state() == Builder::FAILED && b.asked() < b.total(), "faulted part way: %llu of %llu",
          (unsigned long long)b.asked(), (unsigned long long)b.total());
    CHECK(b.run(ask, 500) == 0 && calls == 777, "never asked again");
    CHECK(!b.take().ready(), "nothing kept");
}

static void a_made_grid_keeps_the_files_limits() {
    coachline::GroundGrid g;
    CHECK(!g.make(1, 10, 1, 0, 0) && !g.ready(), "one column");
    CHECK(!g.make(10, 10, 0.01f, 0, 0), "too fine");
    CHECK(!g.make(10, 10, 1, NAN, 0), "NaN origin");
    CHECK(!g.make(coachline::kGridMaxDim + 1, 10, 1, 0, 0), "too wide");
    CHECK(g.make(3, 3, 1, 0, 0) && g.ready(), "made");
    float y;
    CHECK(!g.at(0.5f, 0.5f, y), "unknown until set");
    for (uint32_t r = 0; r < 3; ++r)
        for (uint32_t c = 0; c < 3; ++c) g.set(c, r, float(c));
    g.set(5, 5, 1);  // out of range: ignored
    CHECK(g.at(0.5f, 0.5f, y) && std::fabs(y - 0.5f) < 1e-6f, "bilinear: %f", double(y));
    g.set(1, 1, INFINITY);
    CHECK(!g.at(0.5f, 0.5f, y), "an infinite height is unknown");
}

int main() {
    the_sampler_is_recognised_bare_and_behind_the_guard();
    the_slots_come_out_of_the_accessors_own_lea();
    a_slot_is_read_as_the_sampler_reads_it();
    the_plan_samples_at_half_a_metre_within_the_bounds();
    the_grid_is_made_a_slice_at_a_time();
    misses_stay_unknown_and_too_few_answers_are_no_ground();
    a_fault_stops_it_for_good();
    a_made_grid_keeps_the_files_limits();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("coachterrain: all passed\n");
    return 0;
}
