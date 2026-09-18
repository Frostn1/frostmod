// servermsg.h - styled server announcements, minus the operating system.
//
// A dedicated server can already say things in chat, but it cannot say them in a colour.
// The line is painted by the receiving client from a constant, so the server has no vote:
// every server message is the same red, in the game's own font, and that is the end of it.
//
// So the message does not travel as chat. FrostServer publishes it over its HTTP API and
// FrostMod draws it in its own overlay, where the colour, the animation and the glyphs are
// ours. A rider without FrostMod is unaffected and still sees the server's ordinary chat
// line - this adds a channel, it does not replace one.
//
// Everything here is the part that needs no Win32 and no game: the message model, the feed
// JSON both halves speak, the client's queue, the per-frame colour, and the icon bitmaps.
// tests/servermsg_test.cpp drives all of it on any machine, the way command_channel_test.cpp
// does for cmdchannel.h. The sockets live in frostserver.cpp, the drawing in frostmod.cpp.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace servermsg {

// A message stays on screen for its dwell, then goes. Both ends clamp to this range so a
// typo in the config cannot pin a banner to somebody's screen for the rest of the session.
constexpr float kMinSeconds = 1.0f;
constexpr float kMaxSeconds = 30.0f;
constexpr float kDefaultSeconds = 8.0f;

// What the client keeps. Older entries are dropped rather than queued forever, because a
// rider who joins mid-session wants the next announcement, not the backlog.
constexpr size_t kMaxLive = 6;

// Text longer than this is cut. The overlay draws one line per message and a long one would
// run off the side of the screen with no way for the reader to scroll it.
constexpr size_t kMaxText = 160;

enum class Style {
    None,      // the authored colour, steady
    Pulse,     // brightness breathes
    Fade,      // alpha breathes
    Rainbow,   // hue runs along the string and moves with time
};

inline Style StyleFromName(const std::string& s) {
    if (s == "pulse")   return Style::Pulse;
    if (s == "fade")    return Style::Fade;
    if (s == "rainbow") return Style::Rainbow;
    return Style::None;
}
inline const char* StyleName(Style st) {
    switch (st) {
        case Style::Pulse:   return "pulse";
        case Style::Fade:    return "fade";
        case Style::Rainbow: return "rainbow";
        default:             return "none";
    }
}

struct Message {
    uint32_t    seq     = 0;            // server's monotonic id; the client asks for >since
    std::string text;                   // may contain :icon: tokens
    uint32_t    rgb     = 0xFFFFFFu;    // 0xRRGGBB as authored
    Style       style   = Style::None;
    float       seconds = kDefaultSeconds;
    double      shownAt = 0.0;          // client-side: when it first reached the screen
};

inline float ClampSeconds(float s) {
    if (!(s > 0.0f)) return kDefaultSeconds;            // also catches NaN
    return s < kMinSeconds ? kMinSeconds : (s > kMaxSeconds ? kMaxSeconds : s);
}

// "FF3B00" or "#FF3B00". Returns false on anything else, so a bad colour is reported rather
// than silently drawn as black.
inline bool ParseHexRgb(const std::string& in, uint32_t& out) {
    size_t i = (!in.empty() && in[0] == '#') ? 1 : 0;
    if (in.size() - i != 6) return false;
    uint32_t v = 0;
    for (size_t k = i; k < in.size(); ++k) {
        char c = in[k];
        int d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | (uint32_t)d;
    }
    out = v;
    return true;
}

inline std::string Truncate(const std::string& s) {
    return s.size() <= kMaxText ? s : s.substr(0, kMaxText);
}

// ---------------------------------------------------------------------------
// icons - 8x8 monochrome, tinted by the message colour
//
// Drawn as quads rather than glyphs on purpose. A symbol font would mean depending on a
// particular face being installed and on its private codepoints, neither of which can be
// checked from here; a bitmap can be, and tests/servermsg_test.cpp does. The cost is that
// an icon is one colour - the message's - so these are symbols, not colour emoji.
// ---------------------------------------------------------------------------
constexpr int kIconSize = 8;

struct Icon {
    const char*   name;
    unsigned char rows[kIconSize];   // MSB is the leftmost pixel, row 0 is the top
};

