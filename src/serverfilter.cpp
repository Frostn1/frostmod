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
// strip one pair of surrounding '...' or "..." quotes (YAML scalar), if present.
std::string unquote(std::string v) {
    if (v.size() >= 2 && (v.front() == '\'' || v.front() == '"') && v.back() == v.front())
        v = v.substr(1, v.size() - 2);
    return v;
}
// drop a trailing " # ..." YAML comment from a scalar value (not from quoted/list text).
std::string stripComment(const std::string& v) {
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i] == '#' && (v[i-1] == ' ' || v[i-1] == '\t')) return trim(v.substr(0, i));
    return v;
}
bool truthy(const std::string& v) {
    std::string t = lower(stripComment(v));
    return t == "true" || t == "1" || t == "yes" || t == "on";
}

// Bump this when the shipped defaults change; the loader auto-rewrites an older
// file (backing the old one up to .bak) so new rules land without a manual delete.
constexpr const char* kConfigVersion = "# frostmod-filter v4";

// YAML. A server is hidden if its name contains any 'names' entry OR matches any
// 'regex' (both case-insensitive). Kept short on purpose.
const char* kDefaultConfig =
    "# frostmod-filter v4\n"
    "# FrostMod server filter - hide spam/ad servers from the online browser.\n"
    "# Hidden if the name contains any 'names' entry or matches any 'regex'.\n"
    "hideUnjoinable: false   # ping '---' - unreliable at list time, keep off\n"
    "hideEmpty: false        # hide 0-player servers (many legit ones are just empty)\n"
    "hideLocked: false       # hide password-locked servers\n"
    "maxPerIP: 0             # 0 = off; else hide servers past N from one IP per refresh\n"
    "names:                  # case-insensitive substrings\n"
    "  - che4ts\n"
    "  - kaizo\n"
    "  - kalz0\n"
    "regex:                  # ECMAScript regex; single-quote to keep backslashes literal\n"
    "  - '(che[a4]ts|k[a4][il1]z[o0]|\\.pr0\\b)'\n";

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

// Minimal YAML: `key: value` scalars, and `key:` followed by `  - item` block lists
// for `names` / `regex`. '#' starts a comment; matching is case-insensitive.
void parseInto(Rules& r) {
    FILE* f = nullptr;
    if (fopen_s(&f, g_path.c_str(), "r") != 0 || !f) {
        logf("[filter] could not open %s; no rules loaded.", g_path.c_str());
        return;
    }
    enum { L_NONE, L_NAMES, L_REGEX } curList = L_NONE;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;

        if (s[0] == '-') {                                   // list item under names/regex
            std::string v = unquote(trim(s.substr(1)));
            if (v.empty()) continue;
            if (curList == L_NAMES) {
                r.nameContains.push_back(lower(v));
            } else if (curList == L_REGEX) {
                try {
                    r.regexes.emplace_back(v, std::regex::ECMAScript | std::regex::icase);
                    r.regexSrc.push_back(v);
                } catch (const std::exception& e) {
                    logf("[filter] bad regex '%s' skipped (%s)", v.c_str(), e.what());
                }
            } else {
                logf("[filter] '- %s' with no list key above it; ignored.", v.c_str());
            }
            continue;
        }

        size_t colon = s.find(':');
        if (colon == std::string::npos) { logf("[filter] ignoring line: %s", s.c_str()); continue; }
        std::string key = lower(trim(s.substr(0, colon)));
        std::string val = trim(s.substr(colon + 1));
        curList = L_NONE;
        if      (key == "names")          curList = L_NAMES;
        else if (key == "regex" || key == "regexes") curList = L_REGEX;
        else if (key == "hideunjoinable") r.hideUnjoinable = truthy(val);
        else if (key == "hideempty")      r.hideEmpty      = truthy(val);
        else if (key == "hidelocked")     r.hideLocked     = truthy(val);
        else if (key == "maxperip")       r.maxPerIP       = atoi(stripComment(val).c_str());
        // Allow an inline scalar too (e.g. `names: foo`) for convenience.
        else logf("[filter] ignoring unknown key: %s", key.c_str());

        if ((curList == L_NAMES || curList == L_REGEX) && !val.empty() && val[0] != '#') {
            std::string v = unquote(val);
            if (curList == L_NAMES) r.nameContains.push_back(lower(v));
            else { try { r.regexes.emplace_back(v, std::regex::ECMAScript | std::regex::icase);
                         r.regexSrc.push_back(v); } catch (...) {} }
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
