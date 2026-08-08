// ============================================================================
//  frostserver - server-side companion for FrostMod.
//
//  Loaded as a PiBoSo plugin (frostserver.dlo) INTO the MX Bikes *dedicated
//  server*. It does two things:
//
//    1. Learns the track the server is currently running - via the sanctioned
//       RaceEvent() plugin callback (no memory RE; the game hands us the name).
//    2. Runs a tiny read-only HTTP API so a client (FrostMod) can ask
//       "what map are you running, and where do I download it?" and get back a
//       mxb-mods.com page link the admin configured. FrostMod then hands that
//       link to the MXB App, which downloads + live-reloads the track - so a
//       player can grab a server's map without leaving the game.
//
//  The map -> link table lives in frostserver.yaml (written with docs on first
//  run, next to the plugin). The HTTP contract is documented in
//  docs/FROSTSERVER.md and is what the client + MXB App consume.
//
//  This same source also builds a standalone frostserver.exe (FROSTSERVER_EXE)
//  that serves the API without a game attached - handy for testing the client /
//  MXB App flow on a dev box. Set the "current" track for testing with
//  --track "Track Name".
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <string>
#include <vector>
#include <utility>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <cstdint>

#include "version.h"

#pragma comment(lib, "ws2_32.lib")   // harmless duplicate of the CMake link

