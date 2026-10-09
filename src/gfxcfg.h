// Live bike gfx.cfg: put a bike's grip, lever, chain and rider-offset values into the
// running game without a restart. docs/GFX_CFG.md has the keys, their units and frames.
//
// How the game holds them (MX Bikes beta21e, RVAs, static analysis):
//   0x45020  builds a vehicle's gfx block (vehicle +0x274, memset 0x4C74): formats
//            "%sbikes\%s\gfx.cfg" from the bike entry (+0x4C0 root, +0x00 folder), opens it
//            (bus 0x2C) and calls the part parser twice: 0x419C0(block, block, cfg, "") at
//            0x4540D and 0x419C0(block, block + 0x3C4, cfg, "cockpit/") at 0x45524, then sets
//            block +0x3BC = 1 (cockpit parsed).
//   0x419C0  reads each key into the part descriptor it is given (bus 0x2F float, 0x2E int,
//            0x2D string; names go through the node lookup 0x78 and land as handles).
//   0x523E0  runs every frame from the vehicle gfx update 0x59300 (called by the race,
//            replay and garage renderers) and reads the same descriptor fields again: the
//            grip pos/dir/type feed the hand IK (bus 0x8A at 0x589AC / 0x58BB0), the lever
//            axis and maxrot scale the lever rotation (0x534A4..0x538C1), the chain ratio
//            (0x53CFF), the shock slide (0x524D8..0x524F7) and the rider offset (0x54419).
// So a float written into the descriptor is drawn on the next frame. Nothing is allocated,
// no object is rebuilt, and the game's own parser would have written the same bytes.
//
// Not live (the parse turns them into handles or objects once): every file / name /
// link_obj / texture key, the cockpit models, tyres, plate, and exhaust (a different record,
// vehicle +0x4EEC, parsed by 0x4FB60). Those are re-read when the game builds the vehicle
// again (vehicle create 0x5CAE0, rider re-load 0x5E570, bike apply 0xE4550 all call 0x45020).
//
// Pure: no Win32, no game. frostmod.cpp supplies the memory; tests/gfxcfg_test.cpp runs the
// parser and proves every offset against the instruction bytes it was read from.
#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace gfxcfg {

// ---- layout -------------------------------------------------------------------------
constexpr int kVehGfx        = 0x274;   // vehicle record -> gfx block (0x5D7D4 lea rcx,[r15+0x274])
constexpr int kCockpitDesc   = 0x3C4;   // block -> cockpit part descriptor (0x45425)
constexpr int kCockpitParsed = 0x3BC;   // block: 1 once the cockpit descriptor is filled (0x45529)
constexpr int kChassisModel  = 0x00;    // descriptor: chassis model handle (lever node lookups)
constexpr int kSteerModel    = 0x50;    // descriptor: steer model handle (grip / lever lookups)

enum class Kind { Float, Int, Axis };

struct Field {
    const char* key;     // as written in gfx.cfg, without the "cockpit/" prefix
    int         off;     // into the part descriptor
    Kind        kind;
    bool        prefixed;   // read as "%s<key>" (cockpit/ for the cockpit); false = same key for both
};

