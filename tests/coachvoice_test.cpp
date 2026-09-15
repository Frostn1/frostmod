// MXB Coach's spoken cues (src/coachvoice.h).
//
// The settings file MXB Coach writes, which clip a cue speaks, when a newer cue may cut off
// the one playing, the volume applied to the samples, and the clips committed under
// src/voice/, which mxbcoach.dlo embeds. Pure C++, runs anywhere.

#include "../src/coachvoice.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef COACHVOICE_CLIP_DIR
#define COACHVOICE_CLIP_DIR "src/voice"
#endif

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

static std::string TempDir() {
    const char* t = std::getenv("TMPDIR");
    if (!t) t = std::getenv("TEMP");  // Windows
    std::string d = t ? t : "/tmp";
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d;
}

static std::vector<uint8_t> ReadAll(const std::string& path) {
    std::vector<uint8_t> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}

// A WAV file as the generator writes one, with an optional chunk before the samples.
static std::vector<uint8_t> Wav(const std::vector<int16_t>& s, uint32_t rate = coachvoice::kRate,
                                uint16_t channels = 1, uint16_t bits = 16, bool extra_chunk = false) {
    std::vector<uint8_t> b;
    auto put   = [&](const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
    auto put32 = [&](uint32_t v) { put(&v, 4); };
    auto put16 = [&](uint16_t v) { put(&v, 2); };
    put("RIFF", 4);
    put32(0);
    put("WAVE", 4);
    put("fmt ", 4);
    put32(16);
    put16(1);
    put16(channels);
    put32(rate);
    put32(rate * channels * bits / 8);
    put16(uint16_t(channels * bits / 8));
    put16(bits);
    if (extra_chunk) {
        put("LIST", 4);
        put32(3);  // odd: padded to 4
        put("abc\0", 4);
    }
    put("data", 4);
    put32(uint32_t(s.size() * 2));
    put(s.data(), s.size() * 2);
    const uint32_t riff = uint32_t(b.size() - 8);
    std::memcpy(b.data() + 4, &riff, 4);
    return b;
}

static void ReadsTheSettings() {
    coachvoice::Settings s = coachvoice::ParseSettings("[voice]\nenabled=1\nvolume=80\n");
    CHECK(s.enabled && s.volume == 80, "enabled=%d volume=%d", s.enabled, s.volume);

    s = coachvoice::ParseSettings("; MXB Coach\r\n[voice]\r\nenabled = 0\r\nvolume = 35\r\n");
    CHECK(!s.enabled && s.volume == 35, "off, 35: enabled=%d volume=%d", s.enabled, s.volume);

    s = coachvoice::ParseSettings("[voice]\nenabled=true\n");
    CHECK(s.enabled && s.volume == coachvoice::kDefaultVolume, "volume defaults: %d", s.volume);

    CHECK(coachvoice::ParseSettings("[voice]\nvolume=150\n").volume == 100, "clamped to 100");
    CHECK(coachvoice::ParseSettings("[voice]\nvolume=-5\n").volume == 0, "clamped to 0");
    CHECK(coachvoice::ParseSettings("[voice]\nvolume=loud\n").volume == coachvoice::kDefaultVolume, "junk ignored");
    CHECK(coachvoice::ParseSettings("[voice]\nvolume=50%\n").volume == coachvoice::kDefaultVolume, "junk ignored");
    CHECK(!coachvoice::ParseSettings("").enabled, "empty is off");
    CHECK(!coachvoice::ParseSettings("[voice]\nenabled=2\n").enabled, "only a clear yes turns it on");
    CHECK(!coachvoice::ParseSettings("[other]\nenabled=1\n").enabled, "another section doesn't count");
}

static void AMissingFileIsOff() {
    const std::string path = TempDir() + "coachvoice_test_voice.ini";
    std::remove(path.c_str());
    CHECK(!coachvoice::ReadSettings(path).enabled, "missing file is off");

    std::FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr, "can write %s", path.c_str());
    if (!f) return;
    std::fputs("[voice]\nenabled=1\nvolume=60\n", f);
    std::fclose(f);
    const coachvoice::Settings s = coachvoice::ReadSettings(path);
    CHECK(s.enabled && s.volume == 60, "read back: enabled=%d volume=%d", s.enabled, s.volume);
    std::remove(path.c_str());
}

