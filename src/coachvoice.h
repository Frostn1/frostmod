// coachvoice.h - MXB Coach's spoken cues: the settings file, which clip a cue speaks, which
// voice says it, when a new cue may cut off the one playing, and who owns each sound buffer.
//
// Each cue kind has one short clip per voice, generated offline (tools/voice/make_clips.py) and
// embedded in mxbcoach.dlo as a resource. The plugin speaks a cue the moment it shows, in
// practice only, through winmm's waveOut. No Win32 here, so tests/coachvoice_test.cpp runs
// anywhere.
//
// Settings, written by MXB Coach to <save path>\mxbcoach\cues\voice.ini:
//   [voice]
//   enabled=1     ; 1 to speak. Off when the file or the key is missing.
//   volume=80     ; 0-100, applied to the samples
//   voice=female  ; which voice says them: female (the default) or male
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "coachcue.h"
#include "stance.h"  // IniValue

namespace coachvoice {

// Every clip: 22.05 kHz, 16-bit, mono PCM. The device is opened once in this format.
constexpr uint32_t kRate           = 22050;
constexpr int      kDefaultVolume  = 80;
constexpr size_t   kMaxClipSamples = kRate * 3;

// Which voice speaks the cues. MXB Coach writes the key; the rider picks it there, because the
// first thing the first rider to hear the cues asked for was a voice that wasn't a woman's.
enum VoiceId : uint8_t {
    FEMALE = 0,
    MALE   = 1,
};

struct VoiceInfo {
    const char* key;     // what voice.ini says
    const char* folder;  // src/voice/<folder>/
    const char* model;   // the Piper voice the clips were generated from
};

// The order here is the resource order and the order CMakeLists.txt walks, so adding a voice
// means adding it at the END and nowhere else.
constexpr VoiceInfo kVoices[] = {
    {"female", "female", "en_US-ljspeech-high"},
    {"male", "male", "en_US-norman-medium"},
};
constexpr int kVoiceCount = int(sizeof(kVoices) / sizeof(kVoices[0]));

// A clip's resource id in the DLL: 100 + voice * 20 + cue kind. The stride leaves the female
// set on the ids 101-110 it shipped with in v0.22, so the ids already in the wild keep meaning
// what they meant. CMakeLists.txt generates the same numbers and
// tests/coachvoice_res_test.cpp checks the built .dlo carries every one of them.
constexpr int kResourceBase  = 100;
constexpr int kVoiceIdStride = 20;

inline int ResourceId(int voice, uint8_t kind) {
    return kResourceBase + voice * kVoiceIdStride + int(kind);
}

// One clip per spoken cue kind, in kind order: the cue kind it speaks, the file name
// under src/voice/<voice>/ and what it says. The kind is carried explicitly because the
// kinds a clip exists for are no longer contiguous - CUSTOM (11) is drawn and never
// spoken, so ROLL (12) follows SIT (10) here. The words match MXB Coach's cue texts
// (cues.rs), which the plugin never reads: it picks the clip by kind.
struct ClipInfo {
    uint8_t     kind;
    const char* name;
    const char* text;
};
constexpr ClipInfo kClips[] = {
    {coachcue::BRAKE, "brake", "Brake"},
    {coachcue::OFF_BRAKES, "off_brakes", "Off the brakes"},
    {coachcue::THROTTLE, "gas", "Gas"},
    {coachcue::UPSHIFT, "shift_up", "Shift up"},
    {coachcue::DOWNSHIFT, "shift_down", "Shift down"},
    {coachcue::WIDE, "go_wide", "Go wide"},
    {coachcue::INSIDE, "cut_inside", "Cut inside"},
    {coachcue::SCRUB, "scrub", "Scrub it"},
    {coachcue::STAND, "stand_up", "Stand up"},
    {coachcue::SIT, "sit_down", "Sit down"},
    {coachcue::ROLL, "roll_off", "Roll off"},
};
constexpr int kClipCount = int(sizeof(kClips) / sizeof(kClips[0]));
// Every kind from BRAKE to the last one has a clip, except CUSTOM which is never spoken.
static_assert(kClipCount == int(coachcue::ROLL) - 1, "a clip for every kind but CUSTOM");
static_assert(kClips[0].kind == coachcue::BRAKE, "brake first");
static_assert(kClips[kClipCount - 1].kind == coachcue::ROLL, "the newest kind last");

/// The clip a cue speaks, by its kind whatever its text says, or -1 for none: a custom
/// cue, or a kind from a newer MXB Coach than this plugin, which draws and stays silent.
inline int ClipFor(uint8_t kind) {
    for (int i = 0; i < kClipCount; ++i)
        if (kClips[i].kind == kind) return i;
    return -1;
}

/// The voice `key` names, or FEMALE when it names none. Case and spacing don't matter.
inline uint8_t VoiceFor(std::string key) {
    for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (int i = 0; i < kVoiceCount; ++i)
        if (key == kVoices[i].key) return uint8_t(i);
    return FEMALE;
}

struct Settings {
    bool    enabled = false;
    int     volume  = kDefaultVolume;
    uint8_t voice   = FEMALE;
};

inline Settings ParseSettings(const std::string& text) {
    Settings s;
    std::string on = stance::IniValue(text, "voice", "enabled");
    for (char& c : on) c = char(std::tolower(static_cast<unsigned char>(c)));
    s.enabled = on == "1" || on == "true" || on == "yes" || on == "on";
    const std::string v = stance::IniValue(text, "voice", "volume");
    if (!v.empty()) {
        char*      end = nullptr;
        const long n   = std::strtol(v.c_str(), &end, 10);
        if (end != v.c_str() && *end == '\0') s.volume = int((std::max)(0L, (std::min)(100L, n)));
    }
    s.voice = VoiceFor(stance::IniValue(text, "voice", "voice"));
    return s;
}

/// The settings file, or the defaults (off) when it can't be read.
inline Settings ReadSettings(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return Settings{};
    std::string text;
    char        buf[1024];
    size_t      n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < 65536) text.append(buf, n);
    std::fclose(f);
    return ParseSettings(text);
}