// Every field the frame code reads back. Offsets from 0x419C0; see kSites for the proof.
constexpr Field kFields[] = {
    {"steer/throttlegrip/axis",                 0x084, Kind::Axis,  true},
    {"steer/throttlegrip/maxrot",               0x088, Kind::Float, true},
    {"steer/brakelever/axis",                   0x090, Kind::Axis,  true},
    {"steer/brakelever/maxrot",                 0x094, Kind::Float, true},
    {"chassis/rearbrakepedal/axis",             0x09C, Kind::Axis,  true},
    {"chassis/rearbrakepedal/maxrot",           0x0A0, Kind::Float, true},
    {"chassis/rearbrakepedal/mastercylinder/x", 0x0AC, Kind::Float, true},
    {"chassis/rearbrakepedal/mastercylinder/y", 0x0B0, Kind::Float, true},
    {"chassis/rearbrakepedal/mastercylinder/z", 0x0B4, Kind::Float, true},
    {"steer/clutchlever/axis",                  0x0BC, Kind::Axis,  true},
    {"steer/clutchlever/maxrot",                0x0C0, Kind::Float, true},
    {"chassis/shifter/axis",                    0x0C8, Kind::Axis,  true},
    {"chassis/shifter/maxrot",                  0x0CC, Kind::Float, true},
    {"chassis/shock/link/x",                    0x0DC, Kind::Float, true},
    {"chassis/shock/link/y",                    0x0E0, Kind::Float, true},
    {"chassis/shock/link/z",                    0x0E4, Kind::Float, true},
    {"chassis/shock/slideaxis",                 0x0F0, Kind::Axis,  true},
    {"chassis/shock/reflength",                 0x0F4, Kind::Float, true},
    {"chassis/shock/slidescale",                0x0F8, Kind::Float, true},
    {"chassis/chain/engine/x",                  0x108, Kind::Float, true},
    {"chassis/chain/engine/y",                  0x10C, Kind::Float, true},
    {"chassis/chain/engine/z",                  0x110, Kind::Float, true},
    {"chassis/chain/ratio",                     0x1CC, Kind::Float, true},
    {"rider/xform/x",                           0x1E8, Kind::Float, false},
    {"rider/xform/y",                           0x1EC, Kind::Float, false},
    {"rider/xform/z",                           0x1F0, Kind::Float, false},
    {"steer/leftgrip/type",                     0x37C, Kind::Int,   true},
    {"steer/leftgrip/pos/x",                    0x384, Kind::Float, true},
    {"steer/leftgrip/pos/y",                    0x388, Kind::Float, true},
    {"steer/leftgrip/pos/z",                    0x38C, Kind::Float, true},
    {"steer/leftgrip/dir/x",                    0x390, Kind::Float, true},
    {"steer/leftgrip/dir/y",                    0x394, Kind::Float, true},
    {"steer/leftgrip/dir/z",                    0x398, Kind::Float, true},
    {"steer/rightgrip/type",                    0x39C, Kind::Int,   true},
    {"steer/rightgrip/pos/x",                   0x3A4, Kind::Float, true},
    {"steer/rightgrip/pos/y",                   0x3A8, Kind::Float, true},
    {"steer/rightgrip/pos/z",                   0x3AC, Kind::Float, true},
    {"steer/rightgrip/dir/x",                   0x3B0, Kind::Float, true},
    {"steer/rightgrip/dir/y",                   0x3B4, Kind::Float, true},
    {"steer/rightgrip/dir/z",                   0x3B8, Kind::Float, true},
};
constexpr int kFieldCount = (int)(sizeof(kFields) / sizeof(kFields[0]));

