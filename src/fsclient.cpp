// ============================================================================
//  fsclient - see fsclient.h.
//
//  A FrostServer query must never be felt in-game, so:
//    * every request runs on its own detached worker thread - the game thread
//      only reads a cached Result under a mutex,
//    * timeouts are deliberately short (a server with no FrostServer must fail
//      fast, and most servers won't have one),
//    * results are cached per endpoint with a TTL, so scrolling the server list
//      doesn't re-hit the network,
//    * we go straight out with no proxy: these are raw game-server IPs on
//      non-standard ports, and a corporate proxy would only ever break them.
//
//  The reply is our own tiny contract (docs/FROSTSERVER.md), so it's read with a
//  few targeted field extractions rather than a JSON dependency - same approach
//  the launcher's GitHub update check takes.
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cctype>

#include "fsclient.h"

#pragma comment(lib, "winhttp.lib")   // harmless duplicate of the CMake link

namespace frostmod::fsclient {
namespace {

LogFn g_log = nullptr;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log(buf);
}

// How long an answer is trusted before we ask again. A server that answered is
// re-asked sooner (its map changes between races); one that didn't is left alone
// longer, since "no FrostServer here" won't change mid-session.
constexpr ULONGLONG kTtlOkMs       = 30 * 1000;
constexpr ULONGLONG kTtlNoServerMs = 5 * 60 * 1000;
constexpr int       kMaxInFlight   = 4;      // don't storm the network on a long list

struct Entry {
    Result    result;
    ULONGLONG stamp = 0;      // when 'result' was written
};

std::mutex                  g_mutex;
std::map<std::string, Entry> g_cache;        // key: "ip:port"
std::atomic<int>            g_inFlight{0};

std::string Key(const std::string& ip, uint16_t port) {
    char k[64];
    _snprintf_s(k, sizeof(k), _TRUNCATE, "%s:%u", ip.c_str(), port);
    return k;
}

// ---------------------------------------------------------------------------
// minimal reply reading - our own contract, so a few field lookups suffice
// ---------------------------------------------------------------------------

// Un-escape the JSON string starting after an opening quote at 'i'; leaves 'i' on
// the closing quote. Only the escapes jsonEscape() in frostserver.cpp can emit.
std::string ReadJsonString(const std::string& s, size_t& i) {
    std::string out;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') break;
        if (c != '\\') { out += c; continue; }
        if (++i >= s.size()) break;
        switch (s[i]) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {                                  // \uXXXX - we only emit < 0x20
                if (i + 4 < s.size()) {
                    int v = (int)strtol(s.substr(i + 1, 4).c_str(), nullptr, 16);
                    if (v > 0 && v < 0x80) out += (char)v;
                    i += 4;
                }
                break;
            }
            default: out += s[i]; break;                 // \" and \\ (and anything else)
        }
    }
    return out;
}

// Find "key": in [from, to) and return the position just past the colon, or npos.
size_t FindField(const std::string& s, const char* key, size_t from, size_t to) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat, from);
    if (p == std::string::npos || p >= to) return std::string::npos;
    p += pat.size();
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    if (p >= s.size() || s[p] != ':') return std::string::npos;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    return p;
}

// String-valued field; "" if absent or not a string (e.g. explicit null).
std::string StringField(const std::string& s, const char* key,
                        size_t from = 0, size_t to = std::string::npos) {
    if (to == std::string::npos) to = s.size();
    size_t p = FindField(s, key, from, to);
    if (p == std::string::npos || p >= s.size() || s[p] != '"') return "";
    ++p;
    return ReadJsonString(s, p);
}

// True when the field is present and literally true.
bool BoolField(const std::string& s, const char* key, size_t from, size_t to) {
    size_t p = FindField(s, key, from, to);
    return p != std::string::npos && s.compare(p, 4, "true") == 0;
}

// Number-valued field; 'fallback' if absent or not a number.
int IntField(const std::string& s, const char* key, size_t from, size_t to, int fallback = 0) {
    size_t p = FindField(s, key, from, to);
    if (p == std::string::npos || p >= s.size()) return fallback;
    if (s[p] != '-' && !isdigit((unsigned char)s[p])) return fallback;
    return atoi(s.c_str() + p);
}