namespace {

// ---------------------------------------------------------------------------
// paths + logging (frostserver.log / frostserver.yaml next to the module)
// ---------------------------------------------------------------------------
std::mutex g_logMutex;
char       g_logPath[MAX_PATH]    = {0};
char       g_configPath[MAX_PATH] = {0};
HMODULE    g_selfModule           = nullptr;   // set in DllMain (DLL build)

// Declared up here because the config reload reports against it (see LoadConfig);
// the server loop that owns it lives further down.
std::atomic<bool> g_httpRunning{false};

// Fill 'out' with the folder (trailing '\') that this binary lives in.
void ModuleDir(char* out, size_t n) {
    char p[MAX_PATH] = {0};
    // g_selfModule for the plugin DLL; NULL => the .exe itself (standalone build)
    if (GetModuleFileNameA(g_selfModule, p, sizeof(p))) {
        if (char* slash = strrchr(p, '\\')) *(slash + 1) = 0;
        strncpy_s(out, n, p, _TRUNCATE);
    } else {
        out[0] = 0;
    }
}

void InitPaths() {
    char dir[MAX_PATH];
    ModuleDir(dir, sizeof(dir));
    if (dir[0]) {
        _snprintf_s(g_logPath,    sizeof(g_logPath),    _TRUNCATE, "%sfrostserver.log",  dir);
        _snprintf_s(g_configPath, sizeof(g_configPath), _TRUNCATE, "%sfrostserver.yaml", dir);
    } else {
        char t[MAX_PATH];
        if (GetTempPathA(sizeof(t), t)) {
            _snprintf_s(g_logPath,    sizeof(g_logPath),    _TRUNCATE, "%sfrostserver.log",  t);
            _snprintf_s(g_configPath, sizeof(g_configPath), _TRUNCATE, "%sfrostserver.yaml", t);
        }
    }
}

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lk(g_logMutex);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
#ifdef FROSTSERVER_EXE
    printf("%s\n", buf);
#endif
    if (g_logPath[0]) {
        if (FILE* f; fopen_s(&f, g_logPath, "a") == 0 && f) {
            SYSTEMTIME st; GetLocalTime(&st);
            fprintf(f, "[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
            fclose(f);
        }
    }
}

// ---------------------------------------------------------------------------
// small string helpers (kept local so frostserver is self-contained)
// ---------------------------------------------------------------------------
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
// strip one pair of surrounding quotes, if present
std::string unquote(std::string v) {
    if (v.size() >= 2 && (v.front() == '\'' || v.front() == '"') && v.back() == v.front())
        v = v.substr(1, v.size() - 2);
    return v;
}
// drop a trailing " # ..." comment (URLs contain no spaces, so this is safe)
std::string stripComment(const std::string& v) {
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i] == '#' && (v[i-1] == ' ' || v[i-1] == '\t')) return trim(v.substr(0, i));
    return v;
}
bool ieq(const std::string& a, const std::string& b) {
    return a.size() == b.size() && _strnicmp(a.c_str(), b.c_str(), a.size()) == 0;
}
// JSON-escape a string value (quotes, backslashes, control chars).
std::string jsonEscape(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; _snprintf_s(b, sizeof(b), _TRUNCATE, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// config: port, server name, and the map-name -> mxb-mods link table
// ---------------------------------------------------------------------------
struct Config {
    int         port     = 54210;
    int         gamePort = 54200;                           // the MX Bikes port this server runs on
    std::string name;                                       // friendly server name (optional)
    std::vector<std::pair<std::string, std::string>> maps;  // {track name, link} in file order
};
std::mutex g_cfgMutex;
Config     g_cfg;

constexpr const char* kConfigVersion = "# frostserver v2";

// Bumped when the JSON contract changes in a way a client must notice. Reported as
// "protocol" in /frostserver/info so an older client can bail out cleanly instead
// of misreading a newer reply.
constexpr int kProtocolVersion = 1;

const char* kDefaultConfig =
    "# frostserver v2\n"
    "# FrostServer - expose this dedicated server's current map + its mxb-mods.com\n"
    "# download link so FrostMod clients can one-click download it (then live-reload,\n"
    "# no game restart). Read-only HTTP API; see docs/FROSTSERVER.md.\n"
    "#\n"
    "# Edits are picked up within a few seconds - no restart needed.\n"
    "\n"
    "port: 54210          # TCP port for the HTTP API; clients reach <server-ip>:<port>\n"
    "name: ''             # optional friendly server name reported in /frostserver/info\n"
    "\n"
    "# The MX Bikes port THIS dedicated server runs on. Clients use it to tell which\n"
    "# server they reached when one machine hosts several: they find FrostServer at\n"
    "# <ip>:<port> above, and check this matches the server row they picked. If you\n"
    "# run more than one server on a box, give each a different 'port' above and set\n"
    "# 'gamePort' to that server's game port.\n"
    "gamePort: 54200\n"
    "\n"
    "# For each track this server runs, the mxb-mods.com page to download it.\n"
    "# The KEY must be the track name EXACTLY as FrostServer logs it - watch\n"
    "# frostserver.log for a line like:  [race] current track: '<name>'\n"
    "# and copy that name here. Quote names that contain spaces or ':'.\n"
    "maps:\n"
    "  # 'Red Bud 2024': https://mxb-mods.com/red-bud-2024/\n"
    "  # 'Some MX Track': https://mxb-mods.com/some-mx-track/\n";

bool configIsCurrent() {
    FILE* f = nullptr;
    if (fopen_s(&f, g_configPath, "r") != 0 || !f) return false;
    char first[128] = {0};
    bool got = fgets(first, sizeof(first), f) != nullptr;
    fclose(f);
    return got && trim(first) == kConfigVersion;
}

// Write defaults if missing or from an older version (backing up the old file).
void ensureConfig() {
    if (!g_configPath[0]) return;
    bool exists = GetFileAttributesA(g_configPath) != INVALID_FILE_ATTRIBUTES;
    if (exists && configIsCurrent()) return;
    if (exists) {
        std::string bak = std::string(g_configPath) + ".bak";
        DeleteFileA(bak.c_str());
        MoveFileA(g_configPath, bak.c_str());
        Log("[config] out of date -> backed up old to %s, rewriting defaults", bak.c_str());
    }
    if (FILE* f; fopen_s(&f, g_configPath, "w") == 0 && f) {
        fputs(kDefaultConfig, f);
        fclose(f);
        Log("[config] wrote default config: %s", g_configPath);
    }
}

// Parse one "maps:" entry line into {name, link}. Handles a quoted key (so the
// name may contain ':') and an unquoted key (split on the first ':').
bool parseMapEntry(const std::string& s, std::string& name, std::string& link) {
    if (s.empty()) return false;
    if (s.front() == '\'' || s.front() == '"') {
        char q = s.front();
        size_t end = s.find(q, 1);
        if (end == std::string::npos) return false;
        name = s.substr(1, end - 1);
        size_t colon = s.find(':', end + 1);
        if (colon == std::string::npos) return false;
        link = trim(s.substr(colon + 1));
    } else {
        size_t colon = s.find(':');
        if (colon == std::string::npos) return false;
        name = trim(s.substr(0, colon));
        link = trim(s.substr(colon + 1));
    }
    name = unquote(name);
    link = unquote(stripComment(link));
    return !name.empty() && !link.empty();
}

void LoadConfig() {
    Config c;
    FILE* f = nullptr;
    if (fopen_s(&f, g_configPath, "r") != 0 || !f) {
        Log("[config] could not open %s; using defaults (port %d, no maps).",
            g_configPath, c.port);
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        g_cfg = std::move(c);
        return;
    }
    bool inMaps = false;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        bool indented = (line[0] == ' ' || line[0] == '\t');
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;

        if (indented && inMaps) {
            std::string name, link;
            if (parseMapEntry(s, name, link)) c.maps.emplace_back(name, link);
            else Log("[config] skipped unparseable maps entry: %s", s.c_str());
            continue;
        }

        // top-level key: value
        size_t colon = s.find(':');
        if (colon == std::string::npos) { Log("[config] ignoring line: %s", s.c_str()); continue; }
        std::string key = lower(trim(s.substr(0, colon)));
        std::string val = trim(s.substr(colon + 1));
        inMaps = false;
        if      (key == "maps") inMaps = true;
        else if (key == "port") { int p = atoi(stripComment(val).c_str()); if (p > 0 && p < 65536) c.port = p; }
        else if (key == "gameport") { int p = atoi(stripComment(val).c_str()); if (p > 0 && p < 65536) c.gamePort = p; }
        else if (key == "name") c.name = unquote(stripComment(val));
        else Log("[config] ignoring unknown key: %s", key.c_str());
    }
    fclose(f);

    Log("[config] loaded: port=%d gamePort=%d name='%s' maps=%zu",
        c.port, c.gamePort, c.name.c_str(), c.maps.size());
    int oldPort, newPort;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        oldPort = g_cfg.port;
        newPort = c.port;
        g_cfg = std::move(c);
    }
    // The listener is bound at startup; a port change needs a restart to take effect.
    // Say so rather than letting the file and reality disagree in silence.
    if (g_httpRunning.load() && oldPort != newPort)
        Log("[config] NOTE: 'port' is now %d but the API is already listening on %d - "
            "restart the server to move it.", newPort, oldPort);
}

