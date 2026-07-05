// ============================================================================
//  serverfilter - see serverfilter.h. Pure rule engine + config; no game hooks
//  here (those live in frostmod.cpp). Safe to unit-test in isolation.
// ============================================================================
#include "serverfilter.h"

#include <windows.h>          // GetTickCount64
#include <vector>
#include <regex>
#include <mutex>
#include <unordered_map>
#include <cstdio>
#include <cstdarg>
#include <cstring>            // strlen / _strnicmp
#include <cstdlib>            // atoi
#include <cctype>

namespace frostmod::serverfilter {
namespace {

std::mutex  g_mx;
std::string g_path;
LogFn       g_log = nullptr;

struct Rules {
    std::vector<std::string> nameContains;   // lowercased substrings
    std::vector<std::regex>  regexes;         // compiled (icase)
    std::vector<std::string> regexSrc;        // source text, for logging
    int  maxPerIP  = 0;                        // 0 = disabled
    bool hideLocked = false;
    bool hideEmpty  = false;
    bool hideUnjoinable = false;               // ping "---": USELESS at list-build time (the
                                               // browser shows "---" for EVERY server until pings
                                               // resolve), so OFF by default - it would hide all.
};
Rules g_rules;

// dup-IP tracking, reset each "refresh" (a gap since the last add)
std::unordered_map<std::string, int> g_ipCount;
uint64_t g_lastAddTick = 0;

void logf(const char* fmt, ...) {
    if (!g_log) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log(buf);
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
bool starts_with(const std::string& s, const char* p) {
    size_t n = strlen(p);
    return s.size() >= n && _strnicmp(s.c_str(), p, n) == 0;
}

// Bump this when the shipped defaults change; the loader auto-rewrites an older
// file (backing the old one up to .bak) so new rules land without a manual delete.
constexpr const char* kConfigVersion = "# frostmod-filter v2";

const char* kDefaultConfig =
    "# frostmod-filter v2\n"
    "# FrostMod - server browser spam filter (client-side, your view only).\n"
    "# Hide junk / ad \"ghost\" servers from the MX Bikes list.\n"
    "# One rule per line; '#' starts a comment; matching is case-insensitive.\n"
    "# (Edit freely. On a version bump this file is backed up to .bak and rewritten.)\n"
    "#\n"
    "#   name: <text>      hide a server whose NAME contains <text>\n"
    "#   regex: <pattern>  hide a server whose NAME matches this ECMAScript regex\n"
    "#   maxPerIP: <n>     hide servers past <n> from the same IP per refresh (0=off)\n"
    "#   hideLocked: 0|1   hide password-locked servers\n"
    "#   hideEmpty: 0|1    hide servers with 0 CURRENT players (many legit servers are\n"
    "#                     just empty right now, so leave this OFF unless you want that)\n"
    "#   hideUnjoinable: 0|1  hide servers whose ping shows '---'. NOTE: the browser shows\n"
    "#                     '---' for EVERY server until pings resolve, so at list-build\n"
    "#                     time this hides ALL of them. Keep it OFF - filter by name.\n"
    "#\n"
    "# --- defaults: hide ad/spam servers by NAME (the reliable signal). ---\n"
    "hideUnjoinable: 0\n"
    "hideEmpty: 0\n"
    "hideLocked: 0\n"
    "maxPerIP: 0\n"
    "# Ad/hosting spam + cheat ads seen in the live list. Kept PRECISE so the legit\n"
    "# race servers ('CBRSERVERS.COM', 'CBR Demo Server', community '.com' names) stay:\n"
    "name: cbrhosting.com\n"
    "name: mymxb\n"
    "name: cheap dedi\n"
    "name: server hosting\n"
    "name: dedicated servers\n"
    "name: rent your server\n"
    "# cheat-shop ghost ads (they leetspeak 'kaizo.pro' as 'KAlZ0.PR0' to dodge a\n"
    "# literal match, so cover the variants; the URL regex below is the real backstop):\n"
    "name: che4ts\n"
    "name: kaizo\n"
    "name: kalz0\n"
    "regex: (https?://|www\\.|discord(\\.gg)?|t\\.me/|\\.gg/|telegram|join us)\n"
    "# Add your own below - one per line, case-insensitive:\n"
    "# name: free vbucks\n"
    "# name: gtxgaming\n";

// True if the on-disk file's first line is the current version sentinel.
bool configIsCurrent() {
    FILE* f = nullptr;
    if (fopen_s(&f, g_path.c_str(), "r") != 0 || !f) return false;
    char first[128] = {0};
    bool got = fgets(first, sizeof(first), f) != nullptr;
    fclose(f);
    return got && trim(first) == kConfigVersion;
}

// Write defaults if the file is missing OR from an older version. An out-of-date
// file is backed up to <path>.bak first, so a user's edits are never silently lost.
void ensureConfig() {
    bool exists = GetFileAttributesA(g_path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (exists && configIsCurrent()) return;
    if (exists) {
        std::string bak = g_path + ".bak";
        DeleteFileA(bak.c_str());
        MoveFileA(g_path.c_str(), bak.c_str());
        logf("[filter] config out of date -> backed up old to %s, rewriting defaults", bak.c_str());
    }
    if (FILE* f; fopen_s(&f, g_path.c_str(), "w") == 0 && f) {
        fputs(kDefaultConfig, f);
        fclose(f);
        logf("[filter] wrote %s config: %s", kConfigVersion, g_path.c_str());
    }
}

void parseInto(Rules& r) {
    FILE* f = nullptr;
    if (fopen_s(&f, g_path.c_str(), "r") != 0 || !f) {
        logf("[filter] could not open %s; no rules loaded.", g_path.c_str());
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;

        if (starts_with(s, "name:")) {
            std::string v = lower(trim(s.substr(5)));
            if (!v.empty()) r.nameContains.push_back(v);
        } else if (starts_with(s, "regex:")) {
            std::string v = trim(s.substr(6));
            if (v.empty()) continue;
            try {
                r.regexes.emplace_back(v, std::regex::ECMAScript | std::regex::icase);
                r.regexSrc.push_back(v);
            } catch (const std::exception& e) {
                logf("[filter] bad regex '%s' skipped (%s)", v.c_str(), e.what());
            }
        } else if (starts_with(s, "maxperip:")) {
            r.maxPerIP = atoi(trim(s.substr(9)).c_str());
        } else if (starts_with(s, "hidelocked:")) {
            r.hideLocked = atoi(trim(s.substr(11)).c_str()) != 0;
        } else if (starts_with(s, "hideempty:")) {
            r.hideEmpty = atoi(trim(s.substr(10)).c_str()) != 0;
        } else if (starts_with(s, "hideunjoinable:")) {
            r.hideUnjoinable = atoi(trim(s.substr(15)).c_str()) != 0;
        } else {
            logf("[filter] ignoring unrecognized rule: %s", s.c_str());
        }
    }
    fclose(f);
}

} // namespace

void Reload() {
    std::lock_guard<std::mutex> lk(g_mx);
    Rules r;
    parseInto(r);
    g_rules = std::move(r);
    g_ipCount.clear();
    logf("[filter] loaded %zu name rule(s), %zu regex(es); maxPerIP=%d hideLocked=%d hideEmpty=%d hideUnjoinable=%d",
         g_rules.nameContains.size(), g_rules.regexes.size(),
         g_rules.maxPerIP, (int)g_rules.hideLocked, (int)g_rules.hideEmpty, (int)g_rules.hideUnjoinable);
}

void Init(const std::string& configPath, LogFn log) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_path = configPath;
        g_log  = log;
        ensureConfig();
    }
    Reload();
}

std::string ShouldHide(const ServerInfo& s) {
    std::lock_guard<std::mutex> lk(g_mx);

    // ping "---" was assumed to be the ghost/ad signal, but at list-build time the
    // browser shows "---" for EVERY server (pings aren't resolved yet), so this is off
    // by default - the NAME rules below are the reliable signal.
    if (g_rules.hideUnjoinable && s.unjoinable) return "unjoinable (ping ---)";

    const std::string n = lower(s.name);
    for (const auto& sub : g_rules.nameContains)
        if (n.find(sub) != std::string::npos)
            return "name contains '" + sub + "'";

    for (size_t i = 0; i < g_rules.regexes.size(); ++i)
        if (std::regex_search(s.name, g_rules.regexes[i]))
            return "name matches /" + g_rules.regexSrc[i] + "/";

    if (g_rules.hideLocked && s.locked)          return "password-locked";
    if (g_rules.hideEmpty  && s.players == 0)    return "empty";

    if (g_rules.maxPerIP > 0 && !s.ip.empty()) {
        uint64_t now = GetTickCount64();
        if (now - g_lastAddTick > 3000) g_ipCount.clear();   // gap => new refresh
        g_lastAddTick = now;
        int c = ++g_ipCount[s.ip];
        if (c > g_rules.maxPerIP)
            return "too many from IP " + s.ip + " (>" + std::to_string(g_rules.maxPerIP) + ")";
    }
    return "";
}

} // namespace frostmod::serverfilter
