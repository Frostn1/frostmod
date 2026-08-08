// ============================================================================
//  fsclient - the client half of the FrostServer contract.
//
//  Asks a game server's FrostServer companion (frostserver.dlo, see
//  docs/FROSTSERVER.md) "what map are you running, and where do I download it?"
//  over its tiny read-only HTTP API, so FrostMod can offer a one-key download of
//  a track you don't have.
//
//  Every query runs on a worker thread with short timeouts and is cached per
//  endpoint - the game thread only ever reads a Result, never blocks on a socket.
// ============================================================================
#pragma once
#include <string>
#include <cstdint>

namespace frostmod::fsclient {

// Default port frostserver.yaml ships with; a server admin may change it.
constexpr uint16_t kDefaultPort = 54210;

enum class State {
    Idle,        // never asked
    Querying,    // in flight
    Ok,          // answered
    NoServer,    // nothing listening / not a FrostServer / bad reply
};

struct Result {
    State       state = State::Idle;
    std::string serverName;     // "name" from /frostserver/info
    std::string currentMap;     // "" when the server is idle (currentMap: null)
    std::string link;           // mxb-mods.com page; "" when the admin set none
    bool        haveLink = false;
    // The MX Bikes port the answering FrostServer is attached to. One machine can
    // host several servers but only one of them can own the FrostServer port, so
    // this is how a client tells "this answer is about the row I picked" from
    // "this is a different server on the same box". 0 = not reported (older server).
    int         gamePort = 0;
    int         protocol = 0;   // contract version; 0 = not reported
    std::string error;          // short, human-readable; set when state == NoServer
};

using LogFn = void(*)(const char*);

// 'log' (may be null) receives human-readable status lines.
void Init(LogFn log);

// Ask <ip>:<port> for its current map, unless a fresh answer is already cached.
// Returns immediately; poll Get() for the outcome. A query already in flight for
// the same endpoint is not duplicated.
void Query(const std::string& ip, uint16_t port = kDefaultPort);

// Latest known result for an endpoint (State::Idle if never queried).
Result Get(const std::string& ip, uint16_t port = kDefaultPort);

// Drop cached results so the next Query() really goes out on the wire.
void Invalidate();

} // namespace frostmod::fsclient