// Last write time of the config, so edits can be picked up without a restart. A
// dedicated server runs for days; an admin who adds a link for tonight's track
// shouldn't have to bounce it (and kick everyone) for the link to go live.
uint64_t ConfigWriteTime() {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!g_configPath[0] || !GetFileAttributesExA(g_configPath, GetFileExInfoStandard, &fad))
        return 0;
    return ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
}

std::atomic<uint64_t> g_cfgStamp{0};

// Re-read the config if it changed on disk. Called just before each request is
// answered, so a reply always reflects the file as it is now, at the cost of one
// file-attribute query per request - no polling thread, nothing on a timer.
void ReloadConfigIfChanged() {
    const uint64_t now = ConfigWriteTime();
    if (!now || now == g_cfgStamp.load()) return;
    g_cfgStamp.store(now);
    Log("[config] frostserver.yaml changed on disk - reloading.");
    LoadConfig();
}

// Fold a track name to its comparable form: lowercase, letters and digits only.
// The admin types these keys by hand from a log line, so "Red Bud 2024",
// "red bud 2024" and "RedBud2024" must all resolve to the same entry - an exact
// match would silently return "no link" over a stray space or apostrophe, which
// looks to a player exactly like the admin never configured one.
std::string foldName(const std::string& s) {
    std::string o;
    for (unsigned char c : s)
        if (std::isalnum(c)) o += (char)std::tolower(c);
    return o;
}

// Look up the mxb-mods link for a track name. Exact (case-insensitive) match wins;
// otherwise fall back to the folded comparison above. "" if none.
std::string LinkForTrack(const std::string& track) {
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    for (const auto& m : g_cfg.maps)
        if (ieq(m.first, track)) return m.second;
    const std::string want = foldName(track);
    if (want.empty()) return "";
    for (const auto& m : g_cfg.maps)
        if (foldName(m.first) == want) return m.second;
    return "";
}

