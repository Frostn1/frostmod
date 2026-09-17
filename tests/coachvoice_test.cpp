// MXB Coach's spoken cues (src/coachvoice.h).
//
// The settings file MXB Coach writes, which clip a cue speaks and in which voice, when a newer
// cue may cut off the one playing, the volume applied to the samples, who owns each sound
// buffer, and the clips committed under src/voice/, which mxbcoach.dlo embeds. Pure C++, runs
// anywhere.

#include "../src/coachvoice.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
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

// Which voice says them. The rider picks it in MXB Coach, which writes the key.
static void WhichVoiceSpeaks() {
    CHECK(coachvoice::ParseSettings("[voice]\nenabled=1\n").voice == coachvoice::FEMALE,
          "female until someone asks otherwise");
    CHECK(coachvoice::ParseSettings("[voice]\nenabled=1\nvoice=male\n").voice == coachvoice::MALE, "male when asked");
    CHECK(coachvoice::ParseSettings("[voice]\nvoice=female\n").voice == coachvoice::FEMALE, "female by name");
    CHECK(coachvoice::ParseSettings("[voice]\nvoice=MALE\n").voice == coachvoice::MALE, "any case");
    CHECK(coachvoice::ParseSettings("[voice]\nvoice = male \n").voice == coachvoice::MALE, "spaces around it");
    // An unknown name must fall back rather than leave the rider with no clips at all.
    CHECK(coachvoice::ParseSettings("[voice]\nvoice=robot\n").voice == coachvoice::FEMALE, "an unknown voice");
    CHECK(coachvoice::ParseSettings("[voice]\nvoice=\n").voice == coachvoice::FEMALE, "an empty voice");
    CHECK(coachvoice::VoiceFor("male") == coachvoice::MALE && coachvoice::VoiceFor("") == coachvoice::FEMALE,
          "VoiceFor");
    CHECK(coachvoice::kVoiceCount == 2, "two voices, one of each");
    CHECK(std::strcmp(coachvoice::kVoices[coachvoice::FEMALE].key, "female") == 0 &&
              std::strcmp(coachvoice::kVoices[coachvoice::MALE].key, "male") == 0,
          "the keys voice.ini uses");
}

// The resource ids the DLL embeds the clips under, which CMakeLists.txt generates separately.
// They have to agree, and they have to keep the ids v0.22 shipped.
static void EachVoiceHasItsOwnResources() {
    CHECK(coachvoice::ResourceId(coachvoice::FEMALE, coachcue::BRAKE) == 101, "female brake is still 101");
    CHECK(coachvoice::ResourceId(coachvoice::FEMALE, coachcue::SIT) == 110, "female sit is still 110");
    CHECK(coachvoice::ResourceId(coachvoice::MALE, coachcue::BRAKE) == 121, "male brake");
    CHECK(coachvoice::ResourceId(coachvoice::MALE, coachcue::SIT) == 130, "male sit");
    // No two clips may share an id, or one voice would be playing out of the other's set.
    std::vector<int> ids;
    for (int v = 0; v < coachvoice::kVoiceCount; ++v)
        for (int k = coachcue::BRAKE; k <= coachcue::SIT; ++k) ids.push_back(coachvoice::ResourceId(v, uint8_t(k)));
    for (size_t i = 0; i < ids.size(); ++i)
        for (size_t j = i + 1; j < ids.size(); ++j) CHECK(ids[i] != ids[j], "resource %d used twice", ids[i]);
    // And none may land on the font's id, which is embedded the same way.
    for (int id : ids) CHECK(id != 200, "a clip collides with the HUD font");
}

static void AMissingFileIsOff() {
    const std::string path = TempDir() + "coachvoice_test_voice.ini";
    std::remove(path.c_str());
    CHECK(!coachvoice::ReadSettings(path).enabled, "missing file is off");

    std::FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr, "can write %s", path.c_str());
    if (!f) return;
    std::fputs("[voice]\nenabled=1\nvolume=60\nvoice=male\n", f);
    std::fclose(f);
    const coachvoice::Settings s = coachvoice::ReadSettings(path);
    CHECK(s.enabled && s.volume == 60 && s.voice == coachvoice::MALE, "read back: enabled=%d volume=%d voice=%d",
          s.enabled, s.volume, int(s.voice));
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

