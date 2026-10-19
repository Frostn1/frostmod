// srvpage.h - the in-game Servers page: the parts that need no GL and no game.
//
// A "Servers" button on the game's own main menu (an override ui/main.mnu beside mxbikes.exe,
// which the game reads in place of the one in ui.pkz) opens a page drawn by FrostMod in the
// game's look: its own font (ui/main.fnt, ui/data.fnt) and its own button sprites (main2.tga,
// main3.tga), read from the player's ui.pkz at runtime - nothing of PiBoSo's is shipped. The
// list is MXB App's server book (%APPDATA%\com.frost.mxbikes\servers.json: the master list
// plus our roster), the same list the app shows.
//
// This header: the .fnt and .tga readers, the servers.json reader, the address check that
// keeps a server's address from ever turning into a second command-line argument, and the
// page's layout maths. Pure C++ (plus miniz's inflate), so tests/srvpage_test.cpp covers it.
//
// The game's .fnt, as read off ui.pkz (all little-endian):
//   0     "FNT\0"
//   4     face name, zero-padded to offset 264
//   264   i32 pixel size
//   268   256 glyphs x 10 i32: present, left bearing, width, right bearing, atlas x0, x1, y0, y1, 0, 0
//         (the advance is bearing + width + bearing)
//   then  i32 n, n x (i32 char, i32 adjust)       - per-character spacing tweaks, not used here
//   then  i32 atlas width, i32 height, i32 bytes, 8 bytes, raw deflate of width x height alpha
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "miniz.h"

namespace srvpage {

// ---------------------------------------------------------------------------------------
// The game's font

struct Glyph {
    bool ok = false;
    int  lsb = 0, w = 0, rsb = 0, x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    int  advance() const { return lsb + w + rsb; }
};

struct Font {
    int                  px = 0;  // the size the atlas was made at
    int                  aw = 0, ah = 0;
    int                  cell = 0;  // the tallest glyph row: what a "font size" scales
    Glyph                g[256];
    std::vector<uint8_t> alpha;  // aw x ah, row 0 at the top
    bool                 ok() const { return aw > 0 && ah > 0 && alpha.size() == size_t(aw) * size_t(ah); }
};

inline int32_t I32(const std::vector<uint8_t>& b, size_t o) {
    if (o + 4 > b.size()) return 0;
    int32_t v;
    std::memcpy(&v, b.data() + o, 4);
    return v;
}

inline bool ReadFnt(const std::vector<uint8_t>& b, Font& f) {
    f = Font{};
    if (b.size() < 268 + 256 * 40 + 16 || std::memcmp(b.data(), "FNT\0", 4) != 0) return false;
    f.px = I32(b, 264);
    for (int c = 0; c < 256; ++c) {
        const size_t o = 268 + size_t(c) * 40;
        Glyph&       g = f.g[c];
        g.ok = I32(b, o) != 0;
        g.lsb = I32(b, o + 4), g.w = I32(b, o + 8), g.rsb = I32(b, o + 12);
        g.x0 = I32(b, o + 16), g.x1 = I32(b, o + 20), g.y0 = I32(b, o + 24), g.y1 = I32(b, o + 28);
        if (g.ok && g.y1 - g.y0 > f.cell) f.cell = g.y1 - g.y0;
    }
    size_t       o = 268 + 256 * 40;
    const int32_t n = I32(b, o);
    if (n < 0 || n > 65536) return false;
    o += 4 + size_t(n) * 8;
    const int32_t w = I32(b, o), h = I32(b, o + 4), bytes = I32(b, o + 8);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || bytes <= 8 || o + 12 + size_t(bytes) > b.size()) return false;
    f.alpha.assign(size_t(w) * size_t(h), 0);
    const size_t got = tinfl_decompress_mem_to_mem(f.alpha.data(), f.alpha.size(), b.data() + o + 20, size_t(bytes) - 8, 0);
    if (got != f.alpha.size()) {
        f.alpha.clear();
        return false;
    }
    for (Glyph& g : f.g)
        if (g.ok && (g.x0 < 0 || g.x1 > w || g.y0 < 0 || g.y1 > h || g.x1 < g.x0 || g.y1 < g.y0)) g.ok = false;
    f.aw = w, f.ah = h;
    return true;
}