// ---------------------------------------------------------------------------
// current-track state (fed by RaceEvent / RaceDeinit, or --track in exe mode)
// ---------------------------------------------------------------------------
std::mutex  g_trackMutex;
bool        g_trackActive = false;
std::string g_trackName;

void SetCurrentTrack(const std::string& name) {
    std::lock_guard<std::mutex> lk(g_trackMutex);
    g_trackActive = !name.empty();
    g_trackName   = name;
}
void ClearCurrentTrack() {
    std::lock_guard<std::mutex> lk(g_trackMutex);
    g_trackActive = false;
    g_trackName.clear();
}
bool GetCurrentTrack(std::string& out) {
    std::lock_guard<std::mutex> lk(g_trackMutex);
    out = g_trackName;
    return g_trackActive;
}

// ---------------------------------------------------------------------------
// tiny HTTP/1.1 server (read-only GET; one connection at a time)
// ---------------------------------------------------------------------------
// Shared between the server thread and Stop* only at shutdown; on Win64 an
// aligned pointer-sized read/write is atomic, which is all this needs.
SOCKET            g_listenSock = INVALID_SOCKET;

std::string BuildInfoJson() {
    std::string track;
    bool active = GetCurrentTrack(track);
    std::string serverName; std::string version = FROSTMOD_VERSION;
    int gamePort;
    { std::lock_guard<std::mutex> lk(g_cfgMutex); serverName = g_cfg.name; gamePort = g_cfg.gamePort; }

    char nums[64];
    _snprintf_s(nums, sizeof(nums), _TRUNCATE, "\"protocol\":%d,\"gamePort\":%d,",
                kProtocolVersion, gamePort);

    std::string j = "{\"frostserver\":\"" + jsonEscape(version) + "\",";
    j += nums;
    j += "\"name\":\"" + jsonEscape(serverName) + "\",";
    if (!active) {
        j += "\"currentMap\":null}";
    } else {
        std::string link = LinkForTrack(track);
        j += "\"currentMap\":{\"name\":\"" + jsonEscape(track) + "\",";
        if (link.empty()) j += "\"link\":null,\"haveLink\":false}}";
        else              j += "\"link\":\"" + jsonEscape(link) + "\",\"haveLink\":true}}";
    }
    return j;
}

std::string BuildMapsJson() {
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    std::string j = "{\"maps\":[";
    for (size_t i = 0; i < g_cfg.maps.size(); ++i) {
        if (i) j += ",";
        j += "{\"name\":\"" + jsonEscape(g_cfg.maps[i].first) + "\",";
        j += "\"link\":\"" + jsonEscape(g_cfg.maps[i].second) + "\"}";
    }
    j += "]}";
    return j;
}

void SendResponse(SOCKET c, const char* status, const char* contentType, const std::string& body) {
    char header[256];
    int n = _snprintf_s(header, sizeof(header), _TRUNCATE,
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, contentType, body.size());
    if (n > 0) send(c, header, n, 0);
    if (!body.empty()) send(c, body.c_str(), (int)body.size(), 0);
}

// Extract the request path (without query) from a raw request. "" on failure.
std::string ParsePath(const char* req, int len) {
    // "GET /path?x HTTP/1.1"
    const char* p = (const char*)memchr(req, ' ', len);
    if (!p) return "";
    ++p;
    const char* end = req + len;
    const char* q = p;
    while (q < end && *q != ' ' && *q != '?' && *q != '\r' && *q != '\n') ++q;
    return std::string(p, q - p);
}

void HandleConnection(SOCKET c) {
    char buf[4096];
    int len = recv(c, buf, sizeof(buf) - 1, 0);
    if (len <= 0) return;
    buf[len] = 0;

    // only GET is supported
    if (_strnicmp(buf, "GET ", 4) != 0) {
        SendResponse(c, "405 Method Not Allowed", "text/plain", "method not allowed");
        return;
    }
    std::string path = ParsePath(buf, len);

    if (path == "/frostserver/info")
        SendResponse(c, "200 OK", "application/json", BuildInfoJson());
    else if (path == "/frostserver/maps")
        SendResponse(c, "200 OK", "application/json", BuildMapsJson());
    else if (path == "/health" || path == "/frostserver/health")
        SendResponse(c, "200 OK", "text/plain", "ok");
    else
        SendResponse(c, "404 Not Found", "text/plain", "not found");
}

