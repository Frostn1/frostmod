// setupguard.h: the bytes we check name beta21f's definition offset and validator, and the
// re-check finds exactly the fields the game's validator would reject.
#include "../src/setupguard.h"

#include <cstdio>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); } } while (0)

using namespace setupguard;

static void Put(std::vector<uint8_t>& b, size_t off, int32_t v) { std::memcpy(b.data() + off, &v, 4); }

int main() {
    // The setup job takes the definition at +0x210 and calls +0x198A90 (call at +0x30, rel32 from +0x35).
    CHECK(kSetupJob[0x24] == 0x8D && kSetupJob[0x26] == 0x10 && kSetupJob[0x27] == 0x02, "def at bike+0x210");
    int32_t rel; std::memcpy(&rel, kSetupJob + 0x31, 4);
    CHECK(kRvaSetupJob + 0x35 + rel == kRvaValidator, "setup job calls the validator we hook");
    CHECK(kValidator[0x14] == 0x84 && kValidator[0x15] == 0x00, "first field is def+0x84");
    CHECK(kFieldCount == 37, "36 setup fields plus the per-gear walk");

    // A definition that allows everything up to 5, four gears, two tyre compounds.
    std::vector<uint8_t> def(0x24000, 0), setup(kSetupBytes, 0);
    for (size_t f = 0; f < kFieldCount; ++f) {
        const Field& fd = kFields[f];
        if (fd.kind == Kind::Max) Put(def, fd.limit, 5);
        else if (fd.kind == Kind::Range) Put(def, fd.limit + 8, 5);
        else if (fd.kind == Kind::TyreRange) for (int c = 0; c <= 5; ++c) Put(def, fd.limit + kTyreStride * c + 8, 5);
    }
    Put(def, kGearCount, 4);
    Put(def, kRatioCount, 5);
    Failure f[48];
    CHECK(Evaluate(def.data(), setup.data(), f, 48) == 0, "all zero is valid");

    Put(setup, 0x54, 6);                 // a preload step past the end
    Put(setup, 0x24 + 4 * 2, 9);         // third gear past the ratio list
    Put(setup, 0x24 + 4 * 6, 99);        // gear 7 is not walked: the bike has four
    Put(setup, 0xC0, -1);                // negative
    size_t n = Evaluate(def.data(), setup.data(), f, 48);
    CHECK(n == 3, "three failures");
    CHECK(n >= 1 && kFields[f[0].field].setup == 0x54 && f[0].value == 6 && f[0].limit == 5, "f54 first");
    CHECK(n >= 2 && kFields[f[1].field].kind == Kind::Gears && f[1].gear == 2 && f[1].value == 9, "gear 2");
    CHECK(n >= 3 && kFields[f[2].field].setup == 0xC0 && f[2].value == -1, "negative fC0");
    CHECK(!OnlyBroken(f, n), "a real out-of-range setup keeps its ban");
    CHECK(!OnlyBroken(f, 0), "no failures is not 'only broken'");

    // A bad compound index: its own field fails and the tyre range behind it is not read.
    std::vector<uint8_t> s2(kSetupBytes, 0);
    Put(s2, 0x8C, 1000000);
    n = Evaluate(def.data(), s2.data(), f, 48);
    CHECK(n == 1 && kFields[f[0].field].setup == 0x8C, "compound index out of range, tyre not read");

    // The beta21f case: the server built no tyre sizes, the rider picked the second rear size.
    std::vector<uint8_t> d3 = def, s3(kSetupBytes, 0);
    Put(d3, 0x134A0, 0);                 // front sizes: none on the server
    Put(d3, 0x217B0, 0);                 // rear sizes: none on the server
    Put(s3, kRearSize, 1);
    n = Evaluate(d3.data(), s3.data(), f, 48);
    CHECK(n == 1 && kFields[f[0].field].setup == kRearSize && f[0].limit == 0, "rear size vs empty list");
    CHECK(OnlyBroken(f, n), "the beta21f empty-list failure is let through");
    Put(s3, kFrontSize, 1);
    n = Evaluate(d3.data(), s3.data(), f, 48);
    CHECK(n == 2 && OnlyBroken(f, n), "front and rear against empty lists");
    // ...but not together with a real out-of-range field,
    Put(s3, 0x54, 6);
    n = Evaluate(d3.data(), s3.data(), f, 48);
    CHECK(n == 3 && !OnlyBroken(f, n), "a real cheat next to it keeps the ban");
    // ...not when the server did build the list,
    std::vector<uint8_t> s4(kSetupBytes, 0);
    Put(s4, kRearSize, 9);
    n = Evaluate(def.data(), s4.data(), f, 48);
    CHECK(n == 1 && !OnlyBroken(f, n), "size past a list the server built keeps the ban");
    // ...and not for a negative or absurd index.
    Put(s4, kRearSize, -1);
    n = Evaluate(d3.data(), s4.data(), f, 48);
    CHECK(n == 1 && !OnlyBroken(f, n), "negative size index keeps the ban");
    Put(s4, kRearSize, 100000);
    n = Evaluate(d3.data(), s4.data(), f, 48);
    CHECK(n == 1 && !OnlyBroken(f, n), "absurd size index keeps the ban");

    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("setupguard: all checks passed\n");
    return 0;
}
