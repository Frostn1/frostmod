// Live bike gfx.cfg (src/gfxcfg.h): the cfg parser, the key -> descriptor table, and the
// proof that every offset in that table is the one the game's own instructions use.
//
// A wrong offset here is a float written into the wrong field of a live vehicle every time a
// modder saves the file, so each one is checked against the bytes it was read from rather
// than trusted. Pure, so it runs anywhere.

#include "../src/gfxcfg.h"

#include <cstdio>
#include <cstring>
#include <set>

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

using namespace gfxcfg;

// A made-up bike in PiBoSo's layout: braces on their own lines, a one-line block, ';'
// comments, a key with no spaces round '=', and a cockpit that repeats the steer block.
static const char* kCfg =
    "chassis\n"
    "{\n"
    "\tmodel\n\t{\n\t\tfile = chassis.hrc\n\t}\n"
    "\trearbrakepedal\n\t{\n\t\tname = rb\n\t\taxis = x-\n\t\tmaxrot = -10\n\t}\n"
    "\tshifter { name = gl axis = y maxrot = 12.5 }\n"
    "\tchain\n\t{\n\t\tname = chain\n\t\tengine\n\t\t{\n\t\t\tx = -0.074\n\t\t\ty = 0.485\n"
    "\t\t\tz = -0.109\n\t\t}\n\t\tratio = 0.035 ; per wheel turn\n\t}\n"
    "}\n"
    "steer\n"
    "{\n"
    "\tbrakelever\n\t{\n\t\tname = bl\n\t\taxis = z-\n\t\tmaxrot = 20\n\t}\n"
    "\tleftgrip\n\t{\n\t\ttype = 1\n\t\tpos\n\t\t{\n\t\t\tx = -0.33\n\t\t\ty = 0.22\n"
    "\t\t\tz = -0.025\n\t\t}\n\t}\n"
    "\trightgrip { type = 1 pos { x = 0.33 y = 0.22 z = -0.025 } }\n"
    "}\n"
    "rider\n{\n\txform\n\t{\n\t\tx = 0\n\t\ty = 0.01\n\t\tz = -0.02\n\t}\n}\n"
    "dirt_color=1\n"
    "cockpit\n"
    "{\n"
    "\tsteer\n\t{\n"
    "\t\tleftgrip\n\t\t{\n\t\t\ttype = 1\n\t\t\tpos\n\t\t\t{\n\t\t\t\tx = -0.30\n"
    "\t\t\t\ty = 0.20\n\t\t\t\tz = 0.01\n\t\t\t}\n\t\t}\n"
    "\t\tclutchlever { name = cl axis = Y maxrot = 15 }\n"
    "\t}\n"
    "}\n"
    "tyres = oem_mx\n";

static float F(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }

static const Write* Find(const std::vector<Write>& w, int off) {
    for (const auto& x : w) if (x.off == off) return &x;
    return nullptr;
}

static void parser() {
    const Flat c = Parse(kCfg);
    auto has = [&](const char* k, const char* v) {
        auto it = c.find(k);
        return it != c.end() && it->second == v;
    };
    CHECK(has("chassis/model/file", "chassis.hrc"), "nested block");
    CHECK(has("chassis/rearbrakepedal/axis", "x-"), "axis kept as text");
    CHECK(has("chassis/shifter/maxrot", "12.5"), "one-line block, last key");
    CHECK(has("chassis/shifter/name", "gl"), "one-line block, first key");
    CHECK(has("chassis/chain/ratio", "0.035"), "comment stripped");
    CHECK(has("chassis/chain/engine/z", "-0.109"), "three levels");
    CHECK(has("steer/leftgrip/pos/y", "0.22"), "grip pos");
    CHECK(has("steer/rightgrip/pos/x", "0.33"), "grip one-liner with a nested block");
    CHECK(has("steer/rightgrip/type", "1"), "value before a nested block on the same line");
    CHECK(has("rider/xform/z", "-0.02"), "rider offset");
    CHECK(has("dirt_color", "1"), "no spaces round '='");
    CHECK(has("cockpit/steer/leftgrip/pos/x", "-0.30"), "cockpit nesting");
    CHECK(has("tyres", "oem_mx"), "top-level key after the cockpit");
    CHECK(c.find("pos") == c.end() && c.find("steer") == c.end(), "no block names as keys");
}

