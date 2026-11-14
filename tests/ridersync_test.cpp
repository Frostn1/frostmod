#include "../src/ridersync.h"
#include <cstdio>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::string Id(char c) { return std::string(32, c); }

static void Files() {
    CHECK(ridersync::IsVisualFile("BODY.EDF"), "edf");
    CHECK(ridersync::IsVisualFile("gfx.cfg"), "gfx");
    CHECK(!ridersync::IsVisualFile("paints\\x.dds"), "subfolder");
    CHECK(!ridersync::IsVisualFile("readme.txt"), "txt");
    std::vector<ridersync::FileEntry> out;
    CHECK(ridersync::CanonicalFiles({{"Z.DDS", 2}, {"A.EDF", 1}, {"x.txt", 9}}, out), "canonical");
    CHECK(out.size() == 2 && out[0].name == "a.edf" && out[1].name == "z.dds", "sort/lower");
    CHECK(!ridersync::CanonicalFiles({{"x.dds", 1}}, out), "no edf");
    CHECK(!ridersync::CanonicalFiles({{"x.edf", 64ull * 1024 * 1024 + 1}}, out), "single cap");
    std::vector<ridersync::FileEntry> many(65, {"x.edf", 1});
    CHECK(!ridersync::CanonicalFiles(many, out), "file cap");
    CHECK(!ridersync::CanonicalFiles({{"x.edf", 64ull<<20}, {"a.dds", 64ull<<20}, {"b.dds", 64ull<<20}, {"c.dds", 64ull<<20}, {"d.dds", 1}}, out), "total cap");
}

static std::string Hash(ridersync::Part p, const char* name, uint64_t size, const char* bytes) {
    ridersync::GearHasher h; h.Begin(p); h.File(name, size); h.Update(bytes, (size_t)size); return h.Final();
}
static void Hashing() {
    const std::string a = Hash(ridersync::Rider, "body.edf", 3, "abc");
    CHECK(a == Hash(ridersync::Rider, "body.edf", 3, "abc"), "folder independent");
    CHECK(a != Hash(ridersync::Helmet, "body.edf", 3, "abc"), "part");
    CHECK(a != Hash(ridersync::Rider, "body.edf", 3, "abd"), "bytes");
    CHECK(a != Hash(ridersync::Rider, "body.edf", 2, "ab"), "size");
    CHECK(a != Hash(ridersync::Rider, "other.edf", 3, "abc"), "name");
    looksync::Sha256 s; s.Init();
    const char prefix[] = "frostmod-gear-v1"; s.Update(prefix, sizeof(prefix));
    const char part[] = "rider"; s.Update(part, sizeof(part));
    const char file[] = "body.edf"; s.Update(file, sizeof(file));
    const unsigned char le[8] = {3,0,0,0,0,0,0,0}; s.Update(le, 8); s.Update("abc", 3);
    unsigned char digest[32]; s.Final(digest);
    CHECK(a == looksync::Hex16(digest), "direct vector got %s", a.c_str());
}

static void IndexAndDecision() {
    ridersync::GearIndex idx;
    idx.Set(ridersync::Rider, "Zulu", Id('a')); idx.Set(ridersync::Rider, "alpha", Id('a'));
    idx.Set(ridersync::Helmet, "Helm", Id('b')); idx.Set(ridersync::Boots, std::string(32, 'x'), Id('c'));
    std::string v;
    CHECK(idx.Find(ridersync::Rider, Id('a'), v) && v == "alpha", "deterministic %s", v.c_str());
    CHECK(idx.IdOf(ridersync::Rider, "ALPHA", v) && v == Id('a'), "case folder");
    CHECK(idx.Size() == 3, "size %zu", idx.Size());
    looksync::Look look; std::string own[ridersync::kParts] = {"own", "helm", "boots"};
    auto yes = [](ridersync::Part, const std::string&) { return true; };
    auto no = [](ridersync::Part, const std::string&) { return false; };
    CHECK(!ridersync::Decide(nullptr, 7, own, idx, yes).Any(), "no look");
    CHECK(!ridersync::Decide(&look, 7, own, idx, yes).Any(), "empty id");
    look.riderModel = Id('a'); own[0] = "Zulu";
    CHECK(!ridersync::Decide(&look, 7, own, idx, yes).Any(), "same model");
    own[0] = "own";
    CHECK(!ridersync::Decide(&look, 7, own, idx, no).Any(), "not installed");
    auto got = ridersync::Decide(&look, 7, own, idx, yes);
    CHECK(got.key == 7 && got.folder[0] == "alpha", "override");
    look.boots = Id('c');
    CHECK(ridersync::Decide(&look, 7, own, idx, yes).folder[2].empty(), "long name");
    ridersync::Override copy = got;
    CHECK(copy == got, "equal"); copy.key++; CHECK(copy != got, "not equal");
}

static void LookHelpers() {
    looksync::Look a; a.raceNum = 4; a.rider = "Rider"; a.bike = "Bike";
    ridersync::SetModelId(a, ridersync::Rider, Id('a'));
    ridersync::SetModelId(a, ridersync::Helmet, Id('b'));
    ridersync::SetModelId(a, ridersync::Boots, Id('c'));
    CHECK(ridersync::ModelId(a, ridersync::Helmet) == Id('b'), "model helper");
    looksync::Look b;
    CHECK(looksync::ParseClaimJson(looksync::BuildClaimJson(a), b), "round trip parse");
    CHECK(b.riderModel == Id('a') && b.helmet == Id('b') && b.boots == Id('c'), "round trip ids");
}

int main() {
    Files(); Hashing(); IndexAndDecision(); LookHelpers();
    if (g_failures) return 1;
    std::printf("ridersync tests passed\n");
    return 0;
}