// ---- proof: the instructions the offsets were read from ------------------------------
// FrostMod compares these against the running exe before it writes anything, so a game
// update that moves the layout turns the feature off instead of writing into the wrong
// place. The tests check every field's offset is the displacement encoded here.
struct Site {
    uintptr_t     rva;
    unsigned char bytes[10];
    int           len;
    int           dispAt;   // index of the disp32 in `bytes`
    int           disp;     // what it must decode to
};
constexpr Site kSites[] = {
    // 0x419C0, the part parser: lea r8,[rdi+off] before each bus 0x2F/0x2E read ...
    {0x427E8, {0x4C,0x8D,0x87,0x88,0x00,0x00,0x00}, 7, 3, 0x088},
    {0x4297C, {0x4C,0x8D,0x87,0x94,0x00,0x00,0x00}, 7, 3, 0x094},
    {0x42B0F, {0x4C,0x8D,0x87,0xA0,0x00,0x00,0x00}, 7, 3, 0x0A0},
    {0x42D56, {0x4C,0x8D,0x87,0xAC,0x00,0x00,0x00}, 7, 3, 0x0AC},
    {0x42D89, {0x4C,0x8D,0x87,0xB0,0x00,0x00,0x00}, 7, 3, 0x0B0},
    {0x42DBC, {0x4C,0x8D,0x87,0xB4,0x00,0x00,0x00}, 7, 3, 0x0B4},
    {0x42FC6, {0x4C,0x8D,0x87,0xC0,0x00,0x00,0x00}, 7, 3, 0x0C0},
    {0x43159, {0x4C,0x8D,0x87,0xCC,0x00,0x00,0x00}, 7, 3, 0x0CC},
    {0x434EE, {0x4C,0x8D,0x87,0xDC,0x00,0x00,0x00}, 7, 3, 0x0DC},
    {0x43521, {0x4C,0x8D,0x87,0xE0,0x00,0x00,0x00}, 7, 3, 0x0E0},
    {0x43554, {0x4C,0x8D,0x87,0xE4,0x00,0x00,0x00}, 7, 3, 0x0E4},
    {0x43755, {0x4C,0x8D,0x87,0xF4,0x00,0x00,0x00}, 7, 3, 0x0F4},
    {0x43722, {0x4C,0x8D,0x87,0xF8,0x00,0x00,0x00}, 7, 3, 0x0F8},
    {0x43B3B, {0x4C,0x8D,0x87,0x08,0x01,0x00,0x00}, 7, 3, 0x108},
    {0x43B6E, {0x4C,0x8D,0x87,0x0C,0x01,0x00,0x00}, 7, 3, 0x10C},
    {0x43BA1, {0x4C,0x8D,0x87,0x10,0x01,0x00,0x00}, 7, 3, 0x110},
    {0x43D9D, {0x4C,0x8D,0x87,0xCC,0x01,0x00,0x00}, 7, 3, 0x1CC},
    {0x448D4, {0x4C,0x8D,0x87,0xE8,0x01,0x00,0x00}, 7, 3, 0x1E8},
    {0x448EF, {0x4C,0x8D,0x87,0xEC,0x01,0x00,0x00}, 7, 3, 0x1EC},
    {0x4490A, {0x4C,0x8D,0x87,0xF0,0x01,0x00,0x00}, 7, 3, 0x1F0},
    {0x4456D, {0x4C,0x8D,0x87,0x7C,0x03,0x00,0x00}, 7, 3, 0x37C},
    {0x445FA, {0x4C,0x8D,0x87,0x84,0x03,0x00,0x00}, 7, 3, 0x384},
    {0x4462D, {0x4C,0x8D,0x87,0x88,0x03,0x00,0x00}, 7, 3, 0x388},
    {0x44660, {0x4C,0x8D,0x87,0x8C,0x03,0x00,0x00}, 7, 3, 0x38C},
    {0x44693, {0x4C,0x8D,0x87,0x90,0x03,0x00,0x00}, 7, 3, 0x390},
    {0x446C6, {0x4C,0x8D,0x87,0x94,0x03,0x00,0x00}, 7, 3, 0x394},
    {0x446F9, {0x4C,0x8D,0x87,0x98,0x03,0x00,0x00}, 7, 3, 0x398},
    {0x4472C, {0x4C,0x8D,0x87,0x9C,0x03,0x00,0x00}, 7, 3, 0x39C},
    {0x447B9, {0x4C,0x8D,0x87,0xA4,0x03,0x00,0x00}, 7, 3, 0x3A4},
    {0x447EC, {0x4C,0x8D,0x87,0xA8,0x03,0x00,0x00}, 7, 3, 0x3A8},
    {0x4481F, {0x4C,0x8D,0x87,0xAC,0x03,0x00,0x00}, 7, 3, 0x3AC},
    {0x44852, {0x4C,0x8D,0x87,0xB0,0x03,0x00,0x00}, 7, 3, 0x3B0},
    {0x44885, {0x4C,0x8D,0x87,0xB4,0x03,0x00,0x00}, 7, 3, 0x3B4},
    {0x448B8, {0x4C,0x8D,0x87,0xB8,0x03,0x00,0x00}, 7, 3, 0x3B8},
    // ... mov dword [rdi+off],4 ("x-") in each axis parse ...
    {0x42737, {0xC7,0x87,0x84,0x00,0x00,0x00,0x04,0x00,0x00,0x00}, 10, 2, 0x084},
    {0x428CB, {0xC7,0x87,0x90,0x00,0x00,0x00,0x04,0x00,0x00,0x00}, 10, 2, 0x090},
    {0x42A5E, {0xC7,0x87,0x9C,0x00,0x00,0x00,0x04,0x00,0x00,0x00}, 10, 2, 0x09C},
    {0x42F15, {0xC7,0x87,0xBC,0x00,0x00,0x00,0x04,0x00,0x00,0x00}, 10, 2, 0x0BC},
    {0x430A8, {0xC7,0x87,0xC8,0x00,0x00,0x00,0x04,0x00,0x00,0x00}, 10, 2, 0x0C8},
    {0x4366F, {0xC7,0x87,0xF0,0x00,0x00,0x00,0x04,0x00,0x00,0x00}, 10, 2, 0x0F0},
    // 0x45020: the cockpit descriptor and its flag; vehicle create's gfx block.
    {0x45425, {0x48,0x8D,0x97,0xC4,0x03,0x00,0x00}, 7, 3, kCockpitDesc},
    {0x45529, {0xC7,0x87,0xBC,0x03,0x00,0x00,0x01,0x00,0x00,0x00}, 10, 2, kCockpitParsed},
    {0x5D7D4, {0x49,0x8D,0x8F,0x74,0x02,0x00,0x00}, 7, 3, kVehGfx},
    // 0x523E0, the per-frame reader: left grip pos, brake maxrot, rider offset.
    {0x58915, {0x4C,0x8D,0x87,0x84,0x03,0x00,0x00}, 7, 3, 0x384},
    {0x53587, {0xF3,0x0F,0x59,0x87,0x94,0x00,0x00,0x00}, 8, 4, 0x094},
    {0x54429, {0xF3,0x0F,0x10,0x8F,0xE8,0x01,0x00,0x00}, 8, 4, 0x1E8},
};
constexpr int kSiteCount = (int)(sizeof(kSites) / sizeof(kSites[0]));

