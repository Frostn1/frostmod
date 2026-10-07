// texcompress=1's decision (src/texcompress.h): which glTexImage2D uploads are swapped to
// DXT, that a swapped texture's mip chain stays one format, and that a sub-image update
// undoes the swap and keeps the name unswapped. Pure, so CI runs it.

#include "../src/texcompress.h"

#include <cstdio>
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

using namespace texcompress;

static Request Req(uint32_t ifmt, uint32_t format, int32_t w, int32_t h, int32_t level = 0) {
    Request r{};
    r.target = kTex2D; r.ifmt = ifmt; r.format = format; r.type = kUByte;
    r.level = level; r.w = w; r.h = h; r.border = 0;
    r.hasPixels = true; r.fromGame = true; r.tightRgba = false; r.px = nullptr;
    return r;
}

int main() {
    // The swaps.
    {
        Table t;
        Decision d = t.Choose(1, Req(kRgba8, kRgba, 1024, 1024));
        CHECK(d.skip == SK_NONE && d.ifmt == kDxt5, "RGBA8 1k -> DXT5");
        d = t.Choose(2, Req(3, kRgb, 512, 256));
        CHECK(d.skip == SK_NONE && d.ifmt == kDxt1Rgb, "RGB (3) -> DXT1");
        d = t.Choose(3, Req(kRgba, kRgb, 256, 256));
        CHECK(d.skip == SK_NONE && d.ifmt == kDxt1Rgb, "RGBA internal from RGB data is opaque -> DXT1");
    }
    // Opaque RGBA data -> DXT1; any alpha below 255 -> DXT5.
    {
        Table t;
        std::vector<uint8_t> px(256 * 256 * 4, 0xFF);
        Request r = Req(kRgba8, kRgba, 256, 256);
        r.tightRgba = true; r.px = px.data();
        CHECK(t.Choose(1, r).ifmt == kDxt1Rgb, "all-opaque RGBA -> DXT1");
        px[4 * 1000 + 3] = 0x80;
        CHECK(t.Choose(2, r).ifmt == kDxt5, "one translucent texel -> DXT5");
        r.format = kBgra; px[4 * 1000 + 3] = 0xFF;
        CHECK(t.Choose(3, r).ifmt == kDxt1Rgb, "opaque BGRA -> DXT1");
    }
    // What is left alone, and why.
    {
        Table t;
        Request r = Req(kRgba8, kRgba, 1024, 1024);
        r.target = 0x8515; CHECK(t.Choose(1, r).skip == SK_TARGET, "cube face");
        r = Req(kRgba8, kRgba, 1024, 1024);
        CHECK(t.Choose(0, r).skip == SK_TARGET, "no bound texture");
        r.hasPixels = false; CHECK(t.Choose(1, r).skip == SK_NODATA, "render target / filled later");
        r = Req(kRgba8, kRgba, 128, 1024); CHECK(t.Choose(1, r).skip == SK_SMALL, "under the threshold");
        r = Req(kRgba8, kRgba, 1920, 1080); CHECK(t.Choose(1, r).skip == SK_NPOT, "screen-sized art");
        r = Req(kRgba8, kRgba, 1024, 1024); r.fromGame = false; CHECK(t.Choose(1, r).skip == SK_CALLER, "plugin");
        r = Req(0x803C /*ALPHA8*/, 0x1906, 1024, 1024); CHECK(t.Choose(1, r).skip == SK_FORMAT, "alpha8");
        r = Req(0x881A /*RGBA16F*/, kRgba, 1024, 1024); CHECK(t.Choose(1, r).skip == SK_FORMAT, "float");
        r = Req(kRgba8, kRgba, 1024, 1024); r.type = 0x1406; CHECK(t.Choose(1, r).skip == SK_FORMAT, "float data");
        r = Req(kRgba8, kRgba, 1024, 1024); r.border = 1; CHECK(t.Choose(1, r).skip == SK_BORDER, "border");
        Table s(512);
        CHECK(s.Choose(1, Req(kRgba8, kRgba, 256, 256)).skip == SK_SMALL, "threshold is configurable");
        CHECK(s.Choose(2, Req(kRgba8, kRgba, 512, 512)).skip == SK_NONE, "at the threshold swaps");
    }
    // The mip chain follows level 0, whatever the lower levels' size.
    {
        Table t;
        CHECK(t.Choose(5, Req(kRgba8, kRgba, 1024, 1024, 0)).ifmt == kDxt5, "level 0");
        for (int l = 1; l <= 10; ++l) {
            const int32_t s = 1024 >> l;
            CHECK(t.Choose(5, Req(kRgba8, kRgba, s, s, l)).ifmt == kDxt5, "level %d stays DXT5", l);
        }
        CHECK(t.Find(5) && t.Find(5)->levels == 0x7FF, "levels 0..10 recorded (%x)", t.Find(5) ? t.Find(5)->levels : 0);
        // A texture whose level 0 was not swapped keeps every level as given.
        CHECK(t.Choose(6, Req(kRgba8, kRgba, 128, 128, 0)).skip == SK_SMALL, "small level 0");
        CHECK(t.Choose(6, Req(kRgba8, kRgba, 64, 64, 1)).ifmt == kRgba8, "its level 1 as given");
        // Respecifying level 0 decides again.
        CHECK(t.Choose(5, Req(kRgba8, kRgba, 1024, 1024, 0)).ifmt == kDxt5, "respec");
        CHECK(t.Find(5)->levels == 1, "respec resets the level mask");
        Request nd = Req(kRgba8, kRgba, 1024, 1024, 0); nd.hasPixels = false;
        CHECK(t.Choose(5, nd).skip == SK_NODATA && t.Find(5)->dxt == 0, "respec as a render target unswaps");
        CHECK(t.Choose(5, Req(kRgba8, kRgba, 512, 512, 1)).ifmt == kRgba8, "then its levels stay raw");
    }
    // Sub-image updates: the swap is undone once, and the name is never swapped again.
    {
        Table t;
        t.Choose(7, Req(kRgb8, kRgb, 512, 512, 0));
        t.Choose(7, Req(kRgb8, kRgb, 256, 256, 1));
        const Entry* u = t.SubImage(7);
        CHECK(u && u->dxt == kDxt1Rgb && u->origIfmt == kRgb8 && u->levels == 3 && u->w0 == 512,
              "undo carries what to restore");
        CHECK(t.SubImage(7) == nullptr, "second update: nothing to undo");
        CHECK(t.Choose(7, Req(kRgb8, kRgb, 512, 512, 0)).skip == SK_SUBIMAGE, "never swapped again");
        CHECK(t.SubImage(8) == nullptr, "update on an unswapped texture");
        CHECK(t.Choose(8, Req(kRgb8, kRgb, 512, 512, 0)).skip == SK_SUBIMAGE, "it is remembered too");
        t.Delete(8);
        CHECK(t.Choose(8, Req(kRgb8, kRgb, 512, 512, 0)).skip == SK_NONE, "a deleted name starts fresh");
        CHECK(t.SubImage(0) == nullptr, "texture 0");
    }
    // A driver refusal reverts.
    {
        Table t;
        t.Choose(9, Req(kRgba8, kRgba, 512, 512, 0));
        t.Revert(9);
        CHECK(t.Find(9)->dxt == 0, "reverted");
        CHECK(t.Choose(9, Req(kRgba8, kRgba, 256, 256, 1)).ifmt == kRgba8, "lower level as given after revert");
    }
    // Sizes.
    CHECK(RawBytes(1024, 1024) == 4u * 1024 * 1024, "raw");
    CHECK(DxtBytes(kDxt5, 1024, 1024) == 1024u * 1024, "DXT5 1 B/texel");
    CHECK(DxtBytes(kDxt1Rgb, 1024, 1024) == 512u * 1024, "DXT1 half a byte");
    CHECK(DxtBytes(kDxt1Rgb, 1, 1) == 8, "one block");
    CHECK(IsPow2(1) && IsPow2(2048) && !IsPow2(0) && !IsPow2(1920), "pow2");

    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("texcompress: all checks passed\n");
    return 0;
}
