// ============================================================================
//  serverfilter - client-side spam/"ghost" server filter for MX Bikes.
//
//  MX Bikes fetches its online server list from the master server
//  (master.mx-bikes.com, UDP 54200). Some entries are junk "ghost" servers that
//  register just to advertise and can't be joined. This module decides, per
//  server entry, whether to HIDE it from your browser - purely client-side, your
//  view only. The actual hook that feeds entries in lives in frostmod.cpp and is
//  wired up once the server-list function is reverse-engineered (see offsets.h).
//
//  Rules come from frostmod_serverfilter.txt (created with docs on first run).
// ============================================================================
#pragma once
#include <string>
#include <cstdint>

namespace frostmod::serverfilter {

// One server entry, as extracted from the game by the hook.
struct ServerInfo {
    std::string name;              // display name
    std::string ip;                // "a.b.c.d" or host; empty if unknown
    uint16_t    port       = 0;
    int         players    = -1;   // -1 = unknown
    int         maxPlayers = -1;
    bool        locked     = false; // password protected
    bool        unjoinable = false; // ping shown as "---" (ghost/ad servers)
};

using LogFn = void(*)(const char*);

// Load rules from configPath, writing a documented default file if it's missing.
// 'log' (may be null) receives human-readable status lines.
void Init(const std::string& configPath, LogFn log);

// Re-read the rules from the same path (e.g. after you edit the file).
void Reload();

// "" => show this server. Non-empty => hide it; the string is a short reason
// suitable for logging (e.g. "name matches /discord/").
std::string ShouldHide(const ServerInfo& s);

} // namespace frostmod::serverfilter
