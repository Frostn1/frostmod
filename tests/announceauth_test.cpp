// A13: FrostServer's announce token (src/announceauth.h). Pure logic, builds and runs anywhere.

#include "../src/announceauth.h"

#include <cstdio>
#include <string>

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

using namespace announceauth;

int main() {
    const std::string good = "0123456789abcdefghijklmnop";   // 26 characters

    {   // length
        CHECK(!TokenUsable(""), "empty is off");
        CHECK(!TokenUsable("hunter2"), "a short password is refused");
        CHECK(!TokenUsable(std::string(kMinToken - 1, 'x')), "one under the minimum");
        CHECK(TokenUsable(std::string(kMinToken, 'x')), "exactly the minimum");
        CHECK(TokenUsable(good), "a long one");
    }

    {   // matching
        CHECK(TokenMatches(good, good), "the same token");
        CHECK(!TokenMatches(good.substr(0, good.size() - 1), good), "a prefix is not the token");
        CHECK(!TokenMatches(good + "x", good), "nor is the token plus more");
        CHECK(!TokenMatches("", good), "nor nothing");
        std::string flip = good; flip[good.size() - 1] ^= 1;
        CHECK(!TokenMatches(flip, good), "last byte differs");
        CHECK(BearerToken("Bearer " + good) == good, "Bearer header");
        CHECK(BearerToken("Basic " + good).empty(), "another scheme gives nothing");
        CHECK(BearerToken(good).empty(), "no scheme gives nothing");
    }

    {   // backoff
        FailGate g;
        const uint32_t a = 0x0100007F, b = 0x0200007F;
        uint64_t t = 1000;
        for (int i = 0; i < FailGate::kFreeFails; ++i) {
            CHECK(!g.Blocked(a, t), "free attempt %d", i);
            g.Fail(a, t);
        }
        CHECK(!g.Blocked(a, t), "the free attempts alone don't lock");
        g.Fail(a, t);
        CHECK(g.Blocked(a, t), "one more wrong token locks the address");
        CHECK(g.Blocked(a, t + FailGate::kBaseMs - 1), "for the base time");
        CHECK(!g.Blocked(a, t + FailGate::kBaseMs), "and then lets it try again");
        CHECK(!g.Blocked(b, t), "another address is not affected");

        t += FailGate::kBaseMs;
        g.Fail(a, t);
        CHECK(g.Blocked(a, t + FailGate::kBaseMs), "the next lockout is longer");
        CHECK(!g.Blocked(a, t + 2 * FailGate::kBaseMs), "twice as long");

        for (int i = 0; i < 40; ++i) g.Fail(a, t);
        CHECK(g.Blocked(a, t + FailGate::kMaxMs - 1), "capped at the maximum");
        CHECK(!g.Blocked(a, t + FailGate::kMaxMs), "and no longer");

        g.Ok(a);
        CHECK(!g.Blocked(a, t), "a right token clears it");

        FailGate big;
        for (uint32_t ip = 1; ip <= FailGate::kMaxIps + 50; ++ip) big.Fail(ip, 0);
        CHECK(big.Size() <= FailGate::kMaxIps, "the table stays bounded, got %zu", big.Size());
    }

    if (g_failures == 0) std::printf("announceauth_test: all checks passed\n");
    else                 std::printf("announceauth_test: %d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
