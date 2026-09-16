// coachlog.h - MXB Coach recorder's diagnostic log: what the plugin decided, and why.
//
// The recorder makes a series of decisions the rider cannot see: whether it registered a font
// to draw with, whether the cue and HUD sheets were accepted, whether this session counts as
// practice, whether the sound device opened. Every one of them fails silently today, so a
// report of "the HUD does not work" cannot be told apart from "the sheet was for another
// track" or "the session was a race". This writes them down.
//
// It goes to <save path>\mxbcoach\mxbcoach.log, which the rider can send us. Only the plugin's
// own decisions go in it: no rider name, no GUID, no server address.
//
// No Win32 here, so tests/coachlog_test.cpp runs anywhere. mxbcoach.cpp holds the file handle
// and the clock; everything that decides what a line *says* lives here, where it is pinned.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coachlog {

/// The log is a diagnostic, not a record: past this it is rewritten from the top rather than
/// grown without limit. Generous enough to hold a long session's cue lines.
constexpr size_t kMaxBytes = 256 * 1024;

/// The longest any one value off the game or a file is allowed to be in a line. Track, bike and
/// setup names are content, and content is not always short or well behaved.
constexpr size_t kMaxValue = 120;

/// A value safe to put in a log line: printable ASCII on one line, bounded.
///
/// These strings come from the game's own structs and from files the rider can edit, so they
/// are not trusted to be short, single-line, or even text. A newline in a track name would
/// forge a log entry; a control character would make the file unreadable in a chat window.
inline std::string Safe(const std::string& in) {
    std::string out;
    out.reserve((std::min)(in.size(), kMaxValue));
    for (char raw : in) {
        if (out.size() >= kMaxValue) {
            out += "...";
            break;
        }
        const unsigned char c = static_cast<unsigned char>(raw);
        // Past ASCII becomes '?', as the game's own text drawing does: one mark per byte,
        // rather than smuggling raw bytes into a file someone will paste somewhere.
        out += c >= 0x7F ? '?' : c < 0x20 ? ' ' : raw;
    }
    return out;
}

/// One line: "[stamp] tag: message". The stamp is the caller's, so this stays clock-free.
inline std::string Line(const std::string& stamp, const std::string& tag, const std::string& message) {
    return "[" + Safe(stamp) + "] " + Safe(tag) + ": " + Safe(message) + "\n";
}

/// Whether adding `adding` bytes to a log already `have` bytes long should start it over.
inline bool ShouldRestart(size_t have, size_t adding) {
    return have + adding > kMaxBytes;
}

// ---------------------------------------------------------------------------------------
// The lines that answer the questions we actually get asked. Composed here, and pinned by the
// test, so the wording cannot drift into something that reads the same but says less.

/// What the event is, and so whether cues and the HUD can ever show. `type` is EventInit's
/// m_iType: 1 testing, 2 race, 4 straight rhythm.
inline std::string EventText(int type, const std::string& track, const std::string& bike, float track_len,
                             const std::string& server) {
    const char* name = type == 1 ? "testing" : type == 2 ? "race" : type == 4 ? "straight rhythm" : "unknown";
    char len[32];
    std::snprintf(len, sizeof(len), "%.1f", double(track_len));
    std::string s = "type=" + std::to_string(type) + " (" + name + ") track=" + Safe(track) + " bike=" + Safe(bike) +
                    " length=" + len + "m";
    // Whether the rider is on a server, because "it works offline but not online" is a report
    // we would otherwise have no way to check. The name itself is not logged.
    s += server.empty() ? " offline" : " on a server";
    return s;
}

/// Whether this stint shows cues and the HUD at all. `session` is RunInit's m_iSession.
inline std::string PracticeText(int type, int session, bool practice) {
    std::string s = "session=" + std::to_string(session) + " practice=" + (practice ? "yes" : "no");
    if (!practice) {
        s += type == 2 ? " - cues and the HUD only show in a race event's practice session"
                       : " - cues and the HUD only show in practice";
    }
    return s;
}

/// What DrawInit registered. Without a font the game has nothing to draw our text with, and
/// every string is dropped in silence, so this line is the first thing to read.
inline std::string DrawInitText(bool font_written, const std::string& path, uint32_t bytes) {
    if (font_written) {
        return "registered 1 font, " + Safe(path) + " (" + std::to_string(bytes) + " bytes)";
    }
    return "could not write " + Safe(path) + " - no font registered, so no text can be drawn";
}

/// What the first Draw call handed the game. Zero quads and zero strings while riding means
/// the HUD was switched off or this is not practice; anything else means it was drawn.
inline std::string DrawText(int state, bool practice, int quads, int strings) {
    return "state=" + std::to_string(state) + " practice=" + (practice ? "yes" : "no") +
           " quads=" + std::to_string(quads) + " strings=" + std::to_string(strings);
}

/// Whether the sound device opened. `mmresult` is waveOutOpen's return; 0 is success.
inline std::string VoiceDeviceText(bool opened, uint32_t mmresult) {
    if (opened) return "device opened";
    return "waveOutOpen failed (" + std::to_string(mmresult) + ") - the cues cannot be spoken";
}

/// voice.ini as it was read, and how many clips are ready to play.
inline std::string VoiceSettingsText(bool found, bool enabled, int volume, int clips) {
    if (!found) return "no voice.ini - the cues are not spoken until MXB Coach turns them on";
    return std::string("enabled=") + (enabled ? "1" : "0") + " volume=" + std::to_string(volume) +
           " clips=" + std::to_string(clips);
}

/// Which sheet was taken, or why none was. A sheet made for another track is the common one:
/// the plugin refuses it rather than showing another track's cues.
inline std::string SheetText(const char* kind, const std::string& name, bool loaded, int items) {
    if (loaded) return std::string(kind) + " " + Safe(name) + ", " + std::to_string(items) + " entries";
    return std::string("no ") + kind + " for this track and bike - MXB Coach has not sent one, or it "
                                        "was made for a different track";
}

}  // namespace coachlog
