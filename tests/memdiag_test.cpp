// The memdiag=1 bookkeeping (src/memdiag.h): sizes per format, live texture/buffer accounting
// across respecification and delete, and the log lines. Pure, so CI runs it.

#include "../src/memdiag.h"

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

using namespace memdiag;

int main() {
    // Sizes.
    CHECK(LevelBytes(kRgba8, 1024, 1024) == 4ull * 1024 * 1024, "RGBA8 1k");
    CHECK(LevelBytes(kRgba, 2, 2) == 16, "GL_RGBA 2x2");
    CHECK(LevelBytes(3, 4, 4) == 64, "RGB padded to 4 bytes");
    CHECK(LevelBytes(kLuminance, 8, 8) == 64, "luminance 1 byte");
    CHECK(LevelBytes(kDxt1Rgb, 4, 4) == 8, "DXT1 one block");
    CHECK(LevelBytes(kDxt5, 1024, 1024) == 1024ull * 1024, "DXT5 1 byte a texel");
    CHECK(LevelBytes(kDxt1Rgba, 1, 1) == 8, "DXT1 rounds up to a block");
    CHECK(LevelBytes(kRgba16F, 4, 4) == 128, "RGBA16F");
    CHECK(LevelBytes(kRgba8, 0, 4) == 0 && LevelBytes(kRgba8, -1, 4) == 0, "degenerate");
    CHECK(LevelBytes(kRgba8, 70000, 4) == 0, "absurd width");
    CHECK(TransferBytes(kRgba, kUByte, 16, 16) == 1024, "sub RGBA ubyte");
    CHECK(TransferBytes(kBgra, 0x8367 /*8_8_8_8_REV*/, 16, 16) == 1024, "sub packed");
    CHECK(TransferBytes(kRgb, kFloat, 2, 2) == 48, "sub RGB float");
    CHECK(Describe(kRgba8).cls == CLS_RGBA8 && Describe(4).cls == CLS_RGBA8, "class RGBA8");
    CHECK(Describe(kRgb).cls == CLS_RGB8, "class RGB8");
    CHECK(Describe(kDxt5).cls == CLS_DRIVER_COMP, "DXT given raw texels is driver-compressed");
    CHECK(SizeBucket(64) == 0 && SizeBucket(65) == 1 && SizeBucket(1024) == 4 &&
          SizeBucket(2048) == 5 && SizeBucket(4096) == 6 && SizeBucket(16384) == 6, "size buckets");

    // A mipmapped RGBA8 texture, a compressed one, then respecify and delete.
    {
        Tracker t;
        uint64_t total = 0;
        for (int l = 0, d = 256; d >= 1; ++l, d /= 2) {
            const uint64_t b = LevelBytes(kRgba8, d, d);
            total += b;
            t.TexLevel(7, 0, l, kRgba8, d, d, b, false);
        }
        t.TexLevel(9, 0, 0, kDxt5, 1024, 1024, 1024 * 1024, true);
        Snapshot s = t.Snap();
        CHECK(s.texLive.count == 2, "two live textures (%lld)", (long long)s.texLive.count);
        CHECK(s.texLive.bytes == (int64_t)(total + 1024 * 1024), "live bytes");
        CHECK(s.cls[CLS_RGBA8].count == 1 && s.cls[CLS_RGBA8].bytes == (int64_t)total, "RGBA8 class");
        CHECK(s.cls[CLS_COMPRESSED].count == 1 && s.cls[CLS_COMPRESSED].bytes == 1024 * 1024, "DXT class");
        CHECK(s.size[SizeBucket(256)].count == 1 && s.size[SizeBucket(1024)].count == 1, "size buckets");
        CHECK(!s.fmts.empty() && s.fmts[0].fmt == kDxt5, "biggest format first");

        // Level 0 given again at a smaller size replaces, not adds.
        t.TexLevel(7, 0, 0, kRgba8, 128, 128, LevelBytes(kRgba8, 128, 128), false);
        s = t.Snap();
        CHECK(s.texLive.count == 2, "respecify keeps the count");
        CHECK(s.cls[CLS_RGBA8].bytes == (int64_t)(total - LevelBytes(kRgba8, 256, 256) + LevelBytes(kRgba8, 128, 128)),
              "respecify replaces level 0");
        CHECK(s.size[SizeBucket(256)].count == 0 && s.size[SizeBucket(128)].count == 1, "moved size bucket");
        CHECK(s.texFreed == LevelBytes(kRgba8, 256, 256), "the old level counts as freed");

        // Respecify level 0 as compressed: the texture moves class.
        t.TexLevel(7, 0, 0, kDxt1Rgb, 128, 128, LevelBytes(kDxt1Rgb, 128, 128), true);
        s = t.Snap();
        CHECK(s.cls[CLS_COMPRESSED].count == 2 && s.cls[CLS_RGBA8].count == 0, "class follows level 0");

        t.DeleteTexture(7);
        t.DeleteTexture(9);
        t.DeleteTexture(1234);   // never seen: loaded before the hooks
        s = t.Snap();
        CHECK(s.texLive.count == 0 && s.texLive.bytes == 0, "all freed (%lld bytes left)", (long long)s.texLive.bytes);
        for (int c = 0; c < CLS_COUNT; ++c) CHECK(s.cls[c].count == 0 && s.cls[c].bytes == 0, "class %d empty", c);
        for (int b = 0; b < kSizeBuckets; ++b) CHECK(s.size[b].count == 0 && s.size[b].bytes == 0, "bucket %d empty", b);
        CHECK(s.fmts.empty(), "no live formats left (%zu)", s.fmts.size());
        CHECK(s.untrackedDeletes == 1, "untracked delete counted");
    }

    // Cube map faces are separate levels of one texture; texture 0 is not tracked.
    {
        Tracker t;
        for (int f = 0; f < 6; ++f) t.TexLevel(3, f, 0, kRgba8, 64, 64, LevelBytes(kRgba8, 64, 64), false);
        t.TexLevel(0, 0, 0, kRgba8, 64, 64, 100, false);
        Snapshot s = t.Snap();
        CHECK(s.texLive.count == 1 && s.texLive.bytes == 6 * 64 * 64 * 4, "cube = one texture, six faces");
        CHECK(s.unboundUploads == 1 && s.texUploaded == 6ull * 64 * 64 * 4 + 100, "unbound upload counted in traffic only");
    }

    // Buffers.
    {
        Tracker t;
        t.BufferData(1, 1000);
        t.BufferData(2, 500);
        t.BufferData(1, 300);   // orphaned and given a new size
        Snapshot s = t.Snap();
        CHECK(s.bufLive.count == 2 && s.bufLive.bytes == 800, "buffers live %lld", (long long)s.bufLive.bytes);
        CHECK(s.bufUploaded == 1800 && s.bufFreed == 1000, "buffer traffic");
        t.DeleteBuffer(1);
        t.DeleteBuffer(99);
        s = t.Snap();
        CHECK(s.bufLive.count == 1 && s.bufLive.bytes == 500, "buffer deleted");
    }

    // The log lines carry the numbers and stay inside the buffer.
    {
        Tracker t;
        t.TexLevel(5, 0, 0, kRgba8, 2048, 2048, LevelBytes(kRgba8, 2048, 2048), false);
        t.TexLevel(6, 0, 0, kDxt5, 2048, 2048, LevelBytes(kDxt5, 2048, 2048), true);
        t.BufferData(1, 2 * 1024 * 1024);
        const Snapshot s = t.Snap();
        char line[1024];
        FormatSummary(s, line, sizeof(line));
        CHECK(std::strstr(line, "tex live=2 (20.0 MB)") != nullptr, "summary: %s", line);
        CHECK(std::strstr(line, "RGBA8 1/16.0MB") != nullptr, "summary RGBA8: %s", line);
        CHECK(std::strstr(line, "DXT 1/4.0MB") != nullptr, "summary DXT: %s", line);
        CHECK(std::strstr(line, "buf live=1 (2.0 MB)") != nullptr, "summary buf: %s", line);
        FormatDetail(s, line, sizeof(line));
        CHECK(std::strstr(line, "2048 2/20.0MB") != nullptr, "detail size: %s", line);
        CHECK(std::strstr(line, "RGBA8 1/16.0MB") != nullptr && std::strstr(line, "DXT5 1/4.0MB") != nullptr,
              "detail formats: %s", line);
        char tiny[16];
        FormatSummary(s, tiny, sizeof(tiny));
        CHECK(std::strlen(tiny) < sizeof(tiny), "truncates safely");
        FormatDetail(s, tiny, sizeof(tiny));
        CHECK(std::strlen(tiny) < sizeof(tiny), "detail truncates safely");
    }

    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("memdiag: all checks passed\n");
    return 0;
}
