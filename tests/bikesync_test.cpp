// Invariants for per-rider bike model sync. Pure logic, no Win32 or game state.

#include "../src/bikesync.h"

#include <cstdio>
#include <string>
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

using namespace frostmod::bikesync;

static std::string MakeId(const char* bike) {
    IdBuilder b;
    b.Begin(bike);
    b.File("gfx.cfg", 3); b.Bytes("gfx", 3);
    b.File("model.edf", 5); b.Bytes("model", 5);
    return b.Finish();
}

int main() {
    CHECK(IsVisualFile("MODEL.EDF") && IsVisualFile("model_s.edf") &&
          IsVisualFile("frame.HRC") && IsVisualFile("GfX.CfG"), "visual names, case-insensitive");
    CHECK(!IsVisualFile("model.edf.bak") && !IsVisualFile("paint.pnt"), "non-visual names");

    const auto order = CanonicalVisualOrder({"Z.HRC", "readme.txt", "MODEL.EDF", "z.hrc", "gfx.cfg"});
    CHECK(order == std::vector<std::string>({"gfx.cfg", "model.edf", "z.hrc"}), "canonical order");

    const std::string id = MakeId("OEM 450");
    CHECK(id == MakeId("OEM 450"), "id is deterministic");
    CHECK(id == MakeId("oem 450"), "bike name is case-insensitive");
    // python -c "import hashlib,struct; print(hashlib.sha256(b'oem 450\0gfx.cfg\0'+struct.pack('<Q',3)+b'gfxmodel.edf\0'+struct.pack('<Q',5)+b'model').hexdigest()[:32])"
    CHECK(id == "259cdb277adeaa33cdeb8cade4fb98eb", "known SHA-256 vector: %s", id.c_str());

    CHECK(ShadowName("OEM 450", id) == "OEM 450~" + id.substr(0, 8), "shadow name");
    CHECK(LooksLikeShadow("OEM 450~0123abcd"), "shadow shape");
    CHECK(!LooksLikeShadow("OEM 450~0123ABCd") && !LooksLikeShadow("OEM~0123abcde") &&
          !LooksLikeShadow("OEM0123abcd"), "shadow rejects malformed suffixes");

    std::string bike, parsed;
    const std::string marker = BuildMarker("OEM 450", id);
    CHECK(ParseMarker(marker, bike, parsed) && bike == "OEM 450" && parsed == id, "marker round trip");
    std::string crlf = marker;
    for (size_t p = 0; (p = crlf.find('\n', p)) != std::string::npos; p += 2) crlf.replace(p, 1, "\r\n");
    CHECK(ParseMarker(crlf, bike, parsed), "CRLF marker");
    CHECK(!ParseMarker("frostmod-modelsync 2\nOEM 450\n" + id + "\n", bike, parsed), "header rejected");
    CHECK(!ParseMarker("frostmod-modelsync 1\n../bad\n" + id + "\n", bike, parsed), "bad bike rejected");
    CHECK(!ParseMarker("frostmod-modelsync 1\nOEM 450\n\n", bike, parsed), "empty id rejected");
    CHECK(!ParseMarker(marker + "extra\n", bike, parsed), "extra lines rejected");

    ShadowPlan plan = PlanShadow({"bike.ini", "model.edf", "_active.txt"}, {"MODEL.EDF", "frame.hrc", "note"});
    CHECK(plan.ok && plan.fromRoot == std::vector<std::string>({"bike.ini"}) &&
          plan.fromVariant == std::vector<std::string>({"MODEL.EDF", "frame.hrc"}), "valid shadow plan");
    CHECK(!PlanShadow({"bike.ini"}, {"frame.hrc"}).ok, "model.edf required");
    CHECK(!PlanShadow({"model.edf", "_active.txt", "_modelsync.txt"}, {"model.edf"}).ok,
          "base root file required and markers excluded");

    ShadowIndex idx;
    idx.Set("OEM 450", id, "OEM 450~12345678");
    looksync::Look look; look.bike = "oem 450"; look.swap = id;
    CHECK(ChooseShadow(look, "OEM 450", "other", idx) == "OEM 450~12345678", "shadow selected");
    look.swap.clear(); CHECK(ChooseShadow(look, "OEM 450", "other", idx).empty(), "empty announcement");
    look.swap = id; look.bike = "OEM 250"; CHECK(ChooseShadow(look, "OEM 450", "other", idx).empty(), "bike mismatch");
    look.bike = "OEM 450"; CHECK(ChooseShadow(look, "OEM 450", id, idx).empty(), "active model needs no shadow");
    CHECK(ChooseShadow(look, "OEM 450", "other", ShadowIndex{}).empty(), "missing local model");

    CHECK(SlotOfBlock(0x1274, 0x1000, 0x100, 0x274, 4) == 0, "exact first slot");
    CHECK(SlotOfBlock(0x1474, 0x1000, 0x100, 0x274, 4) == 2, "exact later slot");
    CHECK(SlotOfBlock(0x1475, 0x1000, 0x100, 0x274, 4) == -1, "misaligned block");
    CHECK(SlotOfBlock(0x1674, 0x1000, 0x100, 0x274, 4) == -1, "out of range block");
    CHECK(SlotOfBlock(0x9999, 0x1000, 0x100, 0x274, 4) == -1, "foreign block");

    RemapTable table;
    std::string shadow;
    table.Set(3, 77, "Rider", "OEM 450~12345678");
    CHECK(table.Lookup(3, 77, shadow) && shadow == "OEM 450~12345678", "matching remap");
    CHECK(!table.Lookup(3, 78, shadow), "key mismatch");
    table.Clear(3); CHECK(!table.Lookup(3, 77, shadow), "clear");
    table.Set(-1, 1, "x", "y"); CHECK(table.Get(-1) == nullptr, "out-of-range ignored");

    if (g_failures) { std::printf("%d bikesync test(s) failed\n", g_failures); return 1; }
    std::printf("bikesync tests passed\n");
    return 0;
}
