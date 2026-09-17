// What a crash report says, asserted without a crash.
//
// The report is the only output of FrostMod that is read exactly once, by someone who
// cannot ask for a second one: the game is gone, the player has moved on, and whatever
// the log holds is the whole of the evidence. So the parts that decide what it holds -
// the ring that keeps the last N things that happened, and the layout that turns the ring
// and the session context into lines - are pure, and this runs them.
//
// Pure constants and <cstdio>, like tests/offsets_test.cpp: no Win32, no MSVC, no game, so
// CI and a Mac both execute it.

#include "../src/crashreport.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace frostmod::crash;

static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n  (%s)\n", #cond);                                   \
        }                                                                       \
    } while (0)

// ---- a sink that keeps the lines, so a test can read the report back --------
static void Collect(void* user, const char* line) {
    ((std::vector<std::string>*)user)->push_back(line);
}

static bool Mentions(const std::vector<std::string>& lines, const char* needle) {
    for (const auto& l : lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

static void TrailKeepsTheNewest() {
    Trail t;
    CHECK(t.Count() == 0, "a fresh trail has nothing on it");
    CHECK(t.TotalAdded() == 0, "and has counted nothing");

    // Under capacity: everything is there, oldest first.
    for (int i = 0; i < 5; ++i) {
        char s[32];
        std::snprintf(s, sizeof(s), "note %d", i);
        t.Add((unsigned long long)(i * 100), s);
    }
    CHECK(t.Count() == 5, "five notes, five readable - got %d", t.Count());
    CHECK(std::strcmp(t.At(0).text, "note 0") == 0, "At(0) is the oldest, got '%s'", t.At(0).text);
    CHECK(std::strcmp(t.At(4).text, "note 4") == 0, "At(n-1) is the newest, got '%s'", t.At(4).text);
    CHECK(t.At(2).ms == 200, "the timestamp rides along, got %llu", t.At(2).ms);

    // Past capacity: the oldest fall off, the order still reads forwards. This is the
    // case that matters - a crash comes after hours of play, never after five events.
    Trail full;
    const int total = kMaxCrumbs + 8;
    for (int i = 0; i < total; ++i) {
        char s[32];
        std::snprintf(s, sizeof(s), "note %d", i);
        full.Add((unsigned long long)i, s);
    }
    CHECK(full.Count() == kMaxCrumbs, "the ring holds kMaxCrumbs, got %d", full.Count());
    CHECK(full.TotalAdded() == (unsigned)total,
          "but remembers how many went through it, got %u", full.TotalAdded());
    char expectOldest[32], expectNewest[32];
    std::snprintf(expectOldest, sizeof(expectOldest), "note %d", total - kMaxCrumbs);
    std::snprintf(expectNewest, sizeof(expectNewest), "note %d", total - 1);
    CHECK(std::strcmp(full.At(0).text, expectOldest) == 0,
          "the oldest survivor is '%s', got '%s'", expectOldest, full.At(0).text);
    CHECK(std::strcmp(full.At(kMaxCrumbs - 1).text, expectNewest) == 0,
          "the newest is '%s', got '%s'", expectNewest, full.At(kMaxCrumbs - 1).text);

    // Out of range is empty, not a fault: the only caller is already inside a crash.
    CHECK(full.At(-1).text[0] == 0, "At(-1) is empty");
    CHECK(full.At(kMaxCrumbs).text[0] == 0, "At(past the end) is empty");

    // Longer than a crumb: truncated, never overrun.
    Trail cut;
    std::string huge(kCrumbLen * 2, 'x');
    cut.Add(0, huge.c_str());
    CHECK((int)std::strlen(cut.At(0).text) == kCrumbLen - 1,
          "a long note is truncated to the slot, got %d", (int)std::strlen(cut.At(0).text));
}

static void ReportSaysWhereTheyWere() {
    Context ctx;
    ctx.where.store((int)Where::OnTrack);
    ctx.riders.store(17);
    ctx.lastDrawMs.store(9'000);
    ctx.reloads.store(2);
    ctx.lastReloadMs.store(4'000);
    ctx.SetTrack("Northgate Raceway");
    ctx.SetServer("MXB App Public");
    ctx.SetRider("Frost");

    Trail t;
    t.Add(1'000, "event opened: track='Northgate Raceway'");
    t.Add(8'500, "rider #14 joined (17 in the session)");

    std::vector<std::string> out;
    WriteContext(ctx, t, /*nowMs=*/10'000, &Collect, &out);

    CHECK(Mentions(out, "on track"), "the report says where they were");
    CHECK(Mentions(out, "riders=17"), "and how many were in there with them");
    CHECK(Mentions(out, "Northgate Raceway"), "and which track");
    CHECK(Mentions(out, "MXB App Public"), "and which server");
    CHECK(Mentions(out, "last frame drawn 1000ms ago"), "and how long since a frame");
    CHECK(Mentions(out, "2 content reload(s)"), "and that reloads had run");
    CHECK(Mentions(out, "the last 6000ms ago"), "and how long before the crash");
    CHECK(Mentions(out, "rider #14 joined"), "the trail is in the report");
    CHECK(Mentions(out, "-1500ms  rider #14 joined (17 in the session)"),
          "each note carries how long before the crash it happened");

    // Oldest first: a report is read top to bottom as a sequence of events.
    int iEvent = -1, iJoin = -1;
    for (int i = 0; i < (int)out.size(); ++i) {
        if (out[i].find("event opened") != std::string::npos) iEvent = i;
        if (out[i].find("rider #14 joined") != std::string::npos) iJoin = i;
    }
    CHECK(iEvent >= 0 && iJoin > iEvent, "the trail reads in the order things happened");
}

// The injected copy is never called back with Draw's state, so on its own it would file a
// rider mid-lap as "in the menus". Given the session it mirrors from the plugin copy, it
// says what it does know and admits the rest.
static void ASessionWithoutFramesIsNotTheMenus() {
    Context ctx;
    ctx.inSession.store(true);
    ctx.SetTrack("Northgate Raceway");
    ctx.riders.store(22);
    Trail t;

    std::vector<std::string> out;
    WriteContext(ctx, t, /*nowMs=*/1'000, &Collect, &out);
    CHECK(Mentions(out, "in a session"), "a live session is not reported as the menus");
    CHECK(!Mentions(out, "in the menus"), "and not both at once");
    CHECK(Mentions(out, "this copy sees no frames"), "the limit is stated, not papered over");
    CHECK(Mentions(out, "riders=22"), "the mirrored grid is still in the report");
}

static void ReportIsHonestWhenItKnowsNothing() {
    Context ctx;   // untouched: injected into a game that never reached a track
    Trail t;
    std::vector<std::string> out;
    WriteContext(ctx, t, /*nowMs=*/5'000, &Collect, &out);

    CHECK(Mentions(out, "in the menus"), "an unknown state says so rather than guessing");
    CHECK(Mentions(out, "riders=n/a"), "no race means no rider count, not zero");
    CHECK(Mentions(out, "<none>"), "an unset track/server reads as none");
    CHECK(Mentions(out, "no content reload has run"), "and no reload is stated, not implied");
    CHECK(Mentions(out, "nothing on the trail"), "an empty trail is called empty");
    CHECK(!Mentions(out, "last frame drawn"), "with no frame drawn there is no frame time");
}

static void ReportSaysWhatItDropped() {
    Context ctx;
    Trail t;
    for (int i = 0; i < kMaxCrumbs + 10; ++i) t.Add((unsigned long long)i, "something happened");

    std::vector<std::string> out;
    WriteContext(ctx, t, /*nowMs=*/100'000, &Collect, &out);
    char expect[64];
    std::snprintf(expect, sizeof(expect), "the last %d of %d", kMaxCrumbs, kMaxCrumbs + 10);
    CHECK(Mentions(out, expect),
          "a wrapped trail says how many it dropped ('%s')", expect);
}

// A clock that has gone backwards (a report written a tick before the stamp it is
// comparing against) must not print a nonsense duration - it underflows to something
// like 18446744073709551615ms, which reads as a bug in the game rather than in us.
static void ClockSkewDoesNotPrintNonsense() {
    Context ctx;
    ctx.lastDrawMs.store(5'000);
    ctx.reloads.store(1);
    ctx.lastReloadMs.store(5'000);
    Trail t;
    t.Add(5'000, "a note from the future");

    std::vector<std::string> out;
    WriteContext(ctx, t, /*nowMs=*/4'000, &Collect, &out);
    for (const auto& l : out)
        CHECK(l.find("1844674407") == std::string::npos,
              "no underflowed duration in: %s", l.c_str());
}

// ---- the JSON sidecar -------------------------------------------------------
// This one is read by a machine, not a person, so the bar is different: it has to stay
// parseable whatever the game hands us. Rider names and server names come off a server and
// are attacker-controlled text.

static std::string Joined(const std::vector<std::string>& lines) {
    std::string all;
    for (const auto& l : lines) { all += l; all += "\n"; }
    return all;
}

// A structural check, not a parser: quotes open and close, braces and brackets balance,
// and nothing raw and unescaped is sitting inside a string.
static bool LooksLikeJson(const std::string& text) {
    int braces = 0, brackets = 0;
    bool inString = false, escaped = false;
    for (char c : text) {
        if (inString) {
            if (escaped) { escaped = false; continue; }
            if (c == '\\') { escaped = true; continue; }
            if (c == '"') { inString = false; continue; }
            if ((unsigned char)c < 0x20) return false;   // raw control byte in a string
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{') ++braces;
        else if (c == '}') --braces;
        else if (c == '[') ++brackets;
        else if (c == ']') --brackets;
        if (braces < 0 || brackets < 0) return false;
    }
    return !inString && braces == 0 && brackets == 0;
}

static void EscapeKeepsItParseable() {
    char out[128];
    Escape("plain", out, sizeof(out));
    CHECK(std::strcmp(out, "plain") == 0, "plain text is untouched, got '%s'", out);

    Escape("say \"hi\"", out, sizeof(out));
    CHECK(std::strcmp(out, "say \\\"hi\\\"") == 0, "quotes are escaped, got '%s'", out);

    Escape("C:\\mods", out, sizeof(out));
    CHECK(std::strcmp(out, "C:\\\\mods") == 0, "backslashes are escaped, got '%s'", out);

    Escape("two\nlines", out, sizeof(out));
    CHECK(std::strcmp(out, "two\\nlines") == 0, "newlines are escaped, got '%s'", out);

    char bell[] = {'a', 0x07, 'b', 0};
    Escape(bell, out, sizeof(out));
    CHECK(std::strcmp(out, "a\\u0007b") == 0, "control bytes go out as \\u, got '%s'", out);

    // Truncation must not cut an escape in half, which would leave a dangling backslash
    // and take the rest of the document with it.
    char tiny[6];
    Escape("\"\"\"\"", tiny, sizeof(tiny));
    CHECK(std::strlen(tiny) % 2 == 0, "a truncated escape is dropped whole, got '%s'", tiny);
    CHECK(LooksLikeJson(std::string("\"") + tiny + "\""), "and what is left still parses");
}

static void SidecarCarriesTheFacts() {
    Fault f;
    f.kind = "access violation";
    f.code = 0xC0000005;
    f.site = "mxbikes.exe+0x11D753";
    f.access = "reading";
    f.target = 0x10;
    f.haveTarget = true;
    f.version = "0.29.0";
    f.game = "mxbikes.exe";
    f.whenUtc = "2026-09-16T13:59:29Z";
    f.dumpFile = "frostmod-crash-20260916-145929.dmp";
    f.uptimeMs = 2'400'000;

    Context ctx;
    ctx.where.store((int)Where::OnTrack);
    ctx.inSession.store(true);
    ctx.riders.store(17);
    ctx.lastDrawMs.store(9'000);
    ctx.SetTrack("Northgate Raceway");
    // The name a server handed us, containing the two characters that end a JSON document.
    ctx.SetServer("the \"best\" server\\");
    ctx.SetRider("Frost");

    Trail t;
    t.Add(8'500, "rider #14 joined (17 in the session)");
    const char* frames[] = { "mxbikes.exe+0x11D753", "mxbikes.exe+0x12782D", "frostmod.dll+0x9A1" };

    std::vector<std::string> out;
    WriteJson(f, ctx, t, frames, 3, /*nowMs=*/10'000, &Collect, &out);
    const std::string all = Joined(out);

    CHECK(LooksLikeJson(all), "the sidecar is structurally JSON");
    CHECK(all.find("\"site\": \"mxbikes.exe+0x11D753\"") != std::string::npos,
          "the crash site is the key everything groups by, and it is in there");
    CHECK(all.find("\"code\": \"0xC0000005\"") != std::string::npos, "the exception code is there");
    CHECK(all.find("\"target\": \"0x0000000000000010\"") != std::string::npos,
          "and the address it was refused at");
    CHECK(all.find("\"mxbikes.exe+0x12782D\"") != std::string::npos, "the caller is in the frames");
    CHECK(all.find("\"riders\": 17") != std::string::npos, "the grid size travels with it");
    CHECK(all.find("\"sinceFrameMs\": 1000") != std::string::npos, "so does the time since a frame");
    CHECK(all.find("rider #14 joined") != std::string::npos, "and the trail");
    CHECK(all.find("the \\\"best\\\" server\\\\") != std::string::npos,
          "a hostile server name is escaped rather than dropped");
}

// Absent and zero are different answers, and a dashboard that cannot tell them apart will
// draw conclusions from the wrong one.
static void SidecarSaysNullWhenItDoesNotKnow() {
    Fault f;
    f.kind = "unhandled exception";
    f.site = "mxbikes.exe+0x1";
    Context ctx;    // nothing known
    Trail t;
    std::vector<std::string> out;
    WriteJson(f, ctx, t, nullptr, 0, /*nowMs=*/1'000, &Collect, &out);
    const std::string all = Joined(out);

    CHECK(LooksLikeJson(all), "an empty report is still JSON");
    CHECK(all.find("\"target\": null") != std::string::npos, "no faulting address means null");
    CHECK(all.find("\"riders\": null") != std::string::npos, "no grid means null, not 0");
    CHECK(all.find("\"sinceFrameMs\": null") != std::string::npos, "no frame drawn means null");
    CHECK(all.find("\"sinceReloadMs\": null") != std::string::npos, "no reload means null");
    CHECK(all.find("\"frames\": [") != std::string::npos, "an empty frame list is still a list");
}

int main() {
    EscapeKeepsItParseable();
    SidecarCarriesTheFacts();
    SidecarSaysNullWhenItDoesNotKnow();
    TrailKeepsTheNewest();
    ReportSaysWhereTheyWere();
    ASessionWithoutFramesIsNotTheMenus();
    ReportIsHonestWhenItKnowsNothing();
    ReportSaysWhatItDropped();
    ClockSkewDoesNotPrintNonsense();

    if (g_failures) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("crashreport: all checks passed\n");
    return 0;
}
