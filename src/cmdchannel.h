// The MXB App -> FrostMod command channel, minus the operating system.
//
// A command reaches us as a small JSON file. On Windows an event announces it; on Linux
// nothing does, because MXB App there is a native process outside the Wine prefix the
// game and this dll run in - it can write a file into our folder and that is the whole
// of its reach. So the file has to be read on a poll as well as on an event, and the
// rules about when a document counts as *new* are what keep the two paths honest:
//
//   - a file already on disk when we load belongs to a previous session (`Seed`)
//   - the same command must not run twice because two paths noticed it
//   - two files whose contents differ must not take turns looking new to each other
//   - the same command sent twice must run twice (MXB App stamps each with `at`)
//
// None of that needs Win32 or a game to check, which is why it lives here rather than in
// frostmod.cpp: tests/command_channel_test.cpp drives it on any machine, the way
// offsets_test.cpp does for the offset tables. frostmod.cpp supplies the two parts that
// really are the operating system's - where the files are, and how to read one.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace frostmod {

// Extract a JSON string field. The file is machine-written with a known flat shape
// ({"bikeId":"..","verb":".."}), so a scan for "key" + the following quoted value is
// sufficient - this is deliberately not a general JSON parser.
inline bool JsonStringField(const std::string& doc, const char* key, std::string& out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t k = doc.find(needle);
    if (k == std::string::npos) return false;
    size_t c = doc.find(':', k + needle.size());
    if (c == std::string::npos) return false;
    size_t q = doc.find('"', c + 1);
    if (q == std::string::npos) return false;
    out.clear();
    for (size_t i = q + 1; i < doc.size(); ++i) {
        char ch = doc[i];
        // A backslash escapes the character after it, so an id may hold a quote or a
        // backslash of its own. (Do not end this comment with a backslash: one at the end
        // of a line splices the next line into the comment, which is what deleted the
        // `return true` below and left every command unread. See the changelog.)
        if (ch == '\\' && i + 1 < doc.size()) { out += doc[++i]; continue; }
        if (ch == '"') return true;
        out += ch;
    }
    return false;   // unterminated
}

// Which command documents are new, and what has already been acted on.
//
// Keyed by path and compared by contents: a file written from outside the prefix carries
// a clock that isn't ours, so its timestamp says nothing we can rely on.
class CommandInbox {
public:
    /// Every command file that exists right now, as (path, contents), most-preferred
    /// first. Files that don't exist are simply absent.
    using Snapshot = std::vector<std::pair<std::string, std::string>>;

    /// Record what is already there without acting on it.
    void Seed(const Snapshot& files) {
        for (const auto& [path, doc] : files) last_[path] = doc;
    }

    /// The documents worth dispatching: ones that appeared, or changed, since we last
    /// looked. Marks them as acted on as it returns them, so a caller can't take one and
    /// forget to say so - and so a dispatch that throws or refuses still doesn't loop.
    Snapshot Fresh(const Snapshot& files) {
        Snapshot fresh;
        for (const auto& [path, doc] : files) {
            auto seen = last_.find(path);
            if (seen != last_.end() && seen->second == doc) continue;
            last_[path] = doc;
            fresh.emplace_back(path, doc);
        }
        return fresh;
    }

    /// Mark one document as acted on. For the event path, which reads a single file
    /// itself rather than taking a snapshot.
    void Note(const std::string& path, const std::string& doc) { last_[path] = doc; }

private:
    std::map<std::string, std::string> last_;
};

}   // namespace frostmod
