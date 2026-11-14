#pragma once

#include "servermsg.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace looksync {

struct Sha256 {
    uint32_t h[8];
    uint64_t bytes;
    unsigned char block[64];
    size_t used;

    static uint32_t Rotate(uint32_t x, unsigned n) {
        return (x >> n) | (x << (32 - n));
    }

    void Init() {
        const uint32_t initial[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
        };
        std::memcpy(h, initial, sizeof(h));
        bytes = 0;
        used = 0;
    }

    void Transform(const unsigned char* p) {
        static const uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
        };
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                   (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t a = Rotate(w[i - 15], 7) ^ Rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t b = Rotate(w[i - 2], 17) ^ Rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + a + w[i - 7] + b;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], z = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1 = Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = z + s1 + ch + k[i] + w[i];
            uint32_t s0 = Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + maj;
            z = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += z;
    }

    void Update(const void* data, size_t n) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        bytes += n;
        while (n) {
            size_t take = std::min(n, sizeof(block) - used);
            std::memcpy(block + used, p, take);
            used += take;
            p += take;
            n -= take;
            if (used == sizeof(block)) {
                Transform(block);
                used = 0;
            }
        }
    }

    void Final(unsigned char out[32]) {
        uint64_t bits = bytes * 8;
        block[used++] = 0x80;
        if (used > 56) {
            while (used < 64) block[used++] = 0;
            Transform(block);
            used = 0;
        }
        while (used < 56) block[used++] = 0;
        for (int i = 7; i >= 0; --i)
            block[used++] = static_cast<unsigned char>(bits >> (i * 8));
        Transform(block);
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 4; ++j)
                out[i * 4 + j] = static_cast<unsigned char>(h[i] >> (24 - j * 8));
    }
};

inline std::string Hex16(const unsigned char digest[32]) {
    static const char hex[] = "0123456789abcdef";
    std::string result(32, '0');
    for (int i = 0; i < 16; ++i) {
        result[i * 2] = hex[digest[i] >> 4];
        result[i * 2 + 1] = hex[digest[i] & 15];
    }
    return result;
}

struct Look {
    int raceNum = 0;
    std::string rider;
    std::string bike;
    std::string skin;
    std::string bikeModel;
    std::string riderModel;
    std::string helmetModel;
    std::string bootsModel;
};

inline bool ValidBike(const std::string& s) {
    if (s.empty() || s.size() > 31) return false;
    for (unsigned char c : s) {
        if (!(std::isalnum(c) || c == ' ' || c == '_' || c == '.' || c == '-' ||
              c == '~' || c == '(' || c == ')')) return false;
    }
    return true;
}

inline bool ValidRider(const std::string& s) {
    if (s.empty() || s.size() > 31) return false;
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return false;
    return true;
}

inline bool ValidSkin(const std::string& s) {
    if (s.empty()) return true;
    if (s.size() != 32) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

inline bool ValidLook(const Look& look) {
    return look.raceNum >= 0 && ValidRider(look.rider) &&
           ValidBike(look.bike) && ValidSkin(look.skin) &&
           ValidSkin(look.bikeModel) && ValidSkin(look.riderModel) &&
           ValidSkin(look.helmetModel) && ValidSkin(look.bootsModel);
}

inline std::string BuildClaimJson(const Look& look) {
    std::string json = "{\"raceNumber\":" + std::to_string(look.raceNum) +
                       ",\"rider\":\"" + servermsg::JsonEscape(look.rider) +
                       "\",\"bike\":\"" + servermsg::JsonEscape(look.bike) +
                       "\",\"skin\":\"" + servermsg::JsonEscape(look.skin) + "\"";
    if (!look.bikeModel.empty()) json += ",\"bikeModel\":\"" + look.bikeModel + "\"";
    if (!look.riderModel.empty()) json += ",\"riderModel\":\"" + look.riderModel + "\"";
    if (!look.helmetModel.empty()) json += ",\"helmetModel\":\"" + look.helmetModel + "\"";
    if (!look.bootsModel.empty()) json += ",\"bootsModel\":\"" + look.bootsModel + "\"";
    return json + '}';
}

inline bool ParseClaimJson(const std::string& body, Look& look) {
    if (body.size() > 1024) return false;
    size_t first = body.find_first_not_of(" \t\r\n");
    size_t last = body.find_last_not_of(" \t\r\n");
    if (first == std::string::npos || body[first] != '{' || body[last] != '}') return false;
    double raceNumber = 0;
    Look parsed;
    if (!servermsg::ObjectNumber(body, "raceNumber", raceNumber) ||
        raceNumber < 0 || raceNumber > 2147483647.0 ||
        raceNumber != static_cast<int>(raceNumber) ||
        !servermsg::ObjectString(body, "rider", parsed.rider) ||
        !servermsg::ObjectString(body, "bike", parsed.bike) ||
        !servermsg::ObjectString(body, "skin", parsed.skin)) return false;
    const char* modelKeys[] = {"bikeModel", "riderModel", "helmetModel", "bootsModel"};
    std::string* modelValues[] = {&parsed.bikeModel, &parsed.riderModel,
                                  &parsed.helmetModel, &parsed.bootsModel};
    for (size_t i = 0; i < 4; ++i) {
        const std::string needle = std::string("\"") + modelKeys[i] + "\"";
        if (body.find(needle) != std::string::npos &&
            (!servermsg::ObjectString(body, modelKeys[i], *modelValues[i]) ||
             !ValidSkin(*modelValues[i]))) return false;
    }
    parsed.raceNum = static_cast<int>(raceNumber);
    if (!ValidLook(parsed)) return false;
    look = parsed;
    return true;
}

class Registry {
public:
    enum Result { Accepted, Rejected_Invalid, Rejected_Taken, Rejected_Full };

    void Expire(uint32_t now) {
        size_t oldSize = entries_.size();
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
            return now - entry.seen >= 20;
        }), entries_.end());
        if (entries_.size() != oldSize) ++seq_;
    }

    Result Claim(uint32_t ip, const Look& look, uint32_t now) {
        Expire(now);
        if (!ip || !ValidLook(look)) return Rejected_Invalid;
        for (const Entry& entry : entries_)
            if (entry.ip != ip && SameRider(entry.look, look)) return Rejected_Taken;
        for (Entry& entry : entries_) {
            if (entry.ip == ip && SameRider(entry.look, look)) {
                bool changed = !Same(entry.look, look);
                entry.look = look;
                entry.seen = now;
                if (changed) ++seq_;
                return Accepted;
            }
        }
        size_t fromIp = 0;
        for (const Entry& entry : entries_)
            if (entry.ip == ip) ++fromIp;
        if (fromIp >= 4 || entries_.size() >= 64) return Rejected_Full;
        entries_.push_back({ip, now, look});
        ++seq_;
        return Accepted;
    }

    uint32_t Seq() const { return seq_; }
    size_t Size() const { return entries_.size(); }

    std::string BuildFeedJson() const {
        std::string json = "{\"seq\":" + std::to_string(seq_) + ",\"looks\":[";
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (i) json += ',';
            json += BuildClaimJson(entries_[i].look);
        }
        return json + "]}";
    }