/// What to do with a cue that just showed, given what the voice is doing.
enum Action {
    SPEAK,   // nothing playing: speak it
    CUT_IN,  // a less important cue is playing: stop it and speak this one
    SKIP,    // one at least as important is playing: stay quiet rather than overlap
};

inline Action Choose(bool playing, uint8_t playing_priority, uint8_t priority) {
    if (!playing) return SPEAK;
    return priority > playing_priority ? CUT_IN : SKIP;
}

// ---------------------------------------------------------------------------------------
// Who owns each sound buffer.
//
// waveOut plays out of buffers the device borrows and hands back, and the one rule that
// matters is that a buffer must not be touched again until the device has BOTH finished with
// it (winmm sets WHDR_DONE) AND given it back (waveOutUnprepareHeader accepted it).
//
// v0.22 and v0.23 marked a slot free on the strength of the flag alone and cleared its own
// "in use" bit whether or not the unprepare succeeded. A slot the driver still owned was then
// refilled underneath it - its vector reallocated, its header zeroed - and from that moment
// the flags the driver wrote landed in memory that had been reused, so the slot never read as
// finished again. With two slots that is two clips and then silence for the rest of the
// session, which is exactly what the rider got: "Stand up", "Gas", and nothing after.
//
// So ownership lives here, in one place, with no Win32 in it. A slot leaves `free` only when
// it is handed to the device, and returns only when the device has actually given it back.
// tests/coachvoice_test.cpp plays a long session through it and checks the buffers come back.
class Queue {
public:
    static constexpr int kSlots = 2;

    /// A free slot to fill for a cue at `priority`, or -1 when the device holds them all.
    int take(uint8_t priority) {
        for (int i = 0; i < kSlots; ++i) {
            if (!free_[i]) continue;
            free_[i]  = false;
            priority_ = priority;
            ++taken_;
            return i;
        }
        return -1;
    }