void HttpServerLoop(int port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Log("[http] WSAStartup failed; API disabled.");
        return;
    }
    g_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listenSock == INVALID_SOCKET) {
        Log("[http] socket() failed (%d); API disabled.", WSAGetLastError());
        WSACleanup();
        return;
    }
    BOOL yes = TRUE;
    setsockopt(g_listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;             // reachable from other machines
    addr.sin_port        = htons((u_short)port);
    if (bind(g_listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        Log("[http] bind(:%d) failed (%d) - port in use? API disabled.", port, WSAGetLastError());
        closesocket(g_listenSock); g_listenSock = INVALID_SOCKET; WSACleanup();
        return;
    }
    if (listen(g_listenSock, 8) == SOCKET_ERROR) {
        Log("[http] listen() failed (%d); API disabled.", WSAGetLastError());
        closesocket(g_listenSock); g_listenSock = INVALID_SOCKET; WSACleanup();
        return;
    }

    Log("[http] listening on 0.0.0.0:%d  (GET /frostserver/info, /frostserver/maps, /health)", port);
    while (g_httpRunning.load()) {
        SOCKET c = accept(g_listenSock, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            if (!g_httpRunning.load()) break;    // closed by StopHttpServer
            continue;
        }
        ReloadConfigIfChanged();     // answer from the file as it is right now
        // Never let a bad request take down the listener.
        __try { HandleConnection(c); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[http] request handler faulted - caught."); }
        closesocket(c);
    }
    SOCKET s = g_listenSock; g_listenSock = INVALID_SOCKET;
    if (s != INVALID_SOCKET) closesocket(s);
    WSACleanup();
    Log("[http] server stopped.");
}

void StartHttpServer() {
    if (g_httpRunning.exchange(true)) return;   // already running
    int port;
    { std::lock_guard<std::mutex> lk(g_cfgMutex); port = g_cfg.port; }
    // Detached on purpose: we never join. Joining from DllMain/DLL_PROCESS_DETACH
    // would deadlock on the loader lock, and a joinable std::thread still alive at
    // exit calls std::terminate. Stop signals via the atomic + closing the socket.
    std::thread(HttpServerLoop, port).detach();
}

void StopHttpServer() {
    if (!g_httpRunning.exchange(false)) return;
    SOCKET s = g_listenSock;             // hand off, then close to unblock accept()
    g_listenSock = INVALID_SOCKET;
    if (s != INVALID_SOCKET) closesocket(s);
}

// Diagnostic: log every run of >=3 printable ASCII chars in a buffer, with its
// offset. Lets a dedicated-server run reveal exactly where a callback's
// track/name strings sit - verifying the struct layout on THIS game build
// instead of trusting it (RaceEvent's track is expected at +0x68).
//
// OPT-IN: this fires on every race event, which on a server that runs for days is
// a lot of log for something only needed while confirming the layout once. Drop an
// empty 'frostserver_probe.flag' next to frostserver.log to turn it on.
std::atomic<bool> g_probeOn{false};

void InitProbeFlag() {
    char flag[MAX_PATH] = {0};
    ModuleDir(flag, sizeof(flag));
    if (!flag[0]) return;
    strcat_s(flag, "frostserver_probe.flag");
    const bool on = GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES;
    g_probeOn.store(on);
    Log(on ? "[probe] frostserver_probe.flag present - dumping raw callback fields."
           : "[probe] off (drop frostserver_probe.flag next to the log to dump raw "
             "callback fields when verifying a new game build).");
}

void LogAsciiRuns(const char* tag, const void* data, int size) {
    if (!g_probeOn.load()) return;
    if (!data || size <= 0) return;
    const unsigned char* p = (const unsigned char*)data;
    int cap = size < 512 ? size : 512;      // guard against a bogus size
    int start = -1;
    char run[128];
    for (int i = 0; i <= cap; ++i) {
        bool printable = (i < cap) && p[i] >= 0x20 && p[i] < 0x7f;
        if (printable && start < 0) start = i;
        if (!printable && start >= 0) {
            int len = i - start;
            if (len >= 3) {
                int copy = len < (int)sizeof(run) - 1 ? len : (int)sizeof(run) - 1;
                memcpy(run, p + start, (size_t)copy); run[copy] = 0;
                Log("%s +0x%02X: '%s'", tag, start, run);
            }
            start = -1;
        }
    }
}

// Shared init used by both the plugin Startup() and the standalone main(). The
// overrides exist for the .exe's --port/--name and must be applied after the config
// is read but before the socket is bound, which is why they live in here.
void FrostServerInit(const char* how, int portOverride = 0, const char* nameOverride = nullptr) {
    InitPaths();
    Log("=============== FrostServer %s starting (%s) ===============", FROSTMOD_VERSION, how);
    InitProbeFlag();
    ensureConfig();
    LoadConfig();
    g_cfgStamp.store(ConfigWriteTime());   // baseline, so we only reload on real edits
    if (portOverride || (nameOverride && *nameOverride)) {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        if (portOverride) { g_cfg.port = portOverride; Log("[cfg] port overridden: %d", portOverride); }
        if (nameOverride && *nameOverride) {
            g_cfg.name = nameOverride;
            Log("[cfg] name overridden: '%s'", nameOverride);
        }
    }
    StartHttpServer();
}

} // namespace

// ===========================================================================
// PiBoSo plugin exports (loaded by the MX Bikes dedicated server).
// Identity values must match this game build: "mxbikes", data 8, interface 9
// (same as frostmod.dll). We only use RaceEvent (current track) + RaceDeinit.
// ===========================================================================
#ifndef FROSTSERVER_EXE

// PiBoSo SPluginsRaceEvent_t (verbatim from mxb_example.c): the running track.
struct SPluginsRaceEvent_t {
    int   m_iType;               // session type
    char  m_szName[100];         // event name
    char  m_szTrackName[100];    // track name  <-- what we expose
    float m_fTrackLength;
};

extern "C" {

__declspec(dllexport) char* GetModID() { static char id[] = "mxbikes"; return id; }
__declspec(dllexport) int   GetModDataVersion()   { return 8; }
__declspec(dllexport) int   GetInterfaceVersion() { return 9; }

__declspec(dllexport) int Startup(char* /*_szSavePath*/) {
    FrostServerInit("plugin");
    return 3;   // telemetry rate (unused, must be valid to stay loaded)
}

__declspec(dllexport) void Shutdown() {
    Log("[plugin] Shutdown() requested by game.");
    StopHttpServer();
}

// Called when a race/session on a track begins. _pData is a SPluginsRaceEvent_t;
// the running track is m_szTrackName (+0x68), confirmed against the decompiled
// server plugin loader (resolver 0x14012A4F0; forwarder in the server module at
// 0x14028FF81; the -dedicated flag 0x565E64 gates only startup, not this path).
__declspec(dllexport) void RaceEvent(void* _pData, int _iDataSize) {
    SPluginsRaceEvent_t e = {};
    if (_pData && _iDataSize > 0) {
        int n = _iDataSize < (int)sizeof(e) ? _iDataSize : (int)sizeof(e);
        memcpy(&e, _pData, (size_t)n);
    }
    e.m_szName[sizeof(e.m_szName) - 1]           = 0;   // ensure NUL-terminated
    e.m_szTrackName[sizeof(e.m_szTrackName) - 1] = 0;
    SetCurrentTrack(e.m_szTrackName);
    Log("[race] RaceEvent size=%d type=%d  track='%s'  event='%s'",
        _iDataSize, e.m_iType, e.m_szTrackName, e.m_szName);
    LogAsciiRuns("[race.raw]", _pData, _iDataSize);   // probe: verify field layout
}

// Called when the race/session ends - no track running.
__declspec(dllexport) void RaceDeinit() {
    ClearCurrentTrack();
    Log("[race] RaceDeinit - no current track.");
}

// --- probe callbacks --------------------------------------------------------
// Implemented so the FIRST dedicated-server run tells us empirically which
// callbacks fire (proving the runtime vtable fan-out to plugins that static
// analysis couldn't isolate) and where each carries the track name. They only
// log; the authoritative track source stays RaceEvent's m_szTrackName. Once the
// log confirms the picture on a real server, these can be trimmed.
__declspec(dllexport) void EventInit(void* _pData, int _iDataSize) {
    Log("[race] EventInit size=%d", _iDataSize);
    LogAsciiRuns("[event.raw]", _pData, _iDataSize);
}
__declspec(dllexport) void EventDeinit() { Log("[race] EventDeinit"); }
__declspec(dllexport) void RaceSession(void* _pData, int _iDataSize) {
    Log("[race] RaceSession size=%d", _iDataSize);
    LogAsciiRuns("[session.raw]", _pData, _iDataSize);
}
__declspec(dllexport) void RaceAddEntry(void* _pData, int _iDataSize) {
    Log("[race] RaceAddEntry size=%d", _iDataSize);
    LogAsciiRuns("[entry.raw]", _pData, _iDataSize);
}

} // extern "C"

// Capture our own module handle so we resolve the log/config next to the .dlo.
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = hInst;
        DisableThreadLibraryCalls(hInst);
    }
    // No teardown in DLL_PROCESS_DETACH: at process exit the server thread is
    // already gone, and touching the loader lock / winsock here is unsafe. The
    // game's Shutdown() export is the graceful stop path.
    return TRUE;
}