private:
    struct Entry { uint32_t ip, seen; Look look; };
    static bool SameRider(const Look& a, const Look& b) {
        return a.raceNum == b.raceNum && a.rider == b.rider;
    }
    static bool Same(const Look& a, const Look& b) {
        return SameRider(a, b) && a.bike == b.bike && a.skin == b.skin &&
               a.bikeModel == b.bikeModel && a.riderModel == b.riderModel &&
               a.helmetModel == b.helmetModel && a.bootsModel == b.bootsModel;
    }
    std::vector<Entry> entries_;
    uint32_t seq_ = 0;
};

inline bool ParseFeedJson(const std::string& data, std::vector<Look>& out, uint32_t& seq) {
    out.clear();
    seq = 0;
    if (data.size() > 65536) return false;
    size_t last = data.find_last_not_of(" \t\r\n");
    if (last == std::string::npos || data[last] != '}') return false;
    double number = 0;
    if (!servermsg::ObjectNumber(data, "seq", number) || number < 0 ||
        number > 4294967295.0 || number != static_cast<uint32_t>(number)) return false;
    seq = static_cast<uint32_t>(number);
    size_t key = data.find("\"looks\"");
    size_t open = key == std::string::npos ? std::string::npos : data.find('[', key);
    if (open == std::string::npos) return false;
    int depth = 0;
    size_t start = 0;
    bool inString = false;
    for (size_t i = open + 1; i < data.size(); ++i) {
        char c = data[i];
        if (inString) {
            if (c == '\\') {
                if (++i >= data.size()) return false;
            } else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{') {
            if (depth++ == 0) start = i;
        } else if (c == '}') {
            if (depth <= 0) return false;
            if (--depth == 0) {
                Look look;
                if (!ParseClaimJson(data.substr(start, i - start + 1), look)) return false;
                out.push_back(look);
                if (out.size() > 64) return false;
            }
        } else if (c == ']' && depth == 0) return !inString;
    }
    return false;
}

class SkinIndex {
public:
    void Set(const std::string& bike, const std::string& id, const std::string& paint) {
        if (ValidBike(bike) && ValidSkin(id) && !id.empty()) values_[{Lower(bike), id}] = paint;
    }
    bool Find(const std::string& bike, const std::string& id, std::string& out) const {
        auto it = values_.find({Lower(bike), id});
        if (it == values_.end()) return false;
        out = it->second;
        return true;
    }
    void Clear() { values_.clear(); }
    size_t Size() const { return values_.size(); }

private:
    static std::string Lower(std::string value) {
        for (char& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    }
    std::map<std::pair<std::string, std::string>, std::string> values_;
};

struct AppliedKey { int key = 0; int handle = 0; };
inline bool NeedsApply(const AppliedKey& last, int key, int handle) {
    return last.key != key || last.handle != handle;
}
inline bool NeedsApply(int lastKey, int lastHandle, int key, int handle) {
    return lastKey != key || lastHandle != handle;
}

} // namespace looksync