static void EachKindHasItsClip() {
    CHECK(coachvoice::ClipFor(coachcue::BRAKE) == 0, "brake");
    CHECK(coachvoice::ClipFor(coachcue::SIT) == coachvoice::kClipCount - 1, "sit");
    CHECK(coachvoice::ClipFor(coachcue::CUSTOM) == -1, "a custom cue isn't spoken");
    CHECK(coachvoice::ClipFor(0) == -1, "nor kind 0");
    CHECK(coachvoice::ClipFor(200) == -1, "nor an unknown kind");
    CHECK(std::strcmp(coachvoice::kClips[coachvoice::ClipFor(coachcue::OFF_BRAKES)].text, "Off the brakes") == 0,
          "off the brakes");
    CHECK(std::strcmp(coachvoice::kClips[coachvoice::ClipFor(coachcue::THROTTLE)].text, "Gas") == 0, "gas");
    CHECK(std::strcmp(coachvoice::kClips[coachvoice::ClipFor(coachcue::STAND)].text, "Stand up") == 0, "stand");
}

static void AMoreImportantCueCutsIn() {
    CHECK(coachvoice::Choose(false, 0, 10) == coachvoice::SPEAK, "quiet: speak");
    CHECK(coachvoice::Choose(false, 255, 1) == coachvoice::SPEAK, "quiet: speak, whatever came before");
    CHECK(coachvoice::Choose(true, 100, 200) == coachvoice::CUT_IN, "more important cuts in");
    CHECK(coachvoice::Choose(true, 200, 100) == coachvoice::SKIP, "less important waits its turn");
    CHECK(coachvoice::Choose(true, 150, 150) == coachvoice::SKIP, "as important doesn't overlap");
}

static void VolumeScalesTheSamples() {
    const std::vector<int16_t> in = {0, 1000, -1000, 32767, -32768, 3, -3};
    CHECK(coachvoice::Scale(in, 100) == in, "100 leaves them");
    const std::vector<int16_t> zero = coachvoice::Scale(in, 0);
    CHECK(std::all_of(zero.begin(), zero.end(), [](int16_t s) { return s == 0; }), "0 is silent");
    const std::vector<int16_t> half = coachvoice::Scale(in, 50);
    CHECK(half[1] == 500 && half[2] == -500, "half: %d %d", half[1], half[2]);
    CHECK(half[3] == 16384 && half[4] == -16384, "extremes: %d %d", half[3], half[4]);
    CHECK(half[5] == 2 && half[6] == -2, "rounded: %d %d", half[5], half[6]);
    CHECK(coachvoice::Scale(in, 250) == in, "never louder than the clip");
}

static void ReadsTheClipFormatOnly() {
    const std::vector<int16_t> s = {1, -2, 3, -4};
    std::vector<int16_t> out;
    CHECK(coachvoice::ParseWav(Wav(s).data(), Wav(s).size(), out) && out == s, "plain");
    out.clear();
    const std::vector<uint8_t> extra = Wav(s, coachvoice::kRate, 1, 16, true);
    CHECK(coachvoice::ParseWav(extra.data(), extra.size(), out) && out == s, "skips a padded chunk");
    const std::vector<uint8_t> r44 = Wav(s, 44100), st = Wav(s, coachvoice::kRate, 2), b8 = Wav(s, coachvoice::kRate, 1, 8);
    CHECK(!coachvoice::ParseWav(r44.data(), r44.size(), out), "another rate");
    CHECK(!coachvoice::ParseWav(st.data(), st.size(), out), "stereo");
    CHECK(!coachvoice::ParseWav(b8.data(), b8.size(), out), "8-bit");
    const std::vector<uint8_t> good = Wav(s);
    CHECK(!coachvoice::ParseWav(good.data(), good.size() - 3, out), "truncated");
    CHECK(!coachvoice::ParseWav(good.data(), 11, out), "too short");
    CHECK(!coachvoice::ParseWav(nullptr, 0, out), "nothing");
}