inline int SiteDisp(const Site& s) {
    uint32_t d = 0;
    for (int i = 0; i < 4; ++i) d |= (uint32_t)s.bytes[s.dispAt + i] << (8 * i);
    return (int)d;
}

// ---- the cfg text ---------------------------------------------------------------------
// PiBoSo's format: `name { ... }` blocks (the brace may sit on the next line), `key = value`
// lines, ';' comments. Flattened to "a/b/c" -> value, keys lower-cased.
using Flat = std::map<std::string, std::string>;

inline std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
inline std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline Flat Parse(const std::string& text) {
    Flat out;
    std::vector<std::string> stack;
    std::string pending;   // a bare word waiting for its '{'
    auto path = [&](const std::string& leaf) {
        std::string p;
        for (const auto& s : stack) { p += s; p += '/'; }
        return p + leaf;
    };
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(i, e - i);
        i = e + 1;
        if (size_t c = line.find(';'); c != std::string::npos) line.resize(c);
        if (size_t c = line.find("//"); c != std::string::npos) line.resize(c);
        // A line can hold several tokens: "name {", "}", "{ x = 1 }" in a one-liner.
        size_t p = 0;
        while (p < line.size()) {
            const char ch = line[p];
            if (std::isspace((unsigned char)ch)) { ++p; continue; }
            if (ch == '{') {
                stack.push_back(Lower(Trim(pending)));
                pending.clear();
                ++p;
                continue;
            }
            if (ch == '}') {
                if (!stack.empty()) stack.pop_back();
                pending.clear();
                ++p;
                continue;
            }
            // A word, maybe "key = value".
            size_t q = p;
            while (q < line.size() && line[q] != '{' && line[q] != '}' && line[q] != '=' &&
                   !std::isspace((unsigned char)line[q])) ++q;
            size_t eq = q;   // "key = v": the '=' may sit after spaces
            while (eq < line.size() && std::isspace((unsigned char)line[eq])) ++eq;
            if (eq < line.size() && line[eq] == '=') {
                q = eq;
                const std::string key = Lower(Trim(line.substr(p, q - p)));
                size_t v = q + 1;
                while (v < line.size() && std::isspace((unsigned char)line[v])) ++v;
                size_t w = v;   // the value is one token, so "type = 1 pos {" leaves "pos"
                while (w < line.size() && !std::isspace((unsigned char)line[w]) &&
                       line[w] != '{' && line[w] != '}') ++w;
                if (!key.empty()) out[path(key)] = line.substr(v, w - v);
                pending.clear();
                p = w;
            } else {
                pending = line.substr(p, q - p);
                p = q;
            }
        }
    }
    return out;
}