// Parse a /frostserver/info body into 'out'. False if it isn't one of ours.
bool ParseInfo(const std::string& body, Result& out) {
    if (body.find("\"frostserver\"") == std::string::npos) return false;

    const size_t mapPos = body.find("\"currentMap\"");
    // The top-level "name" is the one before currentMap; the map's own "name" is
    // after it - so bound each lookup instead of trusting field order.
    const size_t topEnd = (mapPos == std::string::npos) ? body.size() : mapPos;
    out.serverName = StringField(body, "name", 0, topEnd);
    out.protocol   = IntField(body, "protocol", 0, topEnd);
    out.gamePort   = IntField(body, "gamePort", 0, topEnd);

    if (mapPos == std::string::npos) return true;             // no map field at all
    size_t v = FindField(body, "currentMap", mapPos, body.size());
    if (v == std::string::npos) return true;
    if (body.compare(v, 4, "null") == 0) return true;         // server idle - no track

    out.currentMap = StringField(body, "name",     mapPos, body.size());
    out.link       = StringField(body, "link",     mapPos, body.size());
    out.haveLink   = BoolField  (body, "haveLink", mapPos, body.size()) && !out.link.empty();
    return true;
}

// ---------------------------------------------------------------------------
// the request itself
// ---------------------------------------------------------------------------
std::wstring Widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());   // IPs are ASCII
}

// GET http://<ip>:<port>/frostserver/info. Returns false + 'err' on any failure.
bool HttpGetInfo(const std::string& ip, uint16_t port, std::string& body, std::string& err) {
    HINTERNET ses = WinHttpOpen(L"FrostMod",
                                WINHTTP_ACCESS_TYPE_NO_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { err = "winhttp init failed"; return false; }
    // resolve / connect / send / receive - all short: a missing FrostServer is the
    // common case and must not keep a worker (or the UI's "querying...") around.
    WinHttpSetTimeouts(ses, 1000, 2000, 2000, 3000);

    bool ok = false;
    if (HINTERNET con = WinHttpConnect(ses, Widen(ip).c_str(), port, 0)) {
        if (HINTERNET req = WinHttpOpenRequest(con, L"GET", L"/frostserver/info", nullptr,
                                               WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, 0)) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                && WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                                    WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    for (;;) {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                        if (body.size() + avail > 64 * 1024) { avail = 0; break; }  // sanity cap
                        std::vector<char> chunk(avail);
                        DWORD got = 0;
                        if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0) break;
                        body.append(chunk.data(), got);
                    }
                    ok = !body.empty();
                    if (!ok) err = "empty reply";
                } else {
                    char b[64]; _snprintf_s(b, sizeof(b), _TRUNCATE, "HTTP %lu", status);
                    err = b;
                }
            } else {
                err = "no answer";      // by far the common case: no FrostServer there
            }
            WinHttpCloseHandle(req);
        } else err = "request failed";
        WinHttpCloseHandle(con);
    } else err = "connect failed";

    WinHttpCloseHandle(ses);
    return ok;
}

void Store(const std::string& key, Result r) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Entry& e = g_cache[key];
    e.result = std::move(r);
    e.stamp  = GetTickCount64();
}

} // namespace

void Init(LogFn log) { g_log = log; }

void Query(const std::string& ip, uint16_t port) {
    if (ip.empty() || !port) return;
    const std::string key = Key(ip, port);

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) {
            const Result& r = it->second.result;
            if (r.state == State::Querying) return;                  // already in flight
            const ULONGLONG ttl = (r.state == State::Ok) ? kTtlOkMs : kTtlNoServerMs;
            if (GetTickCount64() - it->second.stamp < ttl) return;   // still fresh
        }
        if (g_inFlight.load() >= kMaxInFlight) return;               // try again next frame
        Entry& e = g_cache[key];
        e.result = Result{};
        e.result.state = State::Querying;
        e.stamp = GetTickCount64();
    }

    g_inFlight.fetch_add(1);
    // Detached on purpose: the DLL lives for the life of the game, and joining from
    // a render-thread callback would stall the frame we are trying not to disturb.
    std::thread([ip, port, key] {
        std::string body, err;
        Result r;
        if (HttpGetInfo(ip, port, body, err) && ParseInfo(body, r)) {
            r.state = State::Ok;
            const std::string what =
                r.currentMap.empty() ? std::string("idle (no track running)")
                                     : r.currentMap + (r.haveLink ? " (link configured)"
                                                                  : " (NO link configured)");
            Log("[fs] %s:%u -> FrostServer '%s', %s",
                ip.c_str(), port, r.serverName.c_str(), what.c_str());
        } else {
            r.state = State::NoServer;
            r.error = err.empty() ? "not a FrostServer" : err;
            Log("[fs] %s:%u -> no FrostServer (%s).", ip.c_str(), port, r.error.c_str());
        }
        Store(key, std::move(r));
        g_inFlight.fetch_sub(1);
    }).detach();
}

Result Get(const std::string& ip, uint16_t port) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_cache.find(Key(ip, port));
    return it == g_cache.end() ? Result{} : it->second.result;
}

void Invalidate() {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto it = g_cache.begin(); it != g_cache.end(); ) {
        if (it->second.result.state == State::Querying) ++it;   // let it land
        else it = g_cache.erase(it);
    }
}

} // namespace frostmod::fsclient