/// `s` at `size` pixels tall (the menu files' fontsize times the screen height), in pixels.
inline float TextWidth(const Font& f, const std::string& s, float size) {
    if (f.cell <= 0) return 0;
    const float k = size / float(f.cell);
    float       w = 0;
    for (unsigned char c : s)
        if (f.g[c].ok) w += float(f.g[c].advance()) * k;
    return w;
}

/// `s` cut to fit `max` pixels, with "..." when it was cut.
inline std::string Fit(const Font& f, const std::string& s, float size, float max) {
    if (TextWidth(f, s, size) <= max) return s;
    std::string out = s;
    while (!out.empty() && TextWidth(f, out + "...", size) > max) out.pop_back();
    return out + "...";
}

/// One file out of a .pkz (a plain zip), e.g. "ui/main.fnt". Empty when it isn't there.
inline std::vector<uint8_t> ReadPkz(const std::string& pkz, const char* entry) {
    std::vector<uint8_t> out;
    mz_zip_archive       z;
    std::memset(&z, 0, sizeof(z));
    if (!mz_zip_reader_init_file(&z, pkz.c_str(), 0)) return out;
    size_t n = 0;
    if (void* p = mz_zip_reader_extract_file_to_heap(&z, entry, &n, 0)) {
        out.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + n);
        mz_free(p);
    }
    mz_zip_reader_end(&z);
    return out;
}

// ---------------------------------------------------------------------------------------
// The game's sprites: TGA, uncompressed or RLE, 24 or 32 bit. Out as BGRA, row 0 at the top.

struct Image {
    int                  w = 0, h = 0;
    std::vector<uint8_t> bgra;
};

inline bool ReadTga(const std::vector<uint8_t>& b, Image& im) {
    im = Image{};
    if (b.size() < 18) return false;
    const int type = b[2], w = b[12] | (b[13] << 8), h = b[14] | (b[15] << 8), bpp = b[16], desc = b[17];
    if ((type != 2 && type != 10) || (bpp != 24 && bpp != 32) || w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
    const int    bytes = bpp / 8;
    size_t       o     = 18 + b[0];
    const size_t n     = size_t(w) * size_t(h);
    std::vector<uint8_t> px(n * 4, 255);
    auto put = [&](size_t i, const uint8_t* s) {
        px[i * 4 + 0] = s[0], px[i * 4 + 1] = s[1], px[i * 4 + 2] = s[2];
        px[i * 4 + 3] = bytes == 4 ? s[3] : 255;
    };
    if (type == 2) {
        if (o + n * size_t(bytes) > b.size()) return false;
        for (size_t i = 0; i < n; ++i) put(i, &b[o + i * size_t(bytes)]);
    } else {
        size_t i = 0;
        while (i < n) {
            if (o >= b.size()) return false;
            const uint8_t head = b[o++];
            const size_t  run  = size_t(head & 0x7f) + 1;
            if (head & 0x80) {
                if (o + size_t(bytes) > b.size()) return false;
                for (size_t k = 0; k < run && i < n; ++k) put(i++, &b[o]);
                o += size_t(bytes);
            } else {
                if (o + run * size_t(bytes) > b.size()) return false;
                for (size_t k = 0; k < run && i < n; ++k, o += size_t(bytes)) put(i++, &b[o]);
            }
        }
    }
    im.w = w, im.h = h;
    im.bgra.resize(n * 4);
    const bool top = (desc & 0x20) != 0;  // else stored bottom row first
    for (int y = 0; y < h; ++y)
        std::memcpy(&im.bgra[size_t(y) * size_t(w) * 4], &px[size_t(top ? y : h - 1 - y) * size_t(w) * 4], size_t(w) * 4);
    return true;
}

// ---------------------------------------------------------------------------------------
// MXB App's server book

struct Server {
    std::string name, address, track, category, session;
    int         players = 0, max_players = 0;
    bool        passworded = false, joinable = true;
    double      last_seen = 0;
    std::vector<std::string> missing;  // what MXB App would have to install first ("track")
    std::string              region;   // the app's region for the server ("europe", "na-east", ...)
    // What the game's own join needs (offsets.h, "the browser's own JOIN"):
    std::string              layout, classes, bikes;  // classes and bikes "/"-joined; bikes "" = any
    std::string              lan;                     // the LAN address, host:port, or ""
};

/// A server's address as the game's -directconnect takes it: host:port, the host only letters,
/// digits, dots and dashes and never starting with a dash, the port 1..65535. Anything else is
/// refused (empty), since the address goes on a command line and comes from the master list.
inline std::string SafeAddress(const std::string& a) {
    const size_t colon = a.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= a.size() || colon > 253) return "";
    const std::string host = a.substr(0, colon), port = a.substr(colon + 1);
    if (host[0] == '-' || port.size() > 5) return "";
    for (char c : host)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return "";
    long p = 0;
    for (char c : port) {
        if (c < '0' || c > '9') return "";
        p = p * 10 + (c - '0');
    }
    if (p < 1 || p > 65535) return "";
    return host + ":" + port;
}

