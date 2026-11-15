// What counts as one caught crash, and the line MXB App reads (src/protection.h).
#include "../src/protection.h"

#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); } \
    } while (0)

using namespace protection;

static void TestOncePerSession() {
    Ledger l;
    CHECK(l.Take() == 0, "nothing fired");
    l.Catch(Kind::PhysicsNan); l.Catch(Kind::PhysicsNan); l.Catch(Kind::RenderBatch);
    const uint32_t m = l.Take();
    CHECK(m == ((1u << (int)Kind::PhysicsNan) | (1u << (int)Kind::RenderBatch)), "both kinds, once");
    l.Catch(Kind::PhysicsNan);
    CHECK(l.Take() == 0, "the same kind later in the session is not counted again");
    l.NewSession();
    CHECK(l.Take() == 0, "a new session alone writes nothing");
    l.Catch(Kind::PhysicsNan);
    CHECK(l.Take() == (1u << (int)Kind::PhysicsNan), "a new session counts it again");
    l.Catch((Kind)99);
    CHECK(l.Take() == 0, "an unknown kind is ignored");
}

static void TestLine() {
    const std::string s = Line(Kind::RiderInfo, 1794531999, "Vice \"city\"", "a\\b\x01", "0.52.0");
    CHECK(s == "{\"t\":1794531999,\"kind\":\"rider-info\",\"site\":\"mxbikes.exe+0x7A860\","
               "\"track\":\"Vice \\\"city\\\"\",\"server\":\"a\\\\b\\u0001\",\"v\":\"0.52.0\"}\n",
          "line format");
    for (int i = 0; i < kKinds; ++i) {
        const KindInfo& in = Info((Kind)i);
        CHECK(in.id && in.id[0] && in.site && in.name, "every kind is named");
        for (int j = 0; j < i; ++j) CHECK(std::string(in.id) != Info((Kind)j).id, "ids are unique");
    }
}

static void TestKeepNewest() {
    CHECK(KeepNewest("a\nb\nc\n", 5) == "a\nb\nc\n", "short file kept");
    CHECK(KeepNewest("a\nb\nc\n", 2) == "b\nc\n", "newest two");
    CHECK(KeepNewest("", 2).empty(), "empty");
}

int main() {
    TestOncePerSession();
    TestLine();
    TestKeepNewest();
    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("protection: all checks passed\n");
    return 0;
}
