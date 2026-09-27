// Race mode's mod list (frostmod_racemode.txt): which tracks and bikes the game is shown.
//
// MXB App's Auto race mode used to slim the game's mod list by moving folders out of mods/
// and back. This does it without touching the disk: while the game scans mods/tracks and
// mods/bikes, entries that are not on the list are skipped, so the game never sees them -
// and a skipped .pkz is never opened, because the scanner reads archives live as it lists
// them (mxbikes/engine/content.c: there is no persistent .pkz mount).
//
// The file, UTF-8, one path per line, relative to mods/ with forward slashes:
//     tracks/Red Bud
//     bikes/KTM 450.pkz
// A line names a whole mod, folder or .pkz: everything under it is allowed, and every
// folder above it is kept so the scan can get there. Only tracks/ and bikes/ are filtered,
// and each only when the file names something under it - a list of tracks leaves the bikes
// alone. No file, or nothing usable in it, means no filtering. Matching ignores case (the
// game runs on Windows) and treats "X" and "X.pkz" as the same mod.
//
// Pure - no Win32, no game - so tests/racefilter_test.cpp runs it in CI.
#pragma once

#include <string>
#include <set>

namespace frostmod {

class RaceFilter {
public:
    // Normalise a path: lower case, '/' separators, no doubled, leading or trailing ones.
    static std::string Norm(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (char c : in) {
            if (c == '\\') c = '/';
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c == '/' && (out.empty() || out.back() == '/')) continue;
            out.push_back(c);
        }
        while (!out.empty() && (out.back() == '/' || out.back() == ' ' || out.back() == '\t'))
            out.pop_back();
        return out;
    }
    static std::string Stem(const std::string& p) {        // "x.pkz" -> "x"
        return p.size() > 4 && p.compare(p.size() - 4, 4, ".pkz") == 0 ? p.substr(0, p.size() - 4) : p;
    }

    // Parse the file's text. Returns how many lines were taken.
    int Load(const std::string& text) {
        allowed_.clear(); ancestors_.clear(); roots_.clear();
        size_t i = 0;
        if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;   // UTF-8 BOM
        int n = 0;
        while (i < text.size()) {
            size_t e = text.find_first_of("\r\n", i);
            if (e == std::string::npos) e = text.size();
            std::string line = text.substr(i, e - i);
            i = e + 1;
            size_t s = line.find_first_not_of(" \t");
            if (s == std::string::npos || line[s] == '#') continue;
            const std::string p = Stem(Norm(line.substr(s)));
            const size_t slash = p.find('/');
            if (slash == std::string::npos) continue;           // a bare "tracks" names nothing
            const std::string root = p.substr(0, slash);
            if (root != "tracks" && root != "bikes") continue;
            if (p.find("/../") != std::string::npos || p.compare(p.size() - 3, 3, "/..") == 0) continue;
            roots_.insert(root);
            allowed_.insert(p);
            for (size_t k = p.find('/'); k != std::string::npos; k = p.find('/', k + 1))
                ancestors_.insert(p.substr(0, k));
            ++n;
        }
        return n;
    }

    bool Active() const { return !roots_.empty(); }

    // The scanned directory relative to the mods folder, normalised, if the filter has a
    // say over it; "" if not (outside mods, a root the file does not mention, or inside a
    // mod the file allows - in which case everything there passes).
    std::string Scope(const std::string& dir, const std::string& modsDir) const {
        if (!Active()) return std::string();
        const std::string d = Norm(dir);
        std::string rel;
        const std::string m = Norm(modsDir);
        if (!m.empty() && d.size() > m.size() && d.compare(0, m.size(), m) == 0 && d[m.size()] == '/') {
            rel = d.substr(m.size() + 1);
        } else if (m.empty()) {
            const size_t k = d.rfind("/mods/");                // mods folder not known yet
            if (k == std::string::npos) return std::string();
            rel = d.substr(k + 6);
        } else {
            return std::string();
        }
        const size_t slash = rel.find('/');
        const std::string root = rel.substr(0, slash);
        if (!roots_.count(root)) return std::string();
        if (InsideAllowed(rel)) return std::string();
        return rel;
    }

    // May entry `name`, found while scanning `scope` (from Scope), be shown?
    bool Allows(const std::string& scope, const std::string& name) const {
        if (name == "." || name == "..") return true;
        const std::string child = Stem(scope + "/" + Norm(name));
        return allowed_.count(child) || ancestors_.count(child);
    }

private:
    bool InsideAllowed(const std::string& rel) const {
        for (size_t k = rel.find('/'); ; k = rel.find('/', k + 1)) {
            const std::string pre = Stem(k == std::string::npos ? rel : rel.substr(0, k));
            if (allowed_.count(pre)) return true;
            if (k == std::string::npos) return false;
        }
    }

    std::set<std::string> allowed_, ancestors_, roots_;
};

}  // namespace frostmod
