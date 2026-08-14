// The rules the MXB App command channel runs on (src/cmdchannel.h).
//
// This exists because the channel now has two mouths and one stomach. On Windows a
// command arrives as a file plus an event; on Linux MXB App is a native process outside
// the Wine prefix and can only write the file, so the same file is also read on a poll.
// Everything that can go wrong there is quiet: a reload that fires twice, one that never
// fires, yesterday's command running at start-up, or two files taking turns looking new
// and reloading the game forever.
//
// Like offsets_test.cpp this is pure - no Win32, no game, no filesystem - so CI can
// actually run it. The paths below are only ever map keys; nothing opens them.

#include "../src/cmdchannel.h"

#include <cstdio>
#include <string>

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

using frostmod::CommandInbox;

static const char* kTemp = "C:\\users\\steamuser\\Temp\\frostmod_cmd.json";
static const char* kBeside = "Z:\\home\\rider\\.local\\share\\com.frost.mxbikes\\frostmod\\frostmod_cmd.json";

// A command as MXB App writes it: verb, bike id, and the stamp that makes the same
// command twice two different documents.
static std::string cmd(const char* verb, const char* at) {
    return std::string("{\"at\":\"") + at + "\",\"bikeId\":\"\",\"verb\":\"" + verb + "\"}";
}

static void a_file_already_on_disk_is_never_run() {
    // The reload MXB App sent during yesterday's session is still sitting there. Loading
    // the dll must not act on it.
    CommandInbox inbox;
    inbox.Seed({{kTemp, cmd("reload_mods", "1")}});
    CHECK(inbox.Fresh({{kTemp, cmd("reload_mods", "1")}}).empty(),
          "a seeded document was dispatched at load");
}

static void a_file_that_appears_after_load_is_run_once() {
    CommandInbox inbox;
    inbox.Seed({});   // nothing on disk when the game started

    auto first = inbox.Fresh({{kTemp, cmd("reload_mods", "1")}});
    CHECK(first.size() == 1, "a command written after load should dispatch");
    CHECK(first.empty() || first[0].first == std::string(kTemp), "dispatched from the wrong path");

    // The poll runs several times a second; the file is still there each time.
    CHECK(inbox.Fresh({{kTemp, cmd("reload_mods", "1")}}).empty(),
          "the same document dispatched twice");
    CHECK(inbox.Fresh({{kTemp, cmd("reload_mods", "1")}}).empty(),
          "the same document dispatched a third time");
}

static void the_same_command_sent_again_runs_again() {
    // Pressing Reload twice sends the same verb twice, and both must land. That is what
    // the `at` stamp is for - without it the second press is byte-identical to the first.
    CommandInbox inbox;
    inbox.Seed({});
    CHECK(inbox.Fresh({{kTemp, cmd("reload_mods", "1")}}).size() == 1, "first press ignored");
    CHECK(inbox.Fresh({{kTemp, cmd("reload_mods", "2")}}).size() == 1, "second press ignored");
}

static void the_event_path_and_the_poll_do_not_double_up() {
    // Windows: the app writes the file and pulses the event, the event handler dispatches
    // it and says so, and the poll that follows must find nothing to do.
    CommandInbox inbox;
    inbox.Seed({});
    inbox.Note(kTemp, cmd("refresh_bike_model", "1"));
    CHECK(inbox.Fresh({{kTemp, cmd("refresh_bike_model", "1")}}).empty(),
          "the poll re-ran a command the event had already handled");
}

static void two_files_do_not_take_turns_looking_new() {
    // A stale %TEMP% command from a Windows session, next to the folder file a Linux app
    // writes. Tracked per path, so neither makes the other look new; tracked as one
    // string, this loops forever and reloads the game on every poll.
    CommandInbox inbox;
    inbox.Seed({});
    auto both = inbox.Fresh({{kTemp, cmd("reload_mods", "1")}, {kBeside, cmd("reload_mods", "2")}});
    CHECK(both.size() == 2, "both new files should dispatch once each");

    for (int poll = 0; poll < 5; ++poll) {
        CHECK(inbox.Fresh({{kTemp, cmd("reload_mods", "1")}, {kBeside, cmd("reload_mods", "2")}}).empty(),
              "two unchanged files kept re-dispatching each other (poll %d)", poll);
    }
}

static void a_document_is_marked_seen_even_if_acting_on_it_goes_nowhere() {
    // FrostMod refuses plenty of commands - an unknown verb, a verb it hasn't built yet.
    // A refusal must still count as handled, or the poll re-offers it five times a second
    // and fills the log with it.
    CommandInbox inbox;
    inbox.Seed({});
    CHECK(inbox.Fresh({{kTemp, cmd("swap_bike", "1")}}).size() == 1, "first sight should dispatch");
    CHECK(inbox.Fresh({{kTemp, cmd("swap_bike", "1")}}).empty(),
          "a refused command was offered again");
}

static void the_field_reader_handles_what_mxb_app_writes() {
    std::string verb, bike;
    std::string doc = "{\"at\":\"17\",\"bikeId\":\"MX1OEM_1996_Honda_CR250\",\"verb\":\"refresh_bike_model\"}";
    CHECK(frostmod::JsonStringField(doc, "verb", verb) && verb == "refresh_bike_model",
          "verb not read back");
    CHECK(frostmod::JsonStringField(doc, "bikeId", bike) && bike == "MX1OEM_1996_Honda_CR250",
          "bikeId not read back");

    // Ids are folder names, so quotes and backslashes have to survive the round trip.
    std::string escaped = "{\"bikeId\":\"a\\\"b\\\\c\",\"verb\":\"swap_bike\"}";
    CHECK(frostmod::JsonStringField(escaped, "bikeId", bike) && bike == "a\"b\\c",
          "escapes not unescaped: got '%s'", bike.c_str());

    // A truncated write (the app died mid-file) names no verb, and must not read as one.
    std::string cut = "{\"verb\":\"reload_mo";
    CHECK(!frostmod::JsonStringField(cut, "verb", verb), "an unterminated value was accepted");
    CHECK(!frostmod::JsonStringField("{}", "verb", verb), "an empty document named a verb");
}

int main() {
    a_file_already_on_disk_is_never_run();
    a_file_that_appears_after_load_is_run_once();
    the_same_command_sent_again_runs_again();
    the_event_path_and_the_poll_do_not_double_up();
    two_files_do_not_take_turns_looking_new();
    a_document_is_marked_seen_even_if_acting_on_it_goes_nowhere();
    the_field_reader_handles_what_mxb_app_writes();

    if (g_failures) {
        std::printf("\n%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("command channel: all checks passed.\n");
    return 0;
}