// The voice speaks only a cue the player fired: never one it skipped, and never outside practice.
static void SpeaksOnlyWhatShows() {
    std::vector<uint8_t> b(coachcue::kMagic, coachcue::kMagic + 4);
    auto put = [&](const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
    const uint32_t v = coachcue::kVersion, max = 4, count = 2;
    const float len = 1000, lead = 1.2f, show = 1.5f, gap = 3.0f, at1 = 300, at2 = 700;
    put(&v, 4); put(&len, 4); put(&lead, 4); put(&show, 4); put(&gap, 4); put(&max, 4); put(&count, 4);
    for (float at : {at1, at2}) {
        const uint16_t tl = 5;
        put(&at, 4);
        b.push_back(coachcue::BRAKE);
        b.push_back(100);
        put(&tl, 2);
        put("Brake", 5);
    }
    coachcue::Sheet s;
    CHECK(coachcue::Parse(b.data(), b.size(), s), "sheet");
    coachcue::Player p;
    p.load(s);
    CHECK(p.fires() == 0, "nothing yet");
    float t = 0;
    for (float m = 0; m < 1000; m += 0.5f, t += 0.02f) p.on_sample(m / 1000, 25, t, false);
    CHECK(p.fires() == 0, "not in practice");

    p.set_practice(true);
    // Join the lap past the first spot: that one is skipped, and must not be spoken.
    uint32_t fired = p.fires();
    int      spoken = 0;
    for (float m = 400; m < 1000; m += 0.5f, t += 0.02f) {
        p.on_sample(m / 1000, 25, t, false);
        if (p.fires() != fired) {
            fired = p.fires();
            ++spoken;
            CHECK(p.showing() && std::fabs(p.showing()->at_m - at2) < 0.1f, "the second cue");
        }
    }
    CHECK(spoken == 1, "spoke %d", spoken);
}

// The clips mxbcoach.dlo embeds: all there, in the device's format, short and at a sensible level.
static void TheCommittedClips() {
    size_t total = 0;
    for (const coachvoice::ClipInfo& c : coachvoice::kClips) {
        const std::string path = std::string(COACHVOICE_CLIP_DIR) + "/" + c.name + ".wav";
        const std::vector<uint8_t> b = ReadAll(path);
        std::vector<int16_t> s;
        CHECK(coachvoice::ParseWav(b.data(), b.size(), s), "%s parses", path.c_str());
        total += b.size();
        if (s.empty()) continue;
        const double secs = double(s.size()) / coachvoice::kRate;
        CHECK(secs > 0.15 && secs < 1.3, "%s is %.2f s", c.name, secs);
        int peak = 0;
        for (int16_t x : s) peak = (std::max)(peak, std::abs(int(x)));
        CHECK(peak > 16000 && peak < 32767, "%s peaks at %d", c.name, peak);
        // Trimmed: starts and ends quiet, with no long silence either side.
        CHECK(std::abs(int(s.front())) < 2000 && std::abs(int(s.back())) < 2000, "%s fades", c.name);
    }
    CHECK(total > 0 && total < 1000 * 1000, "clips total %zu bytes", total);
    std::printf("coachvoice: %d clips, %zu bytes\n", coachvoice::kClipCount, total);
}

int main() {
    ReadsTheSettings();
    AMissingFileIsOff();
    EachKindHasItsClip();
    AMoreImportantCueCutsIn();
    VolumeScalesTheSamples();
    ReadsTheClipFormatOnly();
    SpeaksOnlyWhatShows();
    TheCommittedClips();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("coachvoice: all checks passed\n");
    return 0;
}