#else // FROSTSERVER_EXE
// ===========================================================================
// Standalone frostserver.exe - serve the API with no game attached, for
// testing the client / MXB App flow. Optionally seed a "current" track.
//   frostserver.exe --track "Red Bud 2024" [--port 54210] [--name "My Server"]
// ===========================================================================
namespace {
std::atomic<bool> g_exeStop{false};

// Ctrl+C / console close: stop the listener and let main() fall through, so the
// socket is closed and the last log line is flushed instead of the process being
// shot mid-write.
BOOL WINAPI ConsoleCtrl(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT: case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT: case CTRL_LOGOFF_EVENT: case CTRL_SHUTDOWN_EVENT:
            Log("[exe] stop requested - shutting down.");
            StopHttpServer();
            g_exeStop.store(true);
            return TRUE;
        default:
            return FALSE;
    }
}

void Usage() {
    printf("FrostServer %s - map/link API for MX Bikes\n\n"
           "  frostserver.exe [--track \"Name\"] [--port N] [--name \"Server\"]\n\n"
           "  --track, -t   seed the 'current' track (the real plugin learns this\n"
           "                from the game; this is for testing a client)\n"
           "  --port,  -p   override the API port from frostserver.yaml\n"
           "  --name,  -n   override the server name from frostserver.yaml\n"
           "  --help,  -h   this text\n\n"
           "Config + full HTTP contract: docs/FROSTSERVER.md\n", FROSTMOD_VERSION);
}
} // namespace