/// An IPv4 host:port as the game's join record holds an address: 01, the address as IPv6
/// (::ffff:a.b.c.d), the port big-endian - 19 bytes. False (and all zero) for anything else.
inline bool PackAddress(const std::string& a, uint8_t out[19]) {
    std::memset(out, 0, 19);
    const std::string s = SafeAddress(a);
    if (s.empty()) return false;
    const size_t      colon = s.rfind(':');
    const std::string host  = s.substr(0, colon);
    unsigned          o[4]  = {};
    int               n     = 0;
    size_t            i     = 0;
    while (n < 4) {
        const size_t      dot  = host.find('.', i);
        const std::string part = host.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
        if (part.empty() || part.size() > 3 || part.find_first_not_of("0123456789") != std::string::npos) return false;
        o[n++] = unsigned(std::atoi(part.c_str()));
        if (o[n - 1] > 255) return false;
        if (dot == std::string::npos) break;
        i = dot + 1;
    }
    if (n != 4 || host.find('.', i) != std::string::npos) return false;
    const unsigned port = unsigned(std::atoi(s.c_str() + colon + 1));
    out[0]  = 1;
    out[11] = 0xFF, out[12] = 0xFF;
    for (int k = 0; k < 4; ++k) out[13 + k] = uint8_t(o[k]);
    out[17] = uint8_t(port >> 8), out[18] = uint8_t(port & 0xFF);
    return true;
}

/// Text the game's fonts can draw: ASCII, anything else one '?' per character.
inline std::string Ascii(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) out += (c < 0x20) ? ' ' : char(c);
        else if ((c & 0xC0) != 0x80) out += '?';  // the lead byte of a UTF-8 sequence
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// JSON, just enough for what MXB App sends down the pipe: one message a line.

struct J {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool                                   b = false;
    double                                 n = 0;
    std::string                            s;
    std::vector<J>                         a;
    std::vector<std::pair<std::string, J>> o;

    const J& operator[](const char* key) const {
        static const J none;
        for (const auto& kv : o)
            if (kv.first == key) return kv.second;
        return none;
    }
    std::string str() const { return t == STR ? s : std::string(); }
    double      num(double d = 0) const { return t == NUM ? n : d; }
    bool        flag(bool d = false) const { return t == BOOL ? b : d; }
};