static void plan() {
    const Flat c = Parse(kCfg);
    std::vector<std::string> bad;
    const auto bike = Plan(c, "", &bad);
    CHECK(bad.empty(), "%zu bad values in a good file", bad.size());

    const Write* w = Find(bike, 0x384);
    CHECK(w && F(w->bits) == -0.33f, "left grip x");
    w = Find(bike, 0x38C);
    CHECK(w && F(w->bits) == -0.025f, "left grip z");
    w = Find(bike, 0x3A4);
    CHECK(w && F(w->bits) == 0.33f, "right grip x");
    w = Find(bike, 0x37C);
    CHECK(w && w->bits == 1, "left grip type");
    w = Find(bike, 0x9C);
    CHECK(w && w->bits == 4, "rear brake axis x- = 4");
    w = Find(bike, 0xA0);
    CHECK(w && F(w->bits) == -10.0f, "rear brake maxrot");
    w = Find(bike, 0xC8);
    CHECK(w && w->bits == 1, "shifter axis y = 1");
    w = Find(bike, 0x90);
    CHECK(w && w->bits == 6, "brake lever axis z- = 6");
    w = Find(bike, 0x1CC);
    CHECK(w && F(w->bits) == 0.035f, "chain ratio");
    w = Find(bike, 0x108);
    CHECK(w && F(w->bits) == -0.074f, "chain engine x");
    w = Find(bike, 0x1EC);
    CHECK(w && F(w->bits) == 0.01f, "rider offset y");
    CHECK(!Find(bike, 0x390), "dir not in the file, not written");
    CHECK(!Find(bike, 0xBC), "the bike has no clutch lever block, so no clutch write");

    const auto cp = Plan(c, "cockpit/", &bad);
    w = Find(cp, 0x384);
    CHECK(w && F(w->bits) == -0.30f, "cockpit grip comes from the cockpit block");
    w = Find(cp, 0xBC);
    CHECK(w && w->bits == 1, "cockpit clutch axis, case-insensitive");
    w = Find(cp, 0x1E8);
    CHECK(w && F(w->bits) == 0.0f, "rider offset is the same key for the cockpit");
    CHECK(!Find(cp, 0x3A4), "cockpit has no right grip block");

    // Values the game could not have meant are skipped and named.
    Flat broken = c;
    broken["steer/leftgrip/pos/x"] = "abc";
    broken["steer/brakelever/axis"] = "w";
    broken["steer/leftgrip/type"] = "9";
    broken["chassis/chain/ratio"] = "1e9";
    bad.clear();
    const auto b2 = Plan(broken, "", &bad);
    CHECK(!Find(b2, 0x384) && !Find(b2, 0x90) && !Find(b2, 0x37C) && !Find(b2, 0x1CC),
          "bad values not written");
    CHECK(bad.size() == 4, "%zu bad values named, want 4", bad.size());
}

static void not_live() {
    const Flat a = Parse(kCfg);
    Flat b = a;
    b["steer/leftgrip/pos/x"] = "-0.4";          // live
    b["cockpit/steer/leftgrip/pos/y"] = "0.3";   // live, cockpit
    b["chassis/shifter/name"] = "gear";          // a node name: needs a rebuild
    b["chassis/exhaust/pos/x"] = "0.1";          // exhaust: a different record
    b.erase("tyres");                            // removed
    auto nl = NotLive(a, b);
    std::set<std::string> s(nl.begin(), nl.end());
    CHECK(s.size() == 3, "%zu not-live keys, want 3", s.size());
    CHECK(s.count("chassis/shifter/name") && s.count("chassis/exhaust/pos/x") && s.count("tyres"),
          "the three not-live keys");
    CHECK(NotLive(a, a).empty(), "no change, nothing to report");
}

// Every offset in the table is the displacement in an instruction that was read out of the
// game, and every site's bytes say what the table claims.
static void offsets_are_proved() {
    for (int i = 0; i < kSiteCount; ++i) {
        const Site& s = kSites[i];
        CHECK(s.dispAt >= 0 && s.dispAt + 4 <= s.len && s.len <= (int)sizeof(s.bytes),
              "site 0x%zx: disp outside the instruction", (size_t)s.rva);
        CHECK(SiteDisp(s) == s.disp, "site 0x%zx encodes 0x%x, table says 0x%x",
              (size_t)s.rva, SiteDisp(s), s.disp);
    }
    // Each field has a parser site (lea r8,[rdi+off] or the axis store) proving its offset.
    for (int f = 0; f < kFieldCount; ++f) {
        bool proved = false;
        for (int i = 0; i < kSiteCount; ++i) {
            const Site& s = kSites[i];
            const bool parser = s.rva >= 0x419C0 && s.rva < 0x4494B;
            const bool lea_r8 = s.bytes[0] == 0x4C && s.bytes[1] == 0x8D && s.bytes[2] == 0x87;
            const bool mov_imm = s.bytes[0] == 0xC7 && s.bytes[1] == 0x87;
            const bool right_kind = kFields[f].kind == Kind::Axis ? mov_imm : lea_r8;
            if (parser && right_kind && s.disp == kFields[f].off) proved = true;
        }
        CHECK(proved, "%s (0x%x) has no parser site", kFields[f].key, kFields[f].off);
    }
    // Fields are 4 bytes, sorted, distinct, and inside one part descriptor.
    for (int f = 0; f < kFieldCount; ++f) {
        CHECK(kFields[f].off % 4 == 0, "%s misaligned", kFields[f].key);
        CHECK(kFields[f].off + 4 <= kCockpitDesc, "%s past the descriptor", kFields[f].key);
        if (f) CHECK(kFields[f].off > kFields[f - 1].off, "%s out of order", kFields[f].key);
    }
    // The cockpit descriptor is the second one in the block and the flag sits between them.
    CHECK(kCockpitParsed < kCockpitDesc && kCockpitParsed >= 0x3B8 + 4, "cockpit flag placement");
    CHECK(kVehGfx == 0x274, "gfx block offset");
}

int main() {
    parser();
    plan();
    not_live();
    offsets_are_proved();
    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("gfxcfg: all checks passed\n");
    return 0;
}
