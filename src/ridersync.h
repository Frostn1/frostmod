#pragma once

#include "riderlooks.h"
#include "sha256.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ridersync {

enum Part { Rider = 0, Helmet = 1, Boots = 2, kParts = 3 };

inline const char* PartDir(Part p) {
    static const char* dirs[kParts] = {"riders", "helmets", "boots"};
    return p >= Rider && p < kParts ? dirs[p] : "";
}
inline const char* PartName(Part p) {
    static const char* names[kParts] = {"rider", "helmet", "boots"};
    return p >= Rider && p < kParts ? names[p] : "";
}

inline const std::string& ModelId(const frostmod::RiderLook& look, Part p) {
    return look.gear[p >= Rider && p < kParts ? p : Rider];
}
inline void SetModelId(frostmod::RiderLook& look, Part p, std::string id) {
    if (p >= Rider && p < kParts) {
        look.gear[p] = std::move(id);
    }
}

inline std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline bool EqualNoCase(const std::string& a, const std::string& b) {
    return Lower(a) == Lower(b);
}

inline bool IsVisualFile(const std::string& name) {
    if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
        return false;
    const std::string lower = Lower(name);
    if (lower == "gfx.cfg") return true;
    const size_t dot = lower.rfind('.');
    if (dot == std::string::npos) return false;
    const std::string ext = lower.substr(dot);
    return ext == ".edf" || ext == ".hrc" || ext == ".dds" || ext == ".tga" ||
           ext == ".png" || ext == ".jpg" || ext == ".bmp";
}

struct FileEntry { std::string name; uint64_t size; };

inline bool CanonicalFiles(std::vector<FileEntry> in, std::vector<FileEntry>& out) {
    out.clear();
    uint64_t total = 0;
    bool edf = false;
    for (FileEntry& f : in) {
        if (!IsVisualFile(f.name)) continue;
        f.name = Lower(f.name);
        if (f.size > 64ull * 1024 * 1024 || total + f.size > 256ull * 1024 * 1024) {
            out.clear(); return false;
        }
        total += f.size;
        edf = edf || (f.name.size() >= 4 && f.name.substr(f.name.size() - 4) == ".edf");
        out.push_back(std::move(f));
        if (out.size() > 64) { out.clear(); return false; }
    }
    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.name < b.name;
    });
    if (out.empty() || !edf) { out.clear(); return false; }
    return true;
}

class GearHasher {
public:
    void Begin(Part p) {
        sha_.Init();
        static const char tag[] = "frostmod-gear-v1";
        sha_.Update(tag, sizeof(tag));
        const char* part = PartName(p);
        sha_.Update(part, std::char_traits<char>::length(part) + 1);
    }
    void File(const std::string& lowerName, uint64_t size) {
        sha_.Update(lowerName.data(), lowerName.size());
        const unsigned char zero = 0;
        sha_.Update(&zero, 1);
        unsigned char le[8];
        for (int i = 0; i < 8; ++i) le[i] = static_cast<unsigned char>(size >> (i * 8));
        sha_.Update(le, sizeof(le));
    }
    void Update(const void* data, size_t size) { sha_.Update(data, size); }
    std::string Final() { unsigned char digest[32]; sha_.Final(digest); return frostmod::Hex16(digest); }
private:
    frostmod::Sha256 sha_{};
};

class GearIndex {
public:
    void Set(Part p, const std::string& folder, const std::string& id) {
        if (p < Rider || p >= kParts || folder.empty() || folder.size() > 31 ||
            !frostmod::ValidId(id)) return;
        const std::string key = Lower(folder);
        byFolder_[p][key] = id;
        auto it = byId_[p].find(id);
        if (it == byId_[p].end() || key < Lower(it->second)) byId_[p][id] = folder;
    }
    bool Find(Part p, const std::string& id, std::string& folder) const {
        if (p < Rider || p >= kParts) return false;
        auto it = byId_[p].find(id);
        if (it == byId_[p].end()) return false;
        folder = it->second; return true;
    }
    bool IdOf(Part p, const std::string& folder, std::string& id) const {
        if (p < Rider || p >= kParts) return false;
        auto it = byFolder_[p].find(Lower(folder));
        if (it == byFolder_[p].end()) return false;
        id = it->second; return true;
    }
    void Clear() { for (int p = 0; p < kParts; ++p) { byFolder_[p].clear(); byId_[p].clear(); } }
    size_t Size() const { size_t n = 0; for (int p = 0; p < kParts; ++p) n += byFolder_[p].size(); return n; }
private:
    std::map<std::string, std::string> byFolder_[kParts];
    std::map<std::string, std::string> byId_[kParts];
};

struct Override {
    int key = 0;
    std::string folder[kParts];
    bool Any() const { return !folder[Rider].empty() || !folder[Helmet].empty() || !folder[Boots].empty(); }
};
inline bool operator==(const Override& a, const Override& b) {
    return a.key == b.key && a.folder[Rider] == b.folder[Rider] &&
           a.folder[Helmet] == b.folder[Helmet] && a.folder[Boots] == b.folder[Boots];
}
inline bool operator!=(const Override& a, const Override& b) { return !(a == b); }

inline Override Decide(const frostmod::RiderLook* look, int key, const std::string own[kParts],
                       const GearIndex& idx,
                       const std::function<bool(Part, const std::string&)>& installed) {
    Override result; result.key = key;
    if (!look) return result;
    for (int i = 0; i < kParts; ++i) {
        const Part p = static_cast<Part>(i);
        const std::string& id = ModelId(*look, p);
        if (id.empty()) continue;
        std::string ownId;
        if (idx.IdOf(p, own[p], ownId) && ownId == id) continue;
        std::string folder;
        if (idx.Find(p, id, folder) && folder.size() <= 31 && !EqualNoCase(folder, own[p]) &&
            installed(p, folder)) result.folder[p] = folder;
    }
    return result;
}

} // namespace ridersync
