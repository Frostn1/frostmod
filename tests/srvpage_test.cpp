// The in-game Servers page (src/srvpage.h): the game's .fnt and .tga readers, MXB App's
// servers.json, the address check that keeps a server's address to one argument, and the
// layout. Synthetic data always; the player's real ui.pkz and servers.json too when present.

#include "../src/srvpage.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

static int g_failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            ++g_failures;                                    \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n  (%s)\n", #cond);                \
        }                                                    \
    } while (0)

using namespace srvpage;

static void Addresses() {
    CHECK(SafeAddress("101.56.183.254:1329") == "101.56.183.254:1329", "a plain address");
    CHECK(SafeAddress("mx.example.com:54210") == "mx.example.com:54210", "a host name");
    CHECK(SafeAddress("1.2.3.4") == "", "no port");
    CHECK(SafeAddress("1.2.3.4:0") == "" && SafeAddress("1.2.3.4:65536") == "", "port out of range");
    CHECK(SafeAddress("-x:54210") == "", "a leading dash would be a flag");
    CHECK(SafeAddress("1.2.3.4:54210 -foo") == "", "a space would be a second argument");
    CHECK(SafeAddress("a\"b:1") == "" && SafeAddress("a;b:1") == "", "quotes and separators");
}

static void Servers() {
    const std::string j = R"([
      {"name": "lele \u00e9 racing", "address": "101.56.183.254:1329", "joinable": true, "players": 2, "maxPlayers": 3,
       "pingMs": null, "passworded": true, "track": "club", "categories": ["MX2-2T OEM", "MX1"], "bikes": [],
       "extra": {"a": [1, 2, {"b": "c"}]}, "lastSeen": 1792429828089},
      {"name": "bad", "address": "1.2.3.4:99999", "players": 9},
      {"name": "empty", "address": "5.6.7.8:54210", "joinable": false, "players": 0, "maxPlayers": 20, "categories": []}
    ])";
    const std::vector<Server> s = ReadServers(j);
    CHECK(s.size() == 2, "the one with a bad address is left out: %zu", s.size());
    if (s.size() == 2) {
        CHECK(s[0].name == "lele ? racing", "non-ASCII is one '?': [%s]", s[0].name.c_str());
        CHECK(s[0].players == 2 && s[0].max_players == 3 && s[0].passworded && s[0].joinable, "the numbers and flags");
        CHECK(s[0].track == "club" && s[0].category == "MX2-2T OEM", "track and the first category");
        CHECK(!s[1].joinable && s[1].category.empty(), "not joinable, no category");
        CHECK(Before(s[0], s[1]), "joinable and busier first");
    }
    CHECK(ReadServers("").empty() && ReadServers("[]").empty() && ReadServers("{").empty(), "nothing to read");
}

static void Messages() {
    bool    ok = false;
    const J m  = ParseJson(R"({"re":"friends","note":"","friends":[
        {"name":"Ann","online":true,"server":{"name":"MX Social","address":"9.9.9.9:9","track":"farm14","players":5,
         "maxPlayers":20,"missing":["track"]}},
        {"name":"Bob","online":false}]})",
                          &ok);
    CHECK(ok && m["re"].str() == "friends", "a friends message parses");
    const std::vector<Friend> f = ReadFriends(m["friends"]);
    CHECK(f.size() == 2, "two friends: %zu", f.size());
    if (f.size() == 2) {
        CHECK(f[0].online && f[0].has_server && f[0].on.address == "9.9.9.9:9", "Ann is on MX Social");
        CHECK(f[0].on.missing.size() == 1 && f[0].on.missing[0] == "track", "and lacks its track");
        CHECK(!f[1].online && !f[1].has_server, "Bob is offline");
    }
    const J j = ParseJson(R"({"re":"join","state":"installing","text":"Downloading Farm 14","pct":40})");
    CHECK(j["state"].str() == "installing" && j["pct"].num() == 40.0, "join progress");
    CHECK(ParseJson("{\"a\":", &ok).t == J::NUL && !ok, "a cut-off line is refused");
    CHECK(Quote("say \"hi\"\\") == "\"say \\\"hi\\\"\\\\\"", "quoting: %s", Quote("say \"hi\"\\").c_str());
    const J back = ParseJson("{\"code\":" + Quote("AB\"C") + "}");
    CHECK(back["code"].str() == "AB\"C", "what Quote writes reads back");
}

static void Layout() {
    CHECK(RowAt(0.5f, kListY0 + 0.5f * kRowH, 10, 100) == 10, "the first row is `top`");
    CHECK(RowAt(0.5f, kListY0 + 2.5f * kRowH, 0, 2) == -1, "past the end");
    CHECK(RowAt(0.1f, kListY0 + 0.5f * kRowH, 0, 100) == -1, "over the game's menu column");
    CHECK(Inside(kButton, 0.1f, 0.43f) && !Inside(kButton, 0.1f, 0.47f), "the Servers button");
}

static std::string Env(const char* k) {
    char*  v = nullptr;
    size_t n = 0;
    std::string out;
    if (_dupenv_s(&v, &n, k) == 0 && v) out = v;
    free(v);
    return out;
}

static void RealFiles() {
    const std::string pkz = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\MX Bikes\\ui.pkz";
    const std::vector<uint8_t> fnt = ReadPkz(pkz, "ui/main.fnt");
    if (fnt.empty()) {
        std::printf("(no ui.pkz here: the real-file checks are skipped)\n");
    } else {
        Font f;
        CHECK(ReadFnt(fnt, f) && f.ok(), "main.fnt reads");
        CHECK(f.g['A'].ok && f.g['A'].w > 0 && f.cell > 0, "it has an A: w %d cell %d", f.g['A'].w, f.cell);
        CHECK(TextWidth(f, "Servers", 48.0f) > 100.0f, "Servers is %0.f px wide at 48", TextWidth(f, "Servers", 48.0f));
        Font d;
        CHECK(ReadFnt(ReadPkz(pkz, "ui/data.fnt"), d) && d.ok(), "data.fnt reads (it has spacing tweaks)");
        Image im;
        CHECK(ReadTga(ReadPkz(pkz, "ui/main2.tga"), im) && im.w == 340 && im.h == 40, "main2.tga: %dx%d", im.w, im.h);
        const std::string fit = Fit(f, "a very long server name that will not fit", 48.0f, 300.0f);
        CHECK(TextWidth(f, fit, 48.0f) <= 300.0f && fit.size() > 3, "cut to fit: [%s]", fit.c_str());
    }
    const std::string path = Env("APPDATA") + "\\com.frost.mxbikes\\servers.json";
    std::ifstream     in(path, std::ios::binary);
    if (!in) return void(std::printf("(no servers.json here)\n"));
    std::stringstream ss;
    ss << in.rdbuf();
    const std::vector<Server> s = ReadServers(ss.str());
    CHECK(s.size() > 100, "MXB App's book reads: %zu servers", s.size());
    std::printf("real files: %zu servers\n", s.size());
}

int main() {
    Addresses();
    Servers();
    Messages();
    Layout();
    RealFiles();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("srvpage: all checks passed\n");
    return 0;
}