namespace json {
struct Reader {
    const char* p;
    const char* e;
    bool        bad = false;
    void        ws() {
        while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
    }
    bool eat(char c) {
        ws();
        if (p < e && *p == c) return ++p, true;
        return false;
    }
    std::string str() {
        std::string out;
        ws();
        if (p >= e || *p != '"') return bad = true, out;
        ++p;
        while (p < e && *p != '"') {
            char c = *p++;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p >= e) break;
            c = *p++;
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned v = 0;
                    for (int k = 0; k < 4 && p < e; ++k, ++p) {
                        const char h = *p;
                        v = v * 16 + unsigned(h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0');
                    }
                    // Only whether it was ASCII matters here: anything else is drawn '?'.
                    if (v < 0x80) out += char(v);
                    else out += "\xC2\xBF";
                    break;
                }
                default: out += c;
            }
        }
        if (p < e) ++p;
        else bad = true;
        return out;
    }
    J value(int depth = 0) {
        J v;
        ws();
        if (p >= e || depth > 32) return bad = true, v;
        const char c = *p;
        if (c == '"') {
            v.t = J::STR, v.s = str();
        } else if (c == '{') {
            ++p, v.t = J::OBJ;
            if (eat('}')) return v;
            do {
                std::string k = str();
                if (!eat(':')) return bad = true, v;
                v.o.emplace_back(std::move(k), value(depth + 1));
            } while (!bad && eat(','));
            if (!eat('}')) bad = true;
        } else if (c == '[') {
            ++p, v.t = J::ARR;
            if (eat(']')) return v;
            do v.a.push_back(value(depth + 1));
            while (!bad && eat(','));
            if (!eat(']')) bad = true;
        } else if (c == 't' || c == 'f' || c == 'n') {
            v.t = c == 'n' ? J::NUL : J::BOOL, v.b = c == 't';
            while (p < e && *p >= 'a' && *p <= 'z') ++p;
        } else {
            const char* s0 = p;
            while (p < e && (std::strchr("+-.eE", *p) || (*p >= '0' && *p <= '9'))) ++p;
            if (s0 == p) return bad = true, v;
            v.t = J::NUM, v.n = std::atof(std::string(s0, p).c_str());
        }
        return v;
    }
};
}  // namespace json

/// One JSON value out of `text`; NUL type (and `ok` false) when it isn't well formed.
inline J ParseJson(const std::string& text, bool* ok = nullptr) {
    json::Reader r{text.data(), text.data() + text.size()};
    J            v = r.value();
    if (ok) *ok = !r.bad;
    return r.bad ? J{} : v;
}

/// JSON's text for `s`, quotes included.
inline std::string Quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += c;
        else if ((unsigned char)c < 0x20) out += ' ';
        else out += c;
    }
    return out + "\"";
}

inline Server ServerOf(const J& j) {
    Server s;
    s.name        = Ascii(j["name"].str());
    s.address     = SafeAddress(j["address"].str());
    s.track       = Ascii(j["track"].str());
    s.session     = Ascii(j["session"].str());
    const J& cats = j["categories"];
    if (cats.t == J::ARR && !cats.a.empty()) s.category = Ascii(cats.a[0].str());
    auto joined = [](const J& arr) {
        std::string out;
        for (const J& v : arr.a)
            if (v.t == J::STR) out += (out.empty() ? "" : "/") + v.s;
        return out;
    };
    s.classes = Ascii(joined(cats));
    s.bikes   = Ascii(joined(j["bikes"]));
    s.layout  = Ascii(j["trackLayout"].str());
    s.lan     = SafeAddress(j["lanAddress"].str());
    s.players     = int(j["players"].num());
    s.max_players = int(j["maxPlayers"].num());
    s.passworded  = j["passworded"].flag();
    s.joinable    = j["joinable"].flag(true);
    s.last_seen   = j["lastSeen"].num();
    s.region      = j["region"].str();
    const J& miss = j["missing"];
    for (const J& m : miss.a)
        if (m.t == J::STR) s.missing.push_back(Ascii(m.s));
    return s;
}

