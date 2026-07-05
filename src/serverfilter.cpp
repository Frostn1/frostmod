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
    bool hideUnjoinable = true;                // ping "---" ghost/ad servers (default ON)
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

const char* kDefaultConfig =
    "# FrostMod - server browser spam filter (client-side, your view only).\n"
    "# Hide junk / ad \"ghost\" servers from the MX Bikes list.\n"
    "# One rule per line; '#' starts a comment; matching is case-insensitive.\n"
    "#\n"
    "#   name: <text>      hide a server whose NAME contains <text>\n"
    "#   regex: <pattern>  hide a server whose NAME matches this ECMAScript regex\n"
    "#   maxPerIP: <n>     hide servers past <n> from the same IP per refresh (0=off)\n"
    "#   hideLocked: 0|1   hide password-locked servers\n"
    "#   hideEmpty: 0|1    hide servers with 0 players\n"
    "#   hideUnjoinable: 0|1  hide servers whose ping shows '---' (ghost/ad spam)\n"
    "#\n"
    "# --- defaults: hide unjoinable ghost servers + obvious ad names. ---\n"
    "hideUnjoinable: 1\n"
    "regex: (https?://|www\\.|discord(\\.gg)?|t\\.me/|\\.gg/|telegram|join us)\n"
    "maxPerIP: 5\n"
    "hideLocked: 0\n"
    "hideEmpty: 0\n"
    "# Examples you can uncomment:\n"
    "# name: free vbucks\n"
    "# name: [ad]\n";

void writeDefaultIfMissing() {
    if (GetFileAttributesA(g_path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    if (FILE* f; fopen_s(&f, g_path.c_str(), "w") == 0 && f) {
        fputs(kDefaultConfig, f);
        fclose(f);
        logf("[filter] wrote default config: %s", g_path.c_str());
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
        writeDefaultIfMissing();
    }
    Reload();
}

std::string ShouldHide(const ServerInfo& s) {
    std::lock_guard<std::mutex> lk(g_mx);

    // The strongest ghost/ad signal: the server can't actually be joined (ping "---").
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
