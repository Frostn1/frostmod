// Invariants over the quad-batch map guard in mapguard.h.
//
// The two rel32 calls we rewrite, the functions they reach, and the GL pointers and flags
// those functions read are all proved against the bytes the build guard insists on, rather
// than restated. A wrong number here is a call into the middle of something else on every
// frame, so it is checked here and not in game.
//
// Pure constants and arithmetic, so it builds and runs anywhere.

#include "../src/mapguard.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

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

using namespace mapguard;

static constexpr uintptr_t kBase = 0x140000000;

// The rip-relative target of the instruction `off` bytes into a function whose bytes are
// `fn` and which starts at `fnRva`: disp32 at byte `at` of the instruction, `len` bytes long.
static uintptr_t RipTarget(const uint8_t* fn, uintptr_t fnRva, size_t off, size_t at, size_t len) {
    int32_t d = 0;
    for (int i = 0; i < 4; ++i) d |= (int32_t)((uint32_t)fn[off + at + i] << (8 * i));
    return (uintptr_t)((int64_t)(fnRva + off + len) + d);
}

static void TestSites() {
    // The calls sit where we say inside the guarded windows, and go where we say.
    CHECK(kMapCallInWin == 19 && kMapCallInWin + kCallLen == kMapWinLen, "map call is the window's last 5 bytes");
    CHECK(kUnmapCallInWin == 18 && kUnmapCallInWin + kCallLen == kUnmapWinLen, "unmap call is the window's last 5 bytes");
    CHECK(CallTarget(kBase + kRvaMapCall, kMapWin + kMapCallInWin) == kBase + kRvaMapFn,
          "the call at +0x240A9C must reach 0x22AC50");
    CHECK(CallTarget(kBase + kRvaUnmapCall, kUnmapWin + kUnmapCallInWin) == kBase + kRvaUnmapFn,
          "the call at +0x240D1E must reach 0x22ACC0");
    // Both rewrite `rcx = batch + 0xA8`, the record the thunks read.
    CHECK(kMapWin[12] == 0x48 && kMapWin[13] == 0x81 && kMapWin[14] == 0xc1 && kMapWin[15] == 0xa8,
          "map: add rcx, 0xA8");
    CHECK(kUnmapWin[11] == 0x48 && kUnmapWin[12] == 0x8d && kUnmapWin[13] == 0x88 && kUnmapWin[14] == 0xa8,
          "unmap: lea rcx, [rax+0xA8]");

    // 0x22AC50: the two flags, the record fields, glBufferData(…, NULL, STREAM_DRAW) and the
    // tail-jump to glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY).
    CHECK(RipTarget(kMapFnBytes, kRvaMapFn, 0x04, 2, 7) == kRvaGlOk, "map: first flag is kRvaGlOk");
    CHECK(kMapFnBytes[0x14] == 0x83 && kMapFnBytes[0x16] == kVbGlName, "map: cmp [rcx+0x18], 0");
    CHECK(RipTarget(kMapFnBytes, kRvaMapFn, 0x1A, 2, 7) == kRvaUseMap, "map: second flag is kRvaUseMap");
    CHECK(kMapFnBytes[0x28] == 0x8b && kMapFnBytes[0x2A] == kVbStride &&
          kMapFnBytes[0x2B] == 0x0f && kMapFnBytes[0x2C] == 0xaf && kMapFnBytes[0x2D] == 0x19,
          "map: size = [rcx+4] * [rcx]");
    CHECK(kMapFnBytes[0x2E] == 0xe8 &&
          CallTarget(kBase + kRvaMapFn + 0x2E, kMapFnBytes + 0x2E) == kBase + kRvaBindFn,
          "map: binds through 0x2293C0");
    CHECK(kMapFnBytes[0x49] == 0xba && kMapFnBytes[0x4A] == 0xb9 && kMapFnBytes[0x4B] == 0x88,
          "map: edx = GL_WRITE_ONLY (0x88B9)");
    CHECK(kMapFnBytes[0x5A] == 0x48 && kMapFnBytes[0x5B] == 0xff && kMapFnBytes[0x5C] == 0x25 &&
          RipTarget(kMapFnBytes, kRvaMapFn, 0x5A, 3, 7) == kRvaGlMapBuffer,
          "map: tail-jumps through the glMapBuffer pointer");

    // 0x22ACC0: same two flags, glUnmapBuffer(GL_ARRAY_BUFFER) on the map path, and the RAM
    // path's glBufferSubData(GL_ARRAY_BUFFER, 0, n, data) - which names the pointer we use.
    CHECK(RipTarget(kUnmapFnBytes, kRvaUnmapFn, 0x08, 2, 7) == kRvaGlOk, "unmap: first flag");
    CHECK(RipTarget(kUnmapFnBytes, kRvaUnmapFn, 0x2C, 2, 7) == kRvaUseMap, "unmap: second flag");
    CHECK(kUnmapFnBytes[0x35] == 0xb9 && kUnmapFnBytes[0x36] == 0x92 && kUnmapFnBytes[0x37] == 0x88 &&
          RipTarget(kUnmapFnBytes, kRvaUnmapFn, 0x3A, 2, 6) == kRvaGlUnmapBuffer,
          "unmap: glUnmapBuffer(GL_ARRAY_BUFFER)");
    CHECK(kUnmapFnBytes[0x40] == 0x33 && kUnmapFnBytes[0x41] == 0xc0, "unmap: map path returns 0");
    CHECK(RipTarget(kUnmapFnBytes, kRvaUnmapFn, 0x68, 2, 6) == kRvaGlBufferSubData,
          "unmap: RAM path calls through the glBufferSubData pointer");

    // The guard refuses any other build.
    CHECK(Matches(kMapWin, kMapWin, kMapWinLen), "stock window matches");
    CHECK(!Matches(nullptr, kMapWin, kMapWinLen), "null never matches");
    for (size_t i = 0; i < kMapWinLen; ++i) {
        uint8_t bad[kMapWinLen];
        std::memcpy(bad, kMapWin, kMapWinLen);
        bad[i] ^= 0xff;
        CHECK(!Matches(bad, kMapWin, kMapWinLen), "map window differing in byte %zu must be refused", i);
    }
}

