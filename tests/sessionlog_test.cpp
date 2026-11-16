// sessionlog_test.cpp - the pure parts of the session-change log lines (src/sessionlog.h).
#include "../src/sessionlog.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                       \
    } while (0)

int main() {
    int session = -1, state = -1;
    CHECK(!sessionlog::ReadSession(nullptr, 8, &session, &state));

    const std::int32_t payload[] = {6, 3};
    CHECK(!sessionlog::ReadSession(payload, 7, &session, &state));
    CHECK(session == -1 && state == -1);
    CHECK(sessionlog::ReadSession(payload, 8, &session, &state));
    CHECK(session == 6);
    CHECK(state == 3);

    const char* const expected[] = {
        "waiting", "practice", "pre-qualify", "qualify practice",
        "qualify", "warmup", "race 1", "race 2"
    };
    for (int i = 0; i < 8; ++i)
        CHECK(std::strcmp(sessionlog::SessionName(i), expected[i]) == 0);
    CHECK(std::strcmp(sessionlog::SessionName(-1), "unknown") == 0);
    CHECK(std::strcmp(sessionlog::SessionName(8), "unknown") == 0);

    CHECK(!sessionlog::ShouldLogRemove(0));
    for (std::uint32_t i = 1; i <= 64; ++i)
        CHECK(sessionlog::ShouldLogRemove(i));
    CHECK(!sessionlog::ShouldLogRemove(65));
    CHECK(!sessionlog::ShouldLogRemove(127));
    CHECK(sessionlog::ShouldLogRemove(128));
    CHECK(sessionlog::ShouldLogRemove(256));
    CHECK(!sessionlog::ShouldLogRemove(257));

    if (g_failures) { std::printf("sessionlog: %d failure(s)\n", g_failures); return 1; }
    std::printf("sessionlog: all checks passed\n");
    return 0;
}
