// Trainer records (src/trainer.h): what gets cleared, and what must survive.
// Pure C++, runs anywhere.

#include "../src/trainer.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            ++g_failures;                                    \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n  (%s)\n", #cond);                \
        }                                                    \
    } while (0)

using namespace trainer;

static std::vector<uint8_t> Record() { return std::vector<uint8_t>(kRecordSize, 0); }

static void Put(std::vector<uint8_t>& r, size_t at, const char* s) {
    std::memcpy(r.data() + at, s, std::strlen(s) + 1);
}

/// A record the trainer UI wrote: all three names real. Nothing may be touched.
static void AGoodRecordIsLeftAlone() {
    auto r = Record();
    Put(r, kBikeAt, "2027_K85M");
    Put(r, kSeriesAt, "MX2");
    Put(r, kTyreAt, "soft");
    const auto before = r;
    CHECK(Sanitise(r.data(), r.size()) == 0, "nothing to fix");
    CHECK(r == before, "and nothing was changed");

    // The session saver leaves both fields empty, which is the game's own "none" and is what
    // the legacy loader writes. It must read as fine, or every clean trainer gets rewritten.
    auto empty = Record();
    Put(empty, kBikeAt, "2027_K85M");
    CHECK(Sanitise(empty.data(), empty.size()) == 0, "empty is a value, not damage");
}

/// The real thing: the saver never wrote the series field, so the stack is on disk. The
/// reported sample holds a 32-bit stack address at the start of it.
static void TheStackAddressIsCleared() {
    auto r = Record();
    Put(r, kBikeAt, "2027_K85M");
    const uint8_t addr[4] = {0xF0, 0xF7, 0x2F, 0x01};  // 0x012FF7F0, as found in the dump
    std::memcpy(r.data() + kSeriesAt, addr, sizeof(addr));
    std::memcpy(r.data() + kSeriesAt + 4, "\x7f\x00\x11\x88", 4);

    CHECK(!LooksLikeName(r.data() + kSeriesAt, kNameLen), "that is not a name");
    CHECK(Sanitise(r.data(), r.size()) == 1, "one field fixed");
    for (size_t i = 0; i < kNameLen; ++i)
        CHECK(r[kSeriesAt + i] == 0, "series byte %zu cleared", i);
    // An empty series is what makes the reader take its safe branch and never call the lookup
    // whose untested return value is the other half of this crash.
    CHECK(r[kSeriesAt] == 0, "the reader will see an empty string");
    // The bike name it was found by is untouched.
    CHECK(std::string((const char*)r.data() + kBikeAt) == "2027_K85M", "the bike name stands");
}

/// A field with no terminator inside it is not a string however printable it looks: the reader
/// runs off the end of the field and into the next one.
static void AnUnterminatedFieldIsCleared() {
    auto r = Record();
    Put(r, kBikeAt, "2027_K85M");
    std::memset(r.data() + kTyreAt, 'A', kNameLen);  // 0x20 printable bytes, no NUL
    CHECK(Sanitise(r.data(), r.size()) == 1, "the tyre field was not a string");
    CHECK(r[kTyreAt] == 0, "cleared");
}

/// A bike name that fills its field with no terminator is forced to end inside it, rather than
/// cleared: it is what the record is found by, and an empty one loses the rider their trainer.
static void TheBikeNameIsClampedNotCleared() {
    auto r = Record();
    std::memset(r.data() + kBikeAt, 'K', kNameLen);
    CHECK(Sanitise(r.data(), r.size()) >= 1, "it needed fixing");
    CHECK(r[kBikeAt] == 'K', "the name is still there");
    CHECK(r[kBikeAt + kNameLen - 1] == 0, "and it ends inside its own field");
    CHECK(std::strlen((const char*)r.data() + kBikeAt) == kNameLen - 1, "exactly one field long");
}

/// The legacy 0x53C record reaches the same reader, and the game zeroes these fields itself on
/// that path. Anything that is not the version-6 record is left alone.
static void OnlyTheRecordWeKnowIsTouched() {
    auto r = Record();
    std::memcpy(r.data() + kSeriesAt, "\x01\x02\x03\x04", 4);
    const auto before = r;
    CHECK(Sanitise(r.data(), 0x53C) == 0, "a legacy record is not ours to fix");
    CHECK(Sanitise(nullptr, kRecordSize) == 0, "no record, nothing to do");
    CHECK(r == before, "and it was not written to");
}

int main() {
    AGoodRecordIsLeftAlone();
    TheStackAddressIsCleared();
    AnUnterminatedFieldIsCleared();
    TheBikeNameIsClampedNotCleared();
    OnlyTheRecordWeKnowIsTouched();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("trainer: all checks passed\n");
    return 0;
}