static void TestPatchBytes() {
    // BuildCall round-trips through CallTarget, and refuses what rel32 cannot reach.
    uint8_t c[kCallLen];
    const uintptr_t site = kBase + kRvaMapCall;
    CHECK(BuildCall(site, kBase + kRvaMapFn, c), "stock target encodes");
    CHECK(std::memcmp(c, kMapWin + kMapCallInWin, kCallLen) == 0, "and gives the stock bytes back");
    const uintptr_t stub = kBase + 0x1200000;
    CHECK(BuildCall(site, stub, c) && CallTarget(site, c) == stub, "a nearby stub round-trips");
    const uintptr_t below = site - 0x7ffff000;
    CHECK(BuildCall(site, below, c) && CallTarget(site, c) == below, "2 GB below still reaches");
    CHECK(!BuildCall(site, site + 0x100000000ull, c), "4 GB away must be refused");
    CHECK(!BuildCall(site, 0, c), "null target refused");
    uint8_t notCall[kCallLen] = {0xE9, 0, 0, 0, 0};
    CHECK(CallTarget(site, notCall) == 0, "a jmp is not decoded as a call");

    // The stub: mov rax, imm64; jmp rax.
    uint8_t s[kStubLen + 4] = {0};
    const uint64_t fn = 0x00007ff812345678ull;
    CHECK(BuildStub(s, fn) == kStubLen, "stub length");
    CHECK(s[0] == 0x48 && s[1] == 0xB8 && s[10] == 0xFF && s[11] == 0xE0, "stub opcodes");
    uint64_t got = 0;
    for (int i = 0; i < 8; ++i) got |= (uint64_t)s[2 + i] << (8 * i);
    CHECK(got == fn, "stub immediate");
}

static void TestSize() {
    CHECK(BufferBytes(16000, 36) == 576000, "the quad batch: 16000 vertices * 36 bytes");
    // The four array offsets the dumps show sit inside that.
    CHECK(192000 + 128000 + 64000 + 192000 == 576000, "12 + 8 + 4 + 12 bytes per vertex");
    CHECK(BufferBytes(0, 36) == 0 && BufferBytes(16000, 0) == 0, "empty record");
    CHECK(BufferBytes(-1, 36) == 0 && BufferBytes(16000, -4) == 0, "negative record");
    CHECK(BufferBytes(0x7fffffff, 0x7fffffff) == 0, "junk record is not an allocation request");
    CHECK(BufferBytes(1 << 20, 64) == kMaxBytes, "exactly the cap is allowed");
    CHECK(BufferBytes((1 << 20) + 1, 64) == 0, "one past the cap is not");
}

struct FakeMem {
    std::vector<Region> regions;
    bool operator()(uintptr_t a, Region* r) const {
        for (const Region& x : regions)
            if (a >= x.base && a < x.base + x.size) { *r = x; return true; }
        return false;
    }
};

