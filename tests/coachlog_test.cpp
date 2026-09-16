// MXB Coach recorder's diagnostic log (src/coachlog.h).
//
// The log exists to answer, from a file the rider can send us, the questions the plugin
// otherwise decides in silence: was a font registered, was the sheet taken, was this practice,
// did the sound device open. So the wording of those lines is pinned here — a line that still
// reads plausibly but no longer says which way the decision went is the failure this catches.
// Pure C++, runs anywhere.

#include "../src/coachlog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

using namespace coachlog;

// A track name is content: the game's struct carries whatever the track author typed, and a
// newline in it would forge a log entry.
static void ValuesAreMadeSafe() {
    CHECK(Safe("indiana") == "indiana", "plain text survives");
    CHECK(Safe("a\nb") == "a b", "a newline cannot split a line: got '%s'", Safe("a\nb").c_str());
    CHECK(Safe("a\r\nb: c") == "a  b: c", "got '%s'", Safe("a\r\nb: c").c_str());
    CHECK(Safe("caf\xC3\xA9") == "caf??", "past ASCII becomes one ? a byte: got '%s'", Safe("caf\xC3\xA9").c_str());
    const std::string long_name(kMaxValue + 40, 'x');
    const std::string cut = Safe(long_name);
    CHECK(cut.size() == kMaxValue + 3 && cut.compare(cut.size() - 3, 3, "...") == 0,
          "a long value is cut and marked: %zu bytes", cut.size());
}

static void OneLine() {
    CHECK(Line("20260915-214530", "drawinit", "registered 1 font") ==
              "[20260915-214530] drawinit: registered 1 font\n",
          "got '%s'", Line("20260915-214530", "drawinit", "registered 1 font").c_str());
}

// The log is a diagnostic, not a record: it restarts rather than growing without limit.
static void TheLogIsCapped() {
    CHECK(!ShouldRestart(0, 100), "an empty log takes a line");
    CHECK(!ShouldRestart(kMaxBytes - 10, 10), "exactly full still fits");
    CHECK(ShouldRestart(kMaxBytes - 10, 11), "one byte over restarts");
}

// Whether cues and the HUD can ever show comes down to the event type and the session, so both
// are written down in words, not just numbers.
static void TheEventAndWhetherItIsPractice() {
    CHECK(EventText(1, "indiana", "MX2_450", 1650.0f, "") ==
              "type=1 (testing) track=indiana bike=MX2_450 length=1650.0m offline",
          "got '%s'", EventText(1, "indiana", "MX2_450", 1650.0f, "").c_str());
    // A server is recorded as a fact, never by name.
    const std::string on = EventText(2, "indiana", "MX2_450", 1650.0f, "Frost's server");
    CHECK(on.find("on a server") != std::string::npos, "got '%s'", on.c_str());
    CHECK(on.find("Frost") == std::string::npos, "the server name is not logged: '%s'", on.c_str());
    CHECK(on.find("type=2 (race)") != std::string::npos, "got '%s'", on.c_str());

    CHECK(PracticeText(1, 1, true) == "session=1 practice=yes", "got '%s'", PracticeText(1, 1, true).c_str());
    // The commonest report we get: it works in testing and not in a race. The line has to say so.
    const std::string race = PracticeText(2, 6, false);
    CHECK(race.find("practice=no") != std::string::npos && race.find("race event's practice session") != std::string::npos,
          "a race session explains itself: '%s'", race.c_str());
    const std::string other = PracticeText(4, 3, false);
    CHECK(other.find("only show in practice") != std::string::npos, "got '%s'", other.c_str());
}

// Without a registered font the game drops every string we hand it, in silence. This is the
// first line anyone reading the log should be able to act on.
static void WhatDrawInitRegistered() {
    const std::string ok = DrawInitText(true, "mxbcoach_data\\coach.fnt", 131049);
    CHECK(ok == "registered 1 font, mxbcoach_data\\coach.fnt (131049 bytes)", "got '%s'", ok.c_str());
    const std::string bad = DrawInitText(false, "mxbcoach_data\\coach.fnt", 0);
    CHECK(bad.find("no font registered") != std::string::npos && bad.find("no text can be drawn") != std::string::npos,
          "a failure says what it costs: '%s'", bad.c_str());
}

static void WhatWasDrawn() {
    CHECK(DrawText(0, true, 37, 4) == "state=0 practice=yes quads=37 strings=4", "got '%s'",
          DrawText(0, true, 37, 4).c_str());
    CHECK(DrawText(2, false, 0, 0) == "state=2 practice=no quads=0 strings=0", "got '%s'",
          DrawText(2, false, 0, 0).c_str());
}