// The fault that silenced the voice after two clips, from the other side: play a long session
// through the buffer ring and check every buffer comes back. There are two, so losing even one
// leaves a single clip's worth of voice and then nothing for the rest of the session.
static void TheBufferRingRecycles() {
    coachvoice::Queue q;
    CHECK(!q.playing() && q.priority() == 0 && q.in_flight() == 0, "a fresh ring is idle");

    // 200 cues, each finishing before the next: every one of them finds a buffer.
    int spoken = 0;
    for (int i = 0; i < 200; ++i) {
        const int slot = q.take(100);
        CHECK(slot >= 0, "cue %d found no buffer - the ring has leaked", i);
        if (slot < 0) break;
        ++spoken;
        CHECK(q.playing() && q.in_flight() == 1, "cue %d: %d in flight", i, q.in_flight());
        q.release(slot);
        CHECK(!q.playing() && q.in_flight() == 0, "cue %d never came back", i);
        // The priority must not outlive the clip, or a loud cue would shut out every quieter
        // one for the rest of the session - the second half of the same bug.
        CHECK(q.priority() == 0, "cue %d left its priority standing", i);
    }
    CHECK(spoken == 200, "spoke %d of 200", spoken);
    CHECK(q.taken() == 200 && q.released() == 200, "given %u, returned %u", q.taken(), q.released());
    CHECK(q.taken() == q.released() + uint32_t(q.in_flight()), "every buffer is accounted for");

    // Two at once is all it holds, and the next cue has to wait rather than overrun one.
    const int a = q.take(10), b = q.take(20);
    CHECK(a >= 0 && b >= 0 && a != b, "two different slots: %d %d", a, b);
    CHECK(q.in_flight() == 2 && q.take(30) < 0, "a third cue has nowhere to go");
    q.release(b);
    CHECK(q.playing() && q.in_flight() == 1, "one still out");
    q.release(a);
    CHECK(!q.playing() && q.priority() == 0, "both back, and nothing is playing");
    CHECK(q.taken() == 202 && q.released() == 202, "given %u, returned %u", q.taken(), q.released());

    // A buffer the device refused: ours again, and never spoken, so it leaves no priority.
    const int refused = q.take(200);
    CHECK(refused >= 0 && q.priority() == 200, "taken");
    q.giveback(refused);
    CHECK(!q.playing() && q.priority() == 0, "a refused buffer leaves nothing behind");
    CHECK(q.released() == 202, "a refused buffer was never played: %u", q.released());

    // The whole point, said plainly: a loud cue that has finished does not silence a quiet one.
    const int loud = q.take(255);
    q.release(loud);
    CHECK(coachvoice::Choose(q.playing(), q.priority(), 1) == coachvoice::SPEAK,
          "a quiet cue still speaks once the loud one has finished");

    // Closing the device drops whatever it was holding, however it was holding it.
    q.take(50);
    CHECK(q.playing(), "one out");
    q.clear();
    CHECK(!q.playing() && q.in_flight() == 0 && q.priority() == 0, "closing the device clears the ring");
    CHECK(q.take(1) >= 0 && q.take(1) >= 0, "and both buffers are usable again");
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

// A whole session's worth of cues driven through the player and the buffer ring together, the
// way mxbcoach.cpp drives them: every cue that fires is spoken, for as many laps as it takes.
// The rider heard two clips and then silence, and this is the shape of that report.
static void ASessionOfCuesIsAllSpoken() {
    coachcue::Sheet sheet;
    sheet.track_len   = 1000;
    sheet.max_per_lap = 4;
    sheet.gap_s       = 3.0f;
    for (float m : {150.f, 400.f, 650.f, 880.f}) {
        coachcue::Cue c;
        c.at_m     = m;
        c.kind     = coachcue::BRAKE;
        c.priority = uint8_t(100 + int(m) % 7);  // a mix, so priority is actually compared
        c.text     = "Brake";
        sheet.cues.push_back(c);
    }
    coachcue::Player p;
    p.load(sheet);
    p.set_practice(true);

    coachvoice::Queue q;
    int      spoken = 0, skipped = 0, playing_until = -1;
    uint32_t fired = 0;
    int      sample = 0;
    // 20 laps at 25 m/s, 50 Hz.
    for (int lap = 0; lap < 20; ++lap) {
        for (float m = 0; m < 1000; m += 0.5f, ++sample) {
            const float t = float(sample) * 0.02f;
            // The device finishes with a clip about a second after it started.
            if (playing_until >= 0 && sample >= playing_until) {
                for (int i = 0; i < coachvoice::Queue::kSlots; ++i) q.release(i);
                playing_until = -1;
            }
            p.on_sample(m / 1000, 25, t, false);
            if (p.fires() == fired || !p.showing()) continue;
            fired = p.fires();
            const coachcue::Cue& c = *p.showing();
            if (coachvoice::Choose(q.playing(), q.priority(), c.priority) == coachvoice::SKIP) {
                ++skipped;
                continue;
            }
            const int slot = q.take(c.priority);
            CHECK(slot >= 0, "cue %u on lap %d found no buffer", fired, lap);
            if (slot >= 0) {
                ++spoken;
                playing_until = sample + 50;  // a second of clip
            }
        }
        p.on_lap();
    }
    CHECK(fired >= 70, "the player fired %u cues over 20 laps", fired);
    // Everything that fired was either spoken or deliberately skipped for a louder one - and
    // nothing was lost to a buffer that never came back.
    CHECK(spoken + skipped == int(fired), "fired %u, spoke %d, skipped %d", fired, spoken, skipped);
    CHECK(spoken > 60, "only %d of %u cues were spoken", spoken, fired);
    CHECK(q.taken() == q.released() + uint32_t(q.in_flight()), "every buffer accounted for");
}

// The clips mxbcoach.dlo embeds, in every voice: all there, in the device's format, short, at a
// sensible level, and - the "gss" check - opening on silence rather than part way into a vowel.
static void TheCommittedClips() {
    size_t total = 0;
    // Each clip's length in every voice, so one voice can be held against another below.
    std::vector<std::vector<double>> secs_of(coachvoice::kVoiceCount, std::vector<double>(coachvoice::kClipCount, 0.0));
    for (int v = 0; v < coachvoice::kVoiceCount; ++v) {
        const coachvoice::VoiceInfo& info = coachvoice::kVoices[v];
        for (int ci = 0; ci < coachvoice::kClipCount; ++ci) {
            const coachvoice::ClipInfo& c = coachvoice::kClips[ci];
            const std::string path = std::string(COACHVOICE_CLIP_DIR) + "/" + info.folder + "/" + c.name + ".wav";
            const std::vector<uint8_t> b = ReadAll(path);
            std::vector<int16_t> s;
            CHECK(coachvoice::ParseWav(b.data(), b.size(), s), "%s parses", path.c_str());
            total += b.size();
            if (s.empty()) continue;
            const double secs = double(s.size()) / coachvoice::kRate;
            // The upper bound is a real check, not a formality: asked for one short word the
            // male voice will say it, pause, and then say something else entirely, and a clip
            // that runs long is that happening.
            CHECK(secs > 0.15 && secs < 1.6, "%s %s is %.2f s", info.key, c.name, secs);
            secs_of[size_t(v)][size_t(ci)] = secs;
            int peak = 0;
            for (int16_t x : s) peak = (std::max)(peak, std::abs(int(x)));
            CHECK(peak > 16000 && peak < 32767, "%s %s peaks at %d", info.key, c.name, peak);
            // Trimmed: starts and ends quiet, with no long silence either side.
            CHECK(std::abs(int(s.front())) < 2000 && std::abs(int(s.back())) < 2000, "%s %s fades", info.key, c.name);
            // The lead-in. "Gas" reached the rider as "gss" because the clip began after the
            // consonant that opens it: the trim had scored that quiet burst as silence. A clip
            // that starts on anything but near-silence has been cut into.
            const size_t lead = coachvoice::kRate / 100;  // the first 10 ms
            int head = 0;
            for (size_t i = 0; i < lead && i < s.size(); ++i) head = (std::max)(head, std::abs(int(s[i])));
            CHECK(head < 300, "%s %s opens at %d, so its first sound has been cut into", info.key, c.name, head);
        }
    }
    // One word, said by two voices, cannot differ by half again in length. The flat 1.6 s bound
    // above was too loose to catch what actually shipped: `gas` ran 1.21 s against the female
    // 0.51 s, `stand_up` 1.38 s against 0.68 s -- the word, then a second utterance nobody
    // asked for, and because the clip is normalised over its whole length the real word was
    // scaled down and the rubbish played at full volume. Holding the voices against each other
    // catches that at any absolute length.
    for (int ci = 0; ci < coachvoice::kClipCount; ++ci) {
        double shortest = 1e9, longest = 0;
        const char* long_voice = "";
        for (int v = 0; v < coachvoice::kVoiceCount; ++v) {
            const double t = secs_of[size_t(v)][size_t(ci)];
            if (t <= 0) continue;
            shortest = (std::min)(shortest, t);
            if (t > longest) longest = t, long_voice = coachvoice::kVoices[v].key;
        }
        if (shortest >= 1e9 || longest <= 0) continue;
        CHECK(longest <= shortest * 1.5, "%s runs %.2f s in %s against %.2f s elsewhere: a second utterance",
              coachvoice::kClips[ci].name, longest, long_voice, shortest);
    }
    CHECK(total > 0 && total < 2000 * 1000, "clips total %zu bytes", total);
    std::printf("coachvoice: %d voices x %d clips, %zu bytes\n", coachvoice::kVoiceCount, coachvoice::kClipCount,
                total);
}

int main() {
    ReadsTheSettings();
    WhichVoiceSpeaks();
    EachVoiceHasItsOwnResources();
    AMissingFileIsOff();
    EachKindHasItsClip();
    AMoreImportantCueCutsIn();
    TheBufferRingRecycles();
    VolumeScalesTheSamples();
    ReadsTheClipFormatOnly();
    SpeaksOnlyWhatShows();
    ASessionOfCuesIsAllSpoken();
    TheCommittedClips();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("coachvoice: all checks passed\n");
    return 0;
}