inline const Icon* Icons(size_t& count) {
    static const Icon kIcons[] = {
        { "flag",   { 0xFC, 0xFC, 0xF8, 0xF0, 0xC0, 0xC0, 0xC0, 0xC0 } },
        { "warn",   { 0x18, 0x18, 0x3C, 0x24, 0x66, 0x42, 0xDB, 0xFF } },
        { "star",   { 0x18, 0x18, 0xFF, 0x7E, 0x3C, 0x7E, 0x66, 0xC3 } },
        { "clock",  { 0x3C, 0x42, 0x99, 0x99, 0x9F, 0x81, 0x42, 0x3C } },
        { "trophy", { 0xFF, 0xBD, 0xBD, 0x7E, 0x3C, 0x18, 0x3C, 0x7E } },
        { "check",  { 0x01, 0x03, 0x06, 0x8C, 0xD8, 0x70, 0x20, 0x00 } },
        { "cross",  { 0xC3, 0xE7, 0x7E, 0x3C, 0x3C, 0x7E, 0xE7, 0xC3 } },
        { "bolt",   { 0x1C, 0x38, 0x70, 0xFE, 0x1C, 0x38, 0x70, 0xE0 } },
        { "skull",  { 0x3C, 0x7E, 0xFF, 0xDB, 0xFF, 0x7E, 0x3C, 0x2A } },
        { "heart",  { 0x66, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C, 0x18, 0x00 } },
    };
    count = sizeof(kIcons) / sizeof(kIcons[0]);
    return kIcons;
}

// Index into Icons(), or -1 when the name is not one of ours.
inline int IconIndex(const std::string& name) {
    size_t n = 0;
    const Icon* all = Icons(n);
    for (size_t i = 0; i < n; ++i)
        if (name == all[i].name) return (int)i;
    return -1;
}

// One run of a message: either literal text or one icon. ":flag: go" is {icon flag}, {" go"}.
// An unknown token is left alone, so ":30:" in a message reads as typed rather than vanishing.
struct Span {
    bool        icon = false;
    int         iconIndex = -1;   // valid when icon
    std::string text;             // valid when !icon
};

inline std::vector<Span> Tokenise(const std::string& s) {
    std::vector<Span> out;
    std::string lit;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == ':') {
            size_t end = s.find(':', i + 1);
            // A name is short and has no spaces; anything else is ordinary punctuation.
            if (end != std::string::npos && end > i + 1 && end - i <= 12 &&
                s.find(' ', i + 1) > end) {
                int idx = IconIndex(s.substr(i + 1, end - i - 1));
                if (idx >= 0) {
                    if (!lit.empty()) { Span t; t.text = lit; out.push_back(t); lit.clear(); }
                    Span g; g.icon = true; g.iconIndex = idx; out.push_back(g);
                    i = end + 1;
                    continue;
                }
            }
        }
        lit += s[i++];
    }
    if (!lit.empty()) { Span t; t.text = lit; out.push_back(t); }
    return out;
}

// How many character cells the message occupies - text characters plus one per icon. The
// renderer needs it to spread a rainbow evenly across the whole line.
inline int CellCount(const std::vector<Span>& spans) {
    int n = 0;
    for (const auto& s : spans) n += s.icon ? 1 : (int)s.text.size();
    return n;
}

// ---------------------------------------------------------------------------
// per-frame colour
// ---------------------------------------------------------------------------
struct Rgba { float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f; };

// Ramped in and out for every style, so a message arrives and leaves instead of popping.
constexpr float kFadeInSec  = 0.20f;
constexpr float kFadeOutSec = 0.50f;

inline float DwellAlpha(const Message& m, double nowSec) {
    const float age  = (float)(nowSec - m.shownAt);
    const float life = ClampSeconds(m.seconds);
    if (age <= 0.0f)   return 0.0f;
    if (age >= life)   return 0.0f;
    if (age < kFadeInSec)          return age / kFadeInSec;
    if (age > life - kFadeOutSec)  return (life - age) / kFadeOutSec;
    return 1.0f;
}

