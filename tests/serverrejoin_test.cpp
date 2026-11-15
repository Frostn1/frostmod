// serverrejoin.h: the bytes we check decode to the table we read, and the owner rule.
#include "../src/serverrejoin.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); } } while (0)

using namespace serverrejoin;

int main() {
    CHECK(DecodeTable(kRemoveWalk) == kRvaEntities, "the remove function walks the table we read");
    CHECK(kRemoveWalk[23] == 0x39 && kRemoveWalk[24] == 0x70 && kRemoveWalk[25] == 0xfc, "in-use flag at -4");
    CHECK(kRemoveWalk[34] == 0xa8 && kRemoveWalk[35] == 0x51 && kRemoveWalk[36] == 0x01, "stride 0x151A8");
    CHECK(kLeft[kLeftLen - 2] == 0x8b && kLeft[kLeftLen - 1] == 0x11, "client id is [rcx]");
    struct E { int32_t flag, id, owner; };
    static const E table[kEntities] = {{1, 10, 7}, {0, 11, 7}, {1, 12, 8}, {1, 13, 7}};
    int32_t out[kEntities];
    const int n = OwnedBy(7, [](int i, int32_t* f, int32_t* id, int32_t* o) {
        *f = table[i].flag; *id = table[i].id; *o = table[i].owner; return true; }, out, kEntities);
    CHECK(n == 2 && out[0] == 10 && out[1] == 13, "only live entities of that client");
    const int m = OwnedBy(7, [](int i, int32_t* f, int32_t* id, int32_t* o) {
        *f = table[i].flag; *id = table[i].id; *o = table[i].owner; return true; }, out, 1);
    CHECK(m == 1, "bounded by max");
    if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
    std::printf("serverrejoin: all checks passed\n");
    return 0;
}