int main(int argc, char** argv) {
    std::string seedTrack, nameOverride;
    int portOverride = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool hasNext = (i + 1 < argc);
        if ((_stricmp(a, "--track") == 0 || _stricmp(a, "-t") == 0) && hasNext)
            seedTrack = argv[++i];
        else if ((_stricmp(a, "--port") == 0 || _stricmp(a, "-p") == 0) && hasNext)
            portOverride = atoi(argv[++i]);
        else if ((_stricmp(a, "--name") == 0 || _stricmp(a, "-n") == 0) && hasNext)
            nameOverride = argv[++i];
        else if (_stricmp(a, "--help") == 0 || _stricmp(a, "-h") == 0 || _stricmp(a, "/?") == 0) {
            Usage(); return 0;
        } else {
            printf("unknown argument: %s\n\n", a); Usage(); return 2;
        }
    }
    if (portOverride && (portOverride < 1 || portOverride > 65535)) {
        printf("--port must be 1..65535\n"); return 2;
    }

    FrostServerInit("standalone", portOverride, nameOverride.c_str());

    SetConsoleCtrlHandler(ConsoleCtrl, TRUE);
    if (!seedTrack.empty()) {
        SetCurrentTrack(seedTrack);
        Log("[exe] seeded current track: '%s'", seedTrack.c_str());
    }
    Log("[exe] running. Press Ctrl+C to stop.");
    while (!g_exeStop.load()) Sleep(200);
    Sleep(200);            // let the accept loop notice the closed socket and log its exit
    Log("[exe] stopped.");
    return 0;
}
#endif
