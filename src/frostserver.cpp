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
    int         port = 54210;
    std::string name;                                       // friendly server name (optional)
    std::vector<std::pair<std::string, std::string>> maps;  // {track name, link} in file order
};
std::mutex g_cfgMutex;
Config     g_cfg;

constexpr const char* kConfigVersion = "# frostserver v1";

const char* kDefaultConfig =
    "# frostserver v1\n"
    "# FrostServer - expose this dedicated server's current map + its mxb-mods.com\n"
    "# download link so FrostMod clients can one-click download it (then live-reload,\n"
    "# no game restart). Read-only HTTP API; see docs/FROSTSERVER.md.\n"
    "\n"
    "port: 54210          # TCP port for the HTTP API; clients reach <server-ip>:<port>\n"
    "name: ''             # optional friendly server name reported in /frostserver/info\n"
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
        else if (key == "name") c.name = unquote(stripComment(val));
        else Log("[config] ignoring unknown key: %s", key.c_str());
    }
    fclose(f);

    Log("[config] loaded: port=%d name='%s' maps=%zu", c.port, c.name.c_str(), c.maps.size());
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    g_cfg = std::move(c);
}

// Look up the mxb-mods link for a track name (case-insensitive). "" if none.
std::string LinkForTrack(const std::string& track) {
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    for (const auto& m : g_cfg.maps)
        if (ieq(m.first, track)) return m.second;
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
std::atomic<bool> g_httpRunning{false};
// Shared between the server thread and Stop* only at shutdown; on Win64 an
// aligned pointer-sized read/write is atomic, which is all this needs.
SOCKET            g_listenSock = INVALID_SOCKET;

std::string BuildInfoJson() {
    std::string track;
    bool active = GetCurrentTrack(track);
    std::string serverName; std::string version = FROSTMOD_VERSION;
    { std::lock_guard<std::mutex> lk(g_cfgMutex); serverName = g_cfg.name; }

    std::string j = "{\"frostserver\":\"" + jsonEscape(version) + "\",";
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
// offset. Lets the first dedicated-server run reveal exactly where a callback's
// track/name strings sit - verifying the struct layout on THIS game build
// instead of trusting it (RaceEvent's track is expected at +0x68).
void LogAsciiRuns(const char* tag, const void* data, int size) {
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

// Shared init used by both the plugin Startup() and the standalone main().
void FrostServerInit() {
    InitPaths();
    Log("=============== FrostServer %s starting ===============", FROSTMOD_VERSION);
    ensureConfig();
    LoadConfig();
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
    FrostServerInit();
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
//   frostserver.exe --track "Red Bud 2024"
// ===========================================================================
int main(int argc, char** argv) {
    std::string seedTrack;
    for (int i = 1; i < argc; ++i) {
        if ((_stricmp(argv[i], "--track") == 0 || _stricmp(argv[i], "-t") == 0) && i + 1 < argc)
            seedTrack = argv[++i];
    }
    FrostServerInit();
    if (!seedTrack.empty()) {
        SetCurrentTrack(seedTrack);
        Log("[exe] seeded current track: '%s'", seedTrack.c_str());
    }
    Log("[exe] running. Press Ctrl+C to stop.");
    for (;;) Sleep(1000);   // serve until killed
}
#endif
