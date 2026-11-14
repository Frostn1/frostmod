#pragma once

#include "sha256.h"

#include <algorithm>
#include <cstddef>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace frostmod::bikesync {

inline std::string Lower(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

inline bool IsVisualFile(const std::string& name) {
    const std::string lower = Lower(name);
    return lower == "model.edf" || lower == "model_s.edf" || lower == "gfx.cfg" ||
           (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".hrc") == 0);
}

inline std::vector<std::string> CanonicalVisualOrder(const std::vector<std::string>& files) {
    std::vector<std::string> result;
    for (const std::string& file : files)
        if (IsVisualFile(file)) result.push_back(Lower(file));
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

// The id stream is: lower(bike) 0x00, then for each visual file in
// CanonicalVisualOrder: lower(name) 0x00 u64le(length) bytes.
class IdBuilder {
public:
    void Begin(const std::string& bike) {
        sha_.Init();
        begun_ = true;
        const std::string lower = Lower(bike);
        sha_.Update(lower.data(), lower.size());
        const unsigned char zero = 0;
        sha_.Update(&zero, 1);
    }

    void File(const std::string& lowerName, uint64_t size) {
        const std::string name = Lower(lowerName);
        sha_.Update(name.data(), name.size());
        const unsigned char zero = 0;
        sha_.Update(&zero, 1);
        unsigned char le[8];
        for (int i = 0; i < 8; ++i) le[i] = static_cast<unsigned char>(size >> (i * 8));
        sha_.Update(le, sizeof(le));
    }

    void Bytes(const void* p, size_t n) { sha_.Update(p, n); }

    std::string Finish() {
        unsigned char digest[32];
        if (!begun_) sha_.Init();
        sha_.Final(digest);
        begun_ = false;
        return frostmod::Hex16(digest);
    }

private:
    frostmod::Sha256 sha_{};
    bool begun_ = false;
};

inline std::string ShadowName(const std::string& bike, const std::string& id) {
    return bike + "~" + id.substr(0, 8);
}

inline bool LooksLikeShadow(const std::string& name) {
    if (name.size() < 9 || name[name.size() - 9] != '~') return false;
    for (size_t i = name.size() - 8; i < name.size(); ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f')))
            return false;
    return true;
}

inline std::string RealBikeOfShadow(const std::string& name) {
    return LooksLikeShadow(name) ? name.substr(0, name.size() - 9) : std::string();
}

inline bool ValidBikeName(const std::string& bike) {
    if (bike.empty() || bike == "." || bike == "..") return false;
    for (unsigned char c : bike)
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            return false;
    return true;
}

inline std::string BuildMarker(const std::string& bike, const std::string& id) {
    return "frostmod-modelsync 1\n" + bike + "\n" + id + "\n";
}

inline bool ParseMarker(const std::string& text, std::string& bike, std::string& id) {
    std::vector<std::string> lines;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    if (lines.size() == 4 && lines.back().empty()) lines.pop_back();
    if (lines.size() != 3 || lines[0] != "frostmod-modelsync 1" ||
        !ValidBikeName(lines[1]) || !frostmod::ValidId(lines[2]))
        return false;
    bike = lines[1];
    id = lines[2];
    return true;
}

struct ShadowPlan {
    bool ok = false;
    std::string why;
    std::vector<std::string> fromRoot, fromVariant;
};

inline ShadowPlan PlanShadow(const std::vector<std::string>& rootFiles,
                             const std::vector<std::string>& variantFiles) {
    ShadowPlan plan;
    for (const std::string& file : rootFiles) {
        const std::string lower = Lower(file);
        if (!IsVisualFile(file) && lower != "_active.txt" && lower != "_modelsync.txt")
            plan.fromRoot.push_back(file);
    }
    bool model = false;
    for (const std::string& file : variantFiles) {
        if (!IsVisualFile(file)) continue;
        plan.fromVariant.push_back(file);
        if (Lower(file) == "model.edf") model = true;
    }
    if (!model) plan.why = "variant has no model.edf";
    else if (plan.fromRoot.empty()) plan.why = "bike root has no base files";
    else plan.ok = true;
    return plan;
}

class ShadowIndex {
public:
    void Set(const std::string& bike, const std::string& id, const std::string& shadowName) {
        values_[{Lower(bike), id}] = shadowName;
    }
    bool Find(const std::string& bike, const std::string& id, std::string& out) const {
        const auto it = values_.find({Lower(bike), id});
        if (it == values_.end()) return false;
        out = it->second;
        return true;
    }
    void Clear() { values_.clear(); }
    size_t Size() const { return values_.size(); }

private:
    std::map<std::pair<std::string, std::string>, std::string> values_;
};

inline std::string ChooseShadow(const std::string& swapId, const std::string& rosterBike,
                                const std::string& localActiveId, const ShadowIndex& idx) {
    if (swapId.empty() || swapId == localActiveId)
        return "";
    std::string shadow;
    return idx.Find(rosterBike, swapId, shadow) ? shadow : "";
}

inline int SlotOfBlock(uintptr_t block, uintptr_t vehiclesBase, int stride, int gfxOff, int max) {
    if (stride <= 0 || max <= 0 || block < vehiclesBase + static_cast<uintptr_t>(gfxOff)) return -1;
    const uintptr_t delta = block - vehiclesBase - static_cast<uintptr_t>(gfxOff);
    if (delta % static_cast<uintptr_t>(stride) != 0) return -1;
    const uintptr_t slot = delta / static_cast<uintptr_t>(stride);
    return slot < static_cast<uintptr_t>(max) ? static_cast<int>(slot) : -1;
}

class RemapTable {
public:
    struct Entry { int key = 0; std::string shadow; };

    void Set(int slot, int key, const std::string& shadow) {
        if (slot < 0 || slot >= 64) return;
        entries_[slot] = {key, shadow};
        used_[slot] = true;
    }
    void Clear(int slot) {
        if (slot < 0 || slot >= 64) return;
        entries_[slot] = Entry{};
        used_[slot] = false;
    }
    void ClearAll() { for (int i = 0; i < 64; ++i) Clear(i); }
    const Entry* Get(int slot) const {
        return slot >= 0 && slot < 64 && used_[slot] ? &entries_[slot] : nullptr;
    }
    bool Lookup(int slot, int key, std::string& shadow) const {
        const Entry* entry = Get(slot);
        if (!entry || entry->key != key || entry->shadow.empty()) return false;
        shadow = entry->shadow;
        return true;
    }
    bool Lookup(int slot, int key, char* shadow, size_t cap) const {
        const Entry* entry = Get(slot);
        if (!entry || entry->key != key || entry->shadow.empty() || !shadow || cap == 0) return false;
        const size_t n = (std::min)(cap - 1, entry->shadow.size());
        std::memcpy(shadow, entry->shadow.data(), n);
        shadow[n] = 0;
        return n == entry->shadow.size();
    }

private:
    Entry entries_[64];
    bool used_[64]{};
};

constexpr int kMaxShadowsInUse = 6;
constexpr int kMaxLoadedBikeGfx = 8;
constexpr int kMaxShadowFolders = 24;

} // namespace frostmod::bikesync
