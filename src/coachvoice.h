// coachvoice.h - MXB Coach's spoken cues: the settings file, which clip a cue speaks, when a
// new cue may cut off the one playing, and the clips' PCM.
//
// Each cue kind has one short clip, generated offline (tools/voice/make_clips.py) and embedded
// in mxbcoach.dlo as a resource. The plugin speaks a cue the moment it shows, in practice only,
// through winmm's waveOut. No Win32 here, so tests/coachvoice_test.cpp runs anywhere.
//
// Settings, written by MXB Coach to <save path>\mxbcoach\cues\voice.ini:
//   [voice]
//   enabled=1     ; 1 to speak. Off when the file or the key is missing.
//   volume=80     ; 0-100, applied to the samples
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
// A clip's resource id in the DLL is this plus its cue kind.
constexpr int kResourceBase = 100;

// One clip per cue kind, BRAKE (1) to SIT (10), in kind order: the file name under src/voice/
// and what it says. The words match MXB Coach's cue texts (cues.rs).
struct ClipInfo {
    const char* name;
    const char* text;
};
constexpr ClipInfo kClips[] = {
    {"brake", "Brake"},         {"off_brakes", "Off the brakes"}, {"gas", "Gas"},
    {"shift_up", "Shift up"},   {"shift_down", "Shift down"},     {"go_wide", "Go wide"},
    {"cut_inside", "Cut inside"}, {"scrub", "Scrub it"},          {"stand_up", "Stand up"},
    {"sit_down", "Sit down"},
};
constexpr int kClipCount = int(sizeof(kClips) / sizeof(kClips[0]));
static_assert(kClipCount == coachcue::SIT, "one clip per cue kind, BRAKE to SIT");

/// The clip a cue speaks, by its kind whatever its text says, or -1 for none (a custom cue).
inline int ClipFor(uint8_t kind) {
    return kind >= coachcue::BRAKE && kind <= coachcue::SIT ? int(kind) - 1 : -1;
}

struct Settings {
    bool enabled = false;
    int  volume  = kDefaultVolume;
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
