// The master list FrostMod hands MXB App (src/masterlist.h). A wrong decode would either hide a
// server from the app or send it probing a stranger's address, so it runs in CI.

#include "../src/masterlist.h"

#include <cstdio>
#include <cstring>
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

using namespace frostmod::masterlist;

// A row as the browser holds it: v4-mapped address at +0x61, port at +0x71.
static std::vector<uint8_t> Entry(const uint8_t ip[4], unsigned port) {
    std::vector<uint8_t> e(0x1D8, 0);
    uint8_t* a = e.data() + kEntryAddr;
    a[10] = a[11] = 0xFF;
    std::memcpy(a + 12, ip, 4);
    e[kEntryPort] = (uint8_t)(port >> 8);
    e[kEntryPort + 1] = (uint8_t)port;
    return e;
}

int main() {
    const uint8_t pub[4] = {203, 0, 113, 7};
    std::string out;

    auto e = Entry(pub, 54245);
    CHECK(DecodeAddress(e.data(), e.size(), out) && out == "203.0.113.7:54245", "got %s", out.c_str());

    const uint8_t lan[4] = {192, 168, 1, 5}, ten[4] = {10, 0, 0, 1}, s172[4] = {172, 20, 0, 1},
                  zero[4] = {0, 0, 0, 0}, loop[4] = {127, 0, 0, 1};
    for (const uint8_t* ip : {lan, ten, s172, zero, loop}) {
        auto p = Entry(ip, 54210);
        CHECK(!DecodeAddress(p.data(), p.size(), out), "a private address must not be offered");
    }
    const uint8_t s172pub[4] = {172, 32, 0, 1};
    auto q = Entry(s172pub, 54210);
    CHECK(DecodeAddress(q.data(), q.size(), out), "172.32 is public");

    auto noport = Entry(pub, 0);
    CHECK(!DecodeAddress(noport.data(), noport.size(), out), "port 0 is no address");

    auto v6 = Entry(pub, 54210);
    v6[kEntryAddr] = 0x20; // a real IPv6 address, not v4-mapped
    CHECK(!DecodeAddress(v6.data(), v6.size(), out), "IPv6 is not offered");
    CHECK(!DecodeAddress(e.data(), kEntryNeeds - 1, out), "a short entry is refused");
    CHECK(!DecodeAddress(nullptr, 0x1D8, out), "null is refused");

    CHECK(CleanName("  A\tB\nC  ") == "A B C", "name '%s'", CleanName("  A\tB\nC  ").c_str());
    CHECK(CleanName("   ").empty(), "blank name");
    CHECK(CleanName(std::string(200, 'x').c_str()).size() == kMaxName, "name is capped");

    std::vector<Row> pass;
    CHECK(AddRow(pass, e.data(), e.size(), "Frost's Server"), "first row");
    CHECK(!AddRow(pass, e.data(), e.size(), "Again"), "same address twice");
    CHECK(!AddRow(pass, e.data(), e.size(), ""), "no name");
    const uint8_t pub2[4] = {198, 51, 100, 9};
    auto e2 = Entry(pub2, 54210);
    CHECK(AddRow(pass, e2.data(), e2.size(), "Second"), "second row");
    CHECK(pass.size() == 2, "rows %zu", pass.size());

    const std::string text = Serialize(pass, 1234);
    CHECK(text == "frostmod-masterlist 1 1234\n203.0.113.7:54245\tFrost's Server\n198.51.100.9:54210\tSecond\n",
          "serialised:\n%s", text.c_str());

    if (g_failures == 0) std::printf("masterlist_test: all passed\n");
    return g_failures ? 1 : 0;
}