// "x"/"y"/"z", "-" for the other way: the codes 0x419C0 stores (x-=4: bit 2 is the sign).
inline bool AxisCode(const std::string& v, int& code) {
    static const struct { const char* s; int c; } k[] = {
        {"x", 0}, {"y", 1}, {"z", 2}, {"x-", 4}, {"y-", 5}, {"z-", 6}};
    for (const auto& a : k)
        if (v == a.s) { code = a.c; return true; }
    return false;
}

inline bool ParseFloat(const std::string& v, float& f) {
    if (v.empty()) return false;
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (!end || *end != 0 || !std::isfinite(d) || std::fabs(d) > 1000.0) return false;
    f = (float)d;
    return true;
}
inline bool ParseInt(const std::string& v, int& n) {
    if (v.empty()) return false;
    char* end = nullptr;
    const long l = std::strtol(v.c_str(), &end, 10);
    if (!end || *end != 0) return false;
    n = (int)l;
    return true;
}

struct Write {
    const Field* field;
    std::string  key;    // the full key it came from
    int          off;    // into the part descriptor
    uint32_t     bits;   // the 4 bytes to store
};

// The writes for one part descriptor. `prefix` is "" for the bike and "cockpit/" for the
// cockpit. Only keys the file has are written: a key the file leaves out keeps what the
// game parsed (the game zeroes the block first, but a live descriptor is not ours to reset).
// Values the game could not have parsed into a sane number are skipped and listed in `bad`.
inline std::vector<Write> Plan(const Flat& cfg, const std::string& prefix,
                               std::vector<std::string>* bad = nullptr) {
    std::vector<Write> out;
    for (const Field& f : kFields) {
        const std::string key = (f.prefixed ? prefix : std::string()) + f.key;
        auto it = cfg.find(key);
        if (it == cfg.end()) continue;
        Write w{&f, key, f.off, 0};
        bool ok = false;
        switch (f.kind) {
        case Kind::Float: {
            float v;
            if ((ok = ParseFloat(it->second, v))) std::memcpy(&w.bits, &v, 4);
            break;
        }
        case Kind::Int: {
            int v;
            // Grip type: stock bikes all use 1; anything outside 0..3 is a typo, not a mode.
            if ((ok = ParseInt(it->second, v) && v >= 0 && v <= 3)) w.bits = (uint32_t)v;
            break;
        }
        case Kind::Axis: {
            int v;
            if ((ok = AxisCode(Lower(it->second), v))) w.bits = (uint32_t)v;
            break;
        }
        }
        if (ok) out.push_back(w);
        else if (bad) bad->push_back(key);
    }
    return out;
}

// Keys that changed between two reads of the file and that a live write cannot carry
// (names, files, textures, exhaust, ...): what to tell the modder needs a rejoin.
inline bool IsLiveKey(const std::string& key) {
    const std::string k = key.rfind("cockpit/", 0) == 0 ? key.substr(8) : key;
    for (const Field& f : kFields)
        if (k == f.key) return true;
    return false;
}
inline std::vector<std::string> NotLive(const Flat& before, const Flat& after) {
    std::vector<std::string> out;
    for (const auto& [k, v] : after) {
        auto it = before.find(k);
        if ((it == before.end() || it->second != v) && !IsLiveKey(k)) out.push_back(k);
    }
    for (const auto& [k, v] : before)
        if (after.find(k) == after.end() && !IsLiveKey(k)) out.push_back(k);
    return out;
}

}   // namespace gfxcfg