inline Rgba HsvToRgb(float h, float s, float v) {
    h = h - std::floor(h);
    const float i = std::floor(h * 6.0f);
    const float f = h * 6.0f - i;
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - f * s);
    const float t = v * (1.0f - (1.0f - f) * s);
    Rgba c;
    switch ((int)i % 6) {
        case 0: c.r = v; c.g = t; c.b = p; break;
        case 1: c.r = q; c.g = v; c.b = p; break;
        case 2: c.r = p; c.g = v; c.b = t; break;
        case 3: c.r = p; c.g = q; c.b = v; break;
        case 4: c.r = t; c.g = p; c.b = v; break;
        default: c.r = v; c.g = p; c.b = q; break;
    }
    return c;
}

// The colour of one cell of one message this frame. `cell` is its position along the line and
// `cells` the total, which only Rainbow uses; the other styles give every cell the same value.
inline Rgba ColorAt(const Message& m, int cell, int cells, double nowSec) {
    const float age = (float)(nowSec - m.shownAt);
    Rgba c;
    c.r = (float)((m.rgb >> 16) & 0xFF) / 255.0f;
    c.g = (float)((m.rgb >>  8) & 0xFF) / 255.0f;
    c.b = (float)( m.rgb        & 0xFF) / 255.0f;
    c.a = DwellAlpha(m, nowSec);

    switch (m.style) {
        case Style::Pulse: {
            const float k = 0.65f + 0.35f * (0.5f + 0.5f * std::sin(age * 5.7f));
            c.r *= k; c.g *= k; c.b *= k;
            break;
        }
        case Style::Fade: {
            c.a *= 0.45f + 0.55f * (0.5f + 0.5f * std::sin(age * 3.4f));
            break;
        }
        case Style::Rainbow: {
            const float spread = cells > 0 ? (float)cell / (float)cells : 0.0f;
            const Rgba hue = HsvToRgb(spread * 0.9f + age * 0.35f, 0.85f, 1.0f);
            const float a = c.a;
            c = hue;
            c.a = a;
            break;
        }
        default: break;
    }
    return c;
}

// ---------------------------------------------------------------------------
// the feed, as JSON
//
// Deliberately not a general parser. Both ends of this wire are in this repo and the shape is
// fixed, so a scanner that knows the shape is smaller, has no dependency, and cannot be fed a
// document that makes it do something surprising. Same reasoning as JsonStringField in
// cmdchannel.h - and like that one, anything it does not recognise is simply skipped.
// ---------------------------------------------------------------------------
inline std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

inline std::string JsonUnescape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        switch (s[++i]) {
            case 'n':  o += '\n'; break;
            case 'r':  o += '\r'; break;
            case 't':  o += '\t'; break;
            case '"':  o += '"';  break;
            case '\\': o += '\\'; break;
            case 'u': {
                // Only the escapes we emit, which are control characters; the rest is dropped
                // rather than guessed at, since the overlay font has nothing to draw it with.
                if (i + 4 < s.size()) i += 4;
                break;
            }
            default: o += s[i]; break;
        }
    }
    return o;
}

// The body of GET /frostserver/messages. `head` is the newest seq the server holds, so a
// client that has fallen behind can tell how far without walking the list.
inline std::string BuildFeedJson(const std::vector<Message>& msgs, uint32_t head) {
    std::string j = "{\"seq\":";
    j += std::to_string(head);
    j += ",\"messages\":[";
    for (size_t i = 0; i < msgs.size(); ++i) {
        const Message& m = msgs[i];
        char col[8];
        std::snprintf(col, sizeof(col), "%06X", m.rgb & 0xFFFFFFu);
        char secs[16];
        std::snprintf(secs, sizeof(secs), "%.2f", ClampSeconds(m.seconds));
        if (i) j += ',';
        j += "{\"seq\":";
        j += std::to_string(m.seq);
        j += ",\"text\":\"";
        j += JsonEscape(m.text);
        j += "\",\"color\":\"";
        j += col;
        j += "\",\"style\":\"";
        j += StyleName(m.style);
        j += "\",\"seconds\":";
        j += secs;
        j += '}';
    }
    j += "]}";
    return j;
}

// Pull "key":"value" out of one object. Honours backslash escapes so a quote inside the text
// does not end the value early.
inline bool ObjectString(const std::string& obj, const char* key, std::string& out) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t k = obj.find(needle);
    if (k == std::string::npos) return false;
    size_t c = obj.find(':', k + needle.size());
    if (c == std::string::npos) return false;
    size_t q = obj.find('"', c + 1);
    if (q == std::string::npos) return false;
    std::string raw;
    for (size_t i = q + 1; i < obj.size(); ++i) {
        if (obj[i] == '\\' && i + 1 < obj.size()) { raw += obj[i]; raw += obj[i + 1]; ++i; continue; }
        if (obj[i] == '"') { out = JsonUnescape(raw); return true; }
        raw += obj[i];
    }
    return false;
}