    /// Preparing or writing it failed, so the device never took it: ours again, and it was
    /// never spoken, so it must not go on holding the priority either.
    void giveback(int slot) {
        if (!held(slot)) return;
        free_[slot] = true;
        idle_check();
    }

    /// The device finished with the slot and handed it back - only once the unprepare has
    /// actually succeeded. A refused unprepare means the driver still owns the memory, so the
    /// slot stays out and this is not called.
    void release(int slot) {
        if (!held(slot)) return;
        free_[slot] = true;
        ++released_;
        idle_check();
    }

    /// The device is closed: it holds nothing any more, whatever it held before.
    void clear() {
        for (int i = 0; i < kSlots; ++i) free_[i] = true;
        priority_ = 0;
    }

    /// Whether the device still holds a buffer, which is what "a cue is playing" means.
    bool playing() const {
        for (int i = 0; i < kSlots; ++i)
            if (!free_[i]) return true;
        return false;
    }

    /// The priority of the cue last handed over, or 0 when nothing is playing. Falling back to
    /// 0 when idle is the point: a priority left standing from a finished clip would go on
    /// shutting out every quieter cue for the rest of the session.
    uint8_t priority() const { return priority_; }

    int in_flight() const {
        int n = 0;
        for (int i = 0; i < kSlots; ++i)
            if (!free_[i]) ++n;
        return n;
    }

    /// How many buffers have been handed to the device, and how many it has given back. They
    /// differ by exactly what is in flight, and if `released` stops climbing the voice has
    /// stopped.
    uint32_t taken() const { return taken_; }
    uint32_t released() const { return released_; }

private:
    bool held(int slot) const { return slot >= 0 && slot < kSlots && !free_[slot]; }
    void idle_check() {
        if (!playing()) priority_ = 0;
    }

    bool     free_[kSlots] = {true, true};
    uint8_t  priority_     = 0;
    uint32_t taken_        = 0;
    uint32_t released_     = 0;
};

/// A clip's samples at `volume` percent. Scaling down only, so nothing clips.
inline std::vector<int16_t> Scale(const std::vector<int16_t>& in, int volume) {
    const int32_t v = (std::max)(0, (std::min)(100, volume));
    std::vector<int16_t> out(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const int32_t s = int32_t(in[i]) * v;
        out[i]          = int16_t((s + (s >= 0 ? 50 : -50)) / 100);
    }
    return out;
}

/// The samples of a WAV file in the clips' format, 22.05 kHz 16-bit mono PCM. Refuses any
/// other format rather than play it at the wrong speed.
inline bool ParseWav(const uint8_t* b, size_t n, std::vector<int16_t>& out) {
    if (!b || n < 12 || std::memcmp(b, "RIFF", 4) != 0 || std::memcmp(b + 8, "WAVE", 4) != 0) return false;
    bool   fmt_ok = false;
    size_t at     = 12;
    while (at + 8 <= n) {
        uint32_t size;
        std::memcpy(&size, b + at + 4, 4);
        const uint8_t* body = b + at + 8;
        if (size > n - at - 8) return false;
        if (std::memcmp(b + at, "fmt ", 4) == 0) {
            if (size < 16) return false;
            uint16_t format, channels, bits;
            uint32_t rate;
            std::memcpy(&format, body, 2);
            std::memcpy(&channels, body + 2, 2);
            std::memcpy(&rate, body + 4, 4);
            std::memcpy(&bits, body + 14, 2);
            fmt_ok = format == 1 && channels == 1 && rate == kRate && bits == 16;
            if (!fmt_ok) return false;
        } else if (std::memcmp(b + at, "data", 4) == 0) {
            if (!fmt_ok || size % 2 != 0 || size == 0 || size / 2 > kMaxClipSamples) return false;
            out.resize(size / 2);
            std::memcpy(out.data(), body, size);
            return true;
        }
        at += 8 + size + (size & 1);  // chunks are padded to an even length
    }
    return false;
}

}  // namespace coachvoice