static void TestClassify() {
    CHECK(ProtectWritable(0x04) && ProtectWritable(0x08) && ProtectWritable(0x40) && ProtectWritable(0x80),
          "RW, WC, XRW, XWC are writable");
    CHECK(ProtectWritable(0x04 | 0x400) && ProtectWritable(0x04 | 0x200), "write-combined / no-cache RW is writable");
    CHECK(!ProtectWritable(0x01) && !ProtectWritable(0x02) && !ProtectWritable(0x20) && !ProtectWritable(0x10),
          "NOACCESS, R, XR, X are not");
    CHECK(!ProtectWritable(0x04 | kPageGuard), "a guard page is not");

    const size_t n = 576000;
    FakeMem m;
    m.regions = {
        {0x10000000, 0x100000, kMemCommit, 0x04},    // a good mapping
        {0x20000000, 0x40000,  kMemCommit, 0x04},    // two adjacent regions that together cover n
        {0x20040000, 0x80000,  kMemCommit, 0x404},
        {0x30000000, 0x100000, kMemCommit, 0x02},    // read-only
        {0x40000000, 0x100000, 0x2000,     0x00},    // reserved only
        {0x50000000, 0x40000,  kMemCommit, 0x04},    // too short, then nothing
    };
    Region bad{};
    CHECK(Classify(0, n, m) == Verdict::Null, "NULL");
    CHECK(Classify(0x10000000, n, m) == Verdict::Ok, "a good mapping passes");
    CHECK(Classify(0x10000000 + 0x100000 - n, n, m) == Verdict::Ok, "ending exactly at the region end passes");
    CHECK(Classify(0x10000000 + 0x100000 - n + 16, n, m) == Verdict::NotCommitted, "one past the end fails");
    CHECK(Classify(0x20000000, n, m) == Verdict::Ok, "spanning two committed RW regions passes");
    CHECK(Classify(0x30000000, n, m, &bad) == Verdict::NotWritable && bad.protect == 0x02, "read-only fails");
    CHECK(Classify(0x40000000, n, m) == Verdict::NotCommitted, "reserved fails");
    CHECK(Classify(0x50000000, n, m) == Verdict::NotCommitted, "running off the end of a region fails");
    // The four addresses from the dumps, in a process where nothing is mapped there.
    for (uintptr_t p : {(uintptr_t)0x14304790, (uintptr_t)0x83E64820, (uintptr_t)0x6B8E5830, (uintptr_t)0xEF3DEFA0})
        CHECK(Classify(p, n, m) == Verdict::NotCommitted, "dump pointer 0x%llx must fail", (unsigned long long)p);
    CHECK(Classify(~(uintptr_t)0 - 100, n, m) == Verdict::NotCommitted, "a range that wraps fails");
    CHECK(Classify(0x10000000, 0, m) == Verdict::Ok, "zero bytes is trivially fine");
    // A query that answers with a region that does not contain the address is not trusted.
    auto liar = [](uintptr_t, Region* r) { *r = {0x1000, 0x1000, kMemCommit, 0x04}; return true; };
    CHECK(Classify(0x10000000, n, liar) == Verdict::NotCommitted, "a region not containing the address fails");
    auto zero = [](uintptr_t a, Region* r) { *r = {a, 0, kMemCommit, 0x04}; return true; };
    CHECK(Classify(0x10000000, n, zero) == Verdict::NotCommitted, "a zero-size region fails (no endless loop)");

    CHECK(std::strcmp(VerdictName(Verdict::Null), "NULL") == 0, "verdict names");
}

static void TestRates() {
    CHECK(!ForceThis(5, 0) && !ForceThis(5, -1), "mapguardtest off");
    CHECK(ForceThis(1, 1) && ForceThis(2, 1), "every map");
    CHECK(!ForceThis(1, 3) && !ForceThis(2, 3) && ForceThis(3, 3) && ForceThis(6, 3), "every third");
    CHECK(!ForceThis(0, 3), "map 0 is never counted");

    int logged = 0;
    for (uint64_t n = 1; n <= 200000; ++n) logged += ShouldLog(n) ? 1 : 0;
    CHECK(ShouldLog(1) && ShouldLog(2) && ShouldLog(3) && ShouldLog(4) && !ShouldLog(5) && ShouldLog(8),
          "first three, then powers of two");
    CHECK(!ShouldLog(0), "nothing at zero");
    CHECK(logged < 25, "an hour of every-frame firing logs %d lines", logged);
}

int main() {
    TestSites();
    TestPatchBytes();
    TestSize();
    TestClassify();
    TestRates();
    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("mapguard: all checks passed\n");
    return 0;
}