/// A JSON array of servers as MXB App writes them (servers.json, or the pipe's "servers"). A
/// server without a usable address is left out.
inline std::vector<Server> ReadServers(const std::string& text) {
    std::vector<Server> out;
    const J             j = ParseJson(text);
    for (const J& e : j.a) {
        if (e.t != J::OBJ) continue;
        Server s = ServerOf(e);
        if (!s.address.empty()) out.push_back(std::move(s));
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// Friends, as MXB App sends them

struct Friend {
    std::string name;
    bool        online = false;
    Server      on;      // the server they're on; empty address when none
    bool        has_server = false;
};

inline std::vector<Friend> ReadFriends(const J& arr) {
    std::vector<Friend> out;
    for (const J& e : arr.a) {
        if (e.t != J::OBJ) continue;
        Friend f;
        f.name   = Ascii(e["name"].str());
        f.online = e["online"].flag();
        if (e["server"].t == J::OBJ) {
            f.on         = ServerOf(e["server"]);
            f.has_server = !f.on.address.empty();
        }
        out.push_back(std::move(f));
    }
    return out;
}

/// The order the page lists them in: joinable first, then the busiest, then by name.
inline bool Before(const Server& a, const Server& b) {
    if (a.joinable != b.joinable) return a.joinable;
    if (a.players != b.players) return a.players > b.players;
    return a.name < b.name;
}

// ---------------------------------------------------------------------------------------
// Colours. Nothing on the page is a fixed colour: everything comes from a theme. "game" is the
// game's own look, the colours its pages are drawn in (sampled from ui.pkz: dialog1580x700t,
// button1..3, tabh1..2, back1..2, done1..2, and the server list's colours in multijoin.mnu);
// "dark" is a dark one for MXB App's dark mode. frostmod_page.ini picks one and can change any
// colour: theme=dark, then e.g. panel=#202228F0.

struct Rgba {
    float r = 0, g = 0, b = 0, a = 1;
};

inline Rgba Hex(uint32_t rgba) {
    return {float((rgba >> 24) & 0xff) / 255.0f, float((rgba >> 16) & 0xff) / 255.0f, float((rgba >> 8) & 0xff) / 255.0f,
            float(rgba & 0xff) / 255.0f};
}

struct Theme {
    Rgba title_bg, title_text;           // the bar across the top, and the page's name in it
    Rgba panel, list_bg, header_bg;      // the big panel, the list inside it, its column headers
    Rgba header_text, row_text, row_hover, row_pick, row_pick_bg, muted, needs, ready, busy;
    Rgba button, button_hover, button_text, button_off;
    Rgba tab, tab_on, tab_text, tab_text_on;
    Rgba bar_bg, bar_text, back, back_hover, go, go_hover, note_warn;
};

inline Theme GameTheme() {
    Theme t;
    t.title_bg = Hex(0x0000007F), t.title_text = Hex(0xF0F0F0FF);
    t.panel = Hex(0xBEB4AAF0), t.list_bg = Hex(0xDCDCDCFF), t.header_bg = Hex(0xB4B4B4FF);
    t.header_text = Hex(0x000000FF), t.row_text = Hex(0x505050FF), t.row_hover = Hex(0x283CF0FF);
    t.row_pick = Hex(0x000000FF), t.row_pick_bg = Hex(0x00000014), t.muted = Hex(0x8C8C8CFF);
    t.needs = Hex(0xB4501EFF), t.ready = Hex(0x237823FF), t.busy = Hex(0x000000FF);
    t.button = Hex(0x3C3C3CFF), t.button_hover = Hex(0x646464FF), t.button_text = Hex(0xF0F0F0FF), t.button_off = Hex(0x8C8C8CFF);
    t.tab = Hex(0x948C84F0), t.tab_on = Hex(0xBEB4AAF0), t.tab_text = Hex(0xF0F0F0FF), t.tab_text_on = Hex(0x000000FF);
    t.bar_bg = Hex(0x0000007F), t.bar_text = Hex(0xF0F0F0FF);
    t.back = Hex(0x8C2323FF), t.back_hover = Hex(0xB42D2DFF), t.go = Hex(0x238C23FF), t.go_hover = Hex(0x2DB42DFF);
    t.note_warn = Hex(0xB4501EFF);
    return t;
}

inline Theme DarkTheme() {
    Theme t   = GameTheme();
    t.panel   = Hex(0x1A1C22F2), t.list_bg = Hex(0x22252DFF), t.header_bg = Hex(0x2C303AFF);
    t.header_text = Hex(0xB8BCC8FF), t.row_text = Hex(0xDCDEE4FF), t.row_hover = Hex(0xFF9A3CFF);
    t.row_pick = Hex(0xFFFFFFFF), t.row_pick_bg = Hex(0xFF7A1A40), t.muted = Hex(0x80869AFF);
    t.needs = Hex(0xFFB347FF), t.ready = Hex(0x6FD38AFF), t.busy = Hex(0xFFFFFFFF);
    t.button = Hex(0x343844FF), t.button_hover = Hex(0x484D5CFF), t.button_off = Hex(0x6A6F7EFF);
    t.tab = Hex(0x262930F0), t.tab_on = Hex(0x1A1C22F2), t.tab_text = Hex(0x9AA0B0FF), t.tab_text_on = Hex(0xFFFFFFFF);
    t.back = Hex(0x4A2A2AFF), t.back_hover = Hex(0x6E3434FF), t.go = Hex(0xE8661AFF), t.go_hover = Hex(0xFF7A1AFF);
    t.note_warn = Hex(0xFFB347FF);
    return t;
}

/// The theme frostmod_page.ini describes: "theme=game|dark" picks the base, and any "name=#RRGGBB"
/// or "#RRGGBBAA" line changes one colour. Unknown lines are ignored.
inline Theme ReadTheme(const std::string& ini) {
    Theme t = GameTheme();
    struct Slot {
        const char* name;
        Rgba Theme::*c;
    };
    static const Slot slots[] = {
        {"title_bg", &Theme::title_bg},       {"title_text", &Theme::title_text},   {"panel", &Theme::panel},
        {"list_bg", &Theme::list_bg},         {"header_bg", &Theme::header_bg},     {"header_text", &Theme::header_text},
        {"row_text", &Theme::row_text},       {"row_hover", &Theme::row_hover},     {"row_pick", &Theme::row_pick},
        {"row_pick_bg", &Theme::row_pick_bg}, {"muted", &Theme::muted},             {"needs", &Theme::needs},
        {"ready", &Theme::ready},             {"busy", &Theme::busy},               {"button", &Theme::button},
        {"button_hover", &Theme::button_hover}, {"button_text", &Theme::button_text}, {"button_off", &Theme::button_off},
        {"tab", &Theme::tab},                 {"tab_on", &Theme::tab_on},           {"tab_text", &Theme::tab_text},
        {"tab_text_on", &Theme::tab_text_on}, {"bar_bg", &Theme::bar_bg},           {"bar_text", &Theme::bar_text},
        {"back", &Theme::back},               {"back_hover", &Theme::back_hover},   {"go", &Theme::go},
        {"go_hover", &Theme::go_hover},       {"note_warn", &Theme::note_warn},
    };
    std::vector<std::pair<std::string, std::string>> kv;
    size_t                                           i = 0;
    while (i < ini.size()) {
        size_t e = ini.find('\n', i);
        if (e == std::string::npos) e = ini.size();
        std::string line = ini.substr(i, e - i);
        i                = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#' || line[0] == ';') continue;
        kv.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    for (const auto& p : kv)
        if (p.first == "theme" && p.second == "dark") t = DarkTheme();
    for (const auto& p : kv) {
        const std::string& v = p.second;
        if (v.size() != 7 && v.size() != 9) continue;
        if (v[0] != '#') continue;
        uint32_t x = 0;
        bool     ok = true;
        for (size_t k = 1; k < v.size(); ++k) {
            const char c = v[k];
            const int  d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) ok = false;
            x = x * 16 + uint32_t(d < 0 ? 0 : d);
        }
        if (!ok) continue;
        if (v.size() == 7) x = (x << 8) | 0xFF;
        for (const Slot& s : slots)
            if (p.first == s.name) t.*(s.c) = Hex(x);
    }
    return t;
}

// ---------------------------------------------------------------------------------------
// Layout, in fractions of the window (0,0 top left), as the game's .mnu files place things.

/// The game's own Race button (ID_MULTIPLAYER in main.mnu): it opens this page instead of the
/// game's server browser, which stays one tab away (Classic).
constexpr float kButton[4] = {0.0f, 0.344444f, 0.2125f, 0.388889f};

// The page is laid out as the game's Server Browser (multijoin.mnu) is: a title bar, one big
// panel with a list in it, tabs under the panel, and Back / Join along the bottom.
constexpr float kTitle[4]   = {0.0f, 0.0f, 1.0f, 0.088889f};
constexpr float kPanel[4]   = {0.00625f, 0.133333f, 0.99375f, 0.911111f};
constexpr float kList[4]    = {0.0125f, 0.188889f, 0.9875f, 0.855556f};
constexpr float kHeaderH    = 0.026f;  // the column headers' band at the top of the list
constexpr float kRowH       = 0.022222f;
constexpr int   kRows       = int((0.855556f - 0.188889f - 0.026f) / 0.022222f);  // 28
constexpr float kListY0     = 0.188889f + 0.026f;  // the first row's top
constexpr float kTopBtn[4]  = {0.0125f, 0.144444f, 0.1375f, 0.177778f};  // like Host World
constexpr float kLowBtn[4]  = {0.0125f, 0.866667f, 0.1375f, 0.9f};       // like Refresh
constexpr float kTabY[2]    = {0.911111f, 0.944444f};
constexpr float kTabW       = 0.125f;
constexpr float kBar[4]     = {0.0f, 0.966667f, 1.0f, 1.0f};
constexpr float kBack[4]    = {0.0f, 0.966667f, 0.1f, 1.0f};
constexpr float kGo[4]      = {0.84375f, 0.966667f, 1.0f, 1.0f};         // Join / Install & Join
constexpr float kTextSize   = 0.022222f;  // the game's 0.022222 for buttons, 0.02 for list rows
constexpr float kRowText    = 0.020f;

/// The row under the pointer at (x, y), counting from `top`, or -1.
inline int RowAt(float x, float y, int top, int count) {
    if (x < kList[0] || x > kList[2] - 0.012f || y < kListY0) return -1;
    const int r = int((y - kListY0) / kRowH);
    if (r < 0 || r >= kRows || top + r >= count) return -1;
    return top + r;
}

/// The tab `i`'s rectangle along the bottom of the panel.
inline void TabRect(int i, float r[4]) {
    r[0] = kPanel[0] + float(i) * kTabW, r[1] = kTabY[0], r[2] = r[0] + kTabW, r[3] = kTabY[1];
}

// The Servers tab's filters, in the game's own row of them above the list.
constexpr float kRegionBtn[4] = {0.0125f, 0.144444f, 0.1375f, 0.177778f};
constexpr float kSearchBox[4] = {0.2750f, 0.155556f, 0.4500f, 0.177778f};  // the game's Filter Name box
constexpr float kCheck[3][4]  = {{0.4750f, 0.155556f, 0.6250f, 0.177778f},
                                 {0.6375f, 0.155556f, 0.7750f, 0.177778f},
                                 {0.7875f, 0.155556f, 0.9875f, 0.177778f}};

/// The regions the Region button steps through, with their labels: the app's own list.
constexpr const char* kRegions[][2] = {{"", "All"},          {"europe", "Europe"}, {"na-east", "NA East"}, {"na-west", "NA West"},
                                       {"na", "N. America"}, {"oceania", "Oceania"}, {"south-america", "S. America"},
                                       {"asia", "Asia"},     {"other", "Other"}};
constexpr int kRegionCount = int(sizeof(kRegions) / sizeof(kRegions[0]));

struct Filters {
    std::string search;      // matched against name and track, any case
    int         region = 0;  // index into kRegions; 0 = all
    bool        hide_empty = false, hide_full = false, ready_only = false;
};

inline std::string Lower(std::string s) {
    for (char& c : s) c = char(tolower((unsigned char)c));
    return s;
}

/// Whether server `s` shows under `f`.
inline bool Passes(const Server& s, const Filters& f) {
    if (f.hide_empty && s.players <= 0) return false;
    if (f.hide_full && s.max_players > 0 && s.players >= s.max_players) return false;
    if (f.ready_only && !s.missing.empty()) return false;
    if (f.region > 0 && f.region < kRegionCount && s.region != kRegions[f.region][0]) return false;
    if (!f.search.empty()) {
        const std::string q = Lower(f.search);
        if (Lower(s.name).find(q) == std::string::npos && Lower(s.track).find(q) == std::string::npos) return false;
    }
    return true;
}

inline bool Inside(const float r[4], float x, float y) { return x >= r[0] && x < r[2] && y >= r[1] && y < r[3]; }

}  // namespace srvpage