static void TheVoice() {
    CHECK(VoiceDeviceText(true, 0) == "device opened", "got '%s'", VoiceDeviceText(true, 0).c_str());
    const std::string bad = VoiceDeviceText(false, 4);
    CHECK(bad.find("waveOutOpen failed (4)") != std::string::npos && bad.find("cannot be spoken") != std::string::npos,
          "the device failure carries its code: '%s'", bad.c_str());
    CHECK(VoiceSettingsText(true, true, 80, 10, "female") == "enabled=1 volume=80 voice=female clips=10", "got '%s'",
          VoiceSettingsText(true, true, 80, 10, "female").c_str());
    // Which voice spoke is the rider's own setting, so the log has to say which one ran: "it
    // still sounds like a woman" and "the male clips are missing" read the same otherwise.
    CHECK(VoiceSettingsText(true, true, 80, 10, "male").find("voice=male") != std::string::npos, "the male voice");
    // Off is the default, and that is not a fault: the line says so rather than reading as one.
    const std::string none = VoiceSettingsText(false, false, 0, 0, "female");
    CHECK(none.find("no voice.ini") != std::string::npos && none.find("until MXB Coach turns them on") != std::string::npos,
          "got '%s'", none.c_str());
    // Clips at zero with the voice on is the "it is on but silent" case, and must be visible.
    CHECK(VoiceSettingsText(true, true, 80, 0, "male") == "enabled=1 volume=80 voice=male clips=0", "got '%s'",
          VoiceSettingsText(true, true, 80, 0, "male").c_str());

    // Every waveOut call, whichever way it went: a success carries its 0 so the log shows the
    // call happened at all, and a failure is marked so it can be found by eye.
    CHECK(VoiceCallText("waveOutWrite", 1, 0) == "waveOutWrite slot=1 mmresult=0", "got '%s'",
          VoiceCallText("waveOutWrite", 1, 0).c_str());
    const std::string still = VoiceCallText("waveOutUnprepareHeader", 0, 33);
    CHECK(still == "waveOutUnprepareHeader slot=0 mmresult=33 FAILED", "got '%s'", still.c_str());
    CHECK(VoiceCallText("waveOutReset", -1, 0) == "waveOutReset mmresult=0", "no slot: '%s'",
          VoiceCallText("waveOutReset", -1, 0).c_str());

    // The buffer count is the line that shows the voice running dry rather than merely quiet.
    CHECK(VoiceBuffersText(12, 12, 0) == "buffers given=12 returned=12 in flight=0", "got '%s'",
          VoiceBuffersText(12, 12, 0).c_str());
    CHECK(VoiceBuffersText(2, 0, 2).find("returned=0") != std::string::npos, "stuck buffers show as given > returned");
}

static void WhichSheet() {
    CHECK(SheetText("cue sheet", "indiana.MX2.cue", true, 4) == "cue sheet indiana.MX2.cue, 4 entries", "got '%s'",
          SheetText("cue sheet", "indiana.MX2.cue", true, 4).c_str());
    const std::string none = SheetText("HUD sheet", "", false, 0);
    CHECK(none.find("no HUD sheet") != std::string::npos && none.find("different track") != std::string::npos,
          "a missing sheet names both reasons: '%s'", none.c_str());
}

// TMPDIR, then TEMP, then /tmp.
static std::string TempDir() {
    const char* t = std::getenv("TMPDIR");
    if (!t) t = std::getenv("TEMP");
    std::string d = t ? t : "/tmp";
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d;
}

// The lines as they land in a file and read back, which is the form we ask the rider for.
static void ToAFileAndBack() {
    const std::string path = TempDir() + "coachlog_test.log";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr, "open %s", path.c_str());
    if (!f) return;
    const std::string text = Line("20260915-214530", "mxbcoach", "0.23.0 starting") +
                             Line("20260915-214531", "drawinit", DrawInitText(true, "mxbcoach_data\\coach.fnt", 131049)) +
                             Line("20260915-214601", "run", PracticeText(2, 6, false));
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);

    std::vector<char> back(text.size() + 64, 0);
    f = std::fopen(path.c_str(), "rb");
    const size_t n = f ? std::fread(back.data(), 1, back.size(), f) : 0;
    if (f) std::fclose(f);
    std::remove(path.c_str());
    CHECK(n == text.size(), "read back %zu of %zu bytes", n, text.size());
    const std::string read(back.data(), n);
    CHECK(read == text, "the file holds exactly the lines composed");
    // Three lines, each its own, whatever the values carried.
    size_t lines = 0;
    for (char c : read) {
        if (c == '\n') ++lines;
    }
    CHECK(lines == 3, "%zu lines", lines);
}

int main() {
    ValuesAreMadeSafe();
    OneLine();
    TheLogIsCapped();
    TheEventAndWhetherItIsPractice();
    WhatDrawInitRegistered();
    WhatWasDrawn();
    TheVoice();
    WhichSheet();
    ToAFileAndBack();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("coachlog: all checks passed\n");
    return 0;
}
