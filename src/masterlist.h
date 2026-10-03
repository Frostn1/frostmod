// The server list the game itself received from the master, handed to MXB App.
//
// MXB App cannot ask the master while the game runs: the master login spends the Steam
// account the game is holding. So during a session the app only knew the servers it already
// remembered, and a server that appeared after that never showed up in the app while the game
// was open - even though the game's own browser listed it. This is the missing half: the
// browser's server rows already pass through the filter hook, and each row carries the
// server's address next to its name. We write the (address, name) pairs of the last browser
// pass to `frostmod_masterlist.txt` beside the log, and the app folds them into its book.
//
// Read-only on the game side: nothing here writes to game memory. The app side only ever
// reads the file.
//
// File format (UTF-8, LF):
//   line 1:  "frostmod-masterlist 1 <unix milliseconds>"
//   then:    "<ipv4>:<port>\t<name>"   one per server, public IPv4 only
//
// Row layout (RE'd against beta21e, confirmed on a live list): in an SB_Entry the 16-byte
// address (an IPv4 server is stored v4-mapped, ::ffff:a.b.c.d) sits at +0x61 and its port,
// big-endian, at +0x71; the name starts at +0x86.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace frostmod {
namespace masterlist {

constexpr size_t kEntryAddr = 0x61;  // 16 bytes
constexpr size_t kEntryPort = 0x71;  // 2 bytes, big-endian
constexpr size_t kEntryNeeds = 0x73; // bytes of the entry the decoder reads
constexpr size_t kMaxRows = 1000;
constexpr size_t kMaxName = 63;
constexpr const char kFileName[] = "frostmod_masterlist.txt";

struct Row {
    std::string address; // "a.b.c.d:port"
    std::string name;
    bool operator==(const Row& o) const { return address == o.address && name == o.name; }
};

// An address the app can ask directly: public IPv4. IPv6 and LAN addresses are not offered.
inline bool PublicV4(const uint8_t ip[4]) {
    if (ip[0] == 0 || ip[0] == 127 || ip[0] >= 224) return false;
    if (ip[0] == 10) return false;
    if (ip[0] == 169 && ip[1] == 254) return false;
    if (ip[0] == 192 && ip[1] == 168) return false;
    if (ip[0] == 172 && ip[1] >= 16 && ip[1] < 32) return false;
    return true;
}

// Decode the address and port of one browser row. False when the row has no address we can use.
inline bool DecodeAddress(const uint8_t* entry, size_t len, std::string& out) {
    if (!entry || len < kEntryNeeds) return false;
    const uint8_t* a = entry + kEntryAddr;
    for (int i = 0; i < 10; ++i)
        if (a[i] != 0) return false;
    if (a[10] != 0xFF || a[11] != 0xFF) return false;
    if (!PublicV4(a + 12)) return false;
    const unsigned port = (unsigned)entry[kEntryPort] << 8 | entry[kEntryPort + 1];
    if (port == 0) return false;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", a[12], a[13], a[14], a[15], port);
    out = buf;
    return true;
}

// A name that fits on one line of the file: no tab or line breaks, trimmed, capped.
inline std::string CleanName(const char* name) {
    std::string s;
    for (const char* p = name; p && *p && s.size() < kMaxName; ++p) {
        const unsigned char c = (unsigned char)*p;
        s.push_back(c < 0x20 || c == 0x7F ? ' ' : (char)c);
    }
    size_t b = s.find_first_not_of(' ');
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(' ');
    return s.substr(b, e - b + 1);
}

// Add a row to a pass. Duplicate addresses collapse; nameless rows and a full pass are skipped.
inline bool AddRow(std::vector<Row>& pass, const uint8_t* entry, size_t len, const char* name) {
    std::string address;
    if (!DecodeAddress(entry, len, address)) return false;
    std::string clean = CleanName(name);
    if (clean.empty() || pass.size() >= kMaxRows) return false;
    for (const Row& r : pass)
        if (r.address == address) return false;
    pass.push_back(Row{address, clean});
    return true;
}

inline std::string Serialize(const std::vector<Row>& rows, uint64_t unixMs) {
    std::string out = "frostmod-masterlist 1 " + std::to_string(unixMs) + "\n";
    for (const Row& r : rows) {
        out += r.address;
        out += '\t';
        out += r.name;
        out += '\n';
    }
    return out;
}

} // namespace masterlist
} // namespace frostmod