inline bool ObjectNumber(const std::string& obj, const char* key, double& out) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t k = obj.find(needle);
    if (k == std::string::npos) return false;
    size_t c = obj.find(':', k + needle.size());
    if (c == std::string::npos) return false;
    size_t i = c + 1;
    while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\t')) ++i;
    size_t start = i;
    if (i < obj.size() && (obj[i] == '-' || obj[i] == '+')) ++i;
    while (i < obj.size() && ((obj[i] >= '0' && obj[i] <= '9') || obj[i] == '.')) ++i;
    if (i == start) return false;
    out = std::atof(obj.substr(start, i - start).c_str());
    return true;
}

// One message object. Missing fields take their defaults, so an older server that does not
// send `style` still produces a readable line rather than nothing.
inline bool ParseMessageObject(const std::string& obj, Message& m) {
    std::string s;
    if (!ObjectString(obj, "text", s)) return false;
    m.text = Truncate(s);
    if (m.text.empty()) return false;

    double d = 0.0;
    if (ObjectNumber(obj, "seq", d) && d >= 0.0) m.seq = (uint32_t)d;
    if (ObjectNumber(obj, "seconds", d)) m.seconds = ClampSeconds((float)d);
    if (ObjectString(obj, "color", s)) { uint32_t v; if (ParseHexRgb(s, v)) m.rgb = v; }
    if (ObjectString(obj, "style", s)) m.style = StyleFromName(s);
    return true;
}

// Split "messages":[ {...}, {...} ] into its objects. Brace-counting, and string-aware so a
// brace inside somebody's message text does not break the split.
inline bool ParseFeedJson(const std::string& doc, std::vector<Message>& out, uint32_t& head) {
    out.clear();
    head = 0;
    double d = 0.0;
    if (ObjectNumber(doc, "seq", d) && d >= 0.0) head = (uint32_t)d;

    size_t k = doc.find("\"messages\"");
    if (k == std::string::npos) return false;
    size_t open = doc.find('[', k);
    if (open == std::string::npos) return false;

    int depth = 0;
    size_t start = 0;
    bool inStr = false;
    for (size_t i = open + 1; i < doc.size(); ++i) {
        const char c = doc[i];
        if (inStr) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '{') { if (depth++ == 0) start = i; continue; }
        if (c == '}') {
            if (--depth == 0) {
                Message m;
                if (ParseMessageObject(doc.substr(start, i - start + 1), m)) out.push_back(m);
            }
            continue;
        }
        if (c == ']' && depth == 0) break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// the client's queue
// ---------------------------------------------------------------------------
class Queue {
  public:
    // Everything newer than what we have already seen, stamped with the time it reached the
    // screen. Out-of-order or repeated entries are ignored: the seq is the only authority.
    void Ingest(const std::vector<Message>& in, double nowSec) {
        for (const Message& m : in) {
            if (m.seq <= lastSeq_) continue;
            Message copy = m;
            copy.shownAt = nowSec;
            live_.push_back(copy);
            lastSeq_ = m.seq;
        }
        if (live_.size() > kMaxLive)
            live_.erase(live_.begin(), live_.begin() + (live_.size() - kMaxLive));
    }

    void Expire(double nowSec) {
        size_t w = 0;
        for (size_t i = 0; i < live_.size(); ++i) {
            const Message& m = live_[i];
            if (nowSec - m.shownAt < (double)ClampSeconds(m.seconds)) live_[w++] = live_[i];
        }
        live_.resize(w);
    }

    // Oldest first, which is the order they are drawn up the screen.
    const std::vector<Message>& Live() const { return live_; }
    uint32_t LastSeq() const { return lastSeq_; }
    bool     Empty()   const { return live_.empty(); }

    // A new server means a new sequence; keeping the old one would swallow its first messages.
    void Reset() { live_.clear(); lastSeq_ = 0; }

  private:
    std::vector<Message> live_;
    uint32_t             lastSeq_ = 0;
};

} // namespace servermsg
