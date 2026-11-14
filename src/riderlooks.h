#pragma once

#include "sha256.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace frostmod {

struct RiderLook {
    std::string rider;
    std::string gear[3];
    std::vector<std::pair<std::string, std::string>> bikes;
};

namespace riderlooks_detail {

inline int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

inline bool Decode(const std::string& encoded, std::string& decoded) {
    decoded.clear();
    decoded.reserve(encoded.size());
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] != '%') {
            const unsigned char c = static_cast<unsigned char>(encoded[i]);
            const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '.' ||
                              c == '~' || c == '(' || c == ')' || c == '[' || c == ']' || c == '-';
            if (!safe) {
                decoded.clear();
                return false;
            }
            decoded.push_back(encoded[i]);
            continue;
        }
        if (i + 2 >= encoded.size()) {
            decoded.clear();
            return false;
        }
        const int hi = HexValue(encoded[i + 1]);
        const int lo = HexValue(encoded[i + 2]);
        if (hi < 0 || lo < 0) {
            decoded.clear();
            return false;
        }
        decoded.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return true;
}

inline std::string Trim(const std::string& value) {
    size_t first = 0;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t' ||
                                    value[first] == '\r' || value[first] == '\n')) {
        ++first;
    }
    size_t last = value.size();
    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t' ||
                            value[last - 1] == '\r' || value[last - 1] == '\n')) {
        --last;
    }
    return value.substr(first, last - first);
}

inline char FoldAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

inline std::vector<std::string> Split(const std::string& value, char separator) {
    std::vector<std::string> result;
    size_t start = 0;
    for (;;) {
        const size_t end = value.find(separator, start);
        result.push_back(value.substr(start, end - start));
        if (end == std::string::npos) {
            return result;
        }
        start = end + 1;
    }
}

} // namespace riderlooks_detail

inline std::string PercentDecode(const std::string& encoded) {
    std::string decoded;
    riderlooks_detail::Decode(encoded, decoded);
    return decoded;
}

inline std::string PercentEncode(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char c : value) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '.' ||
                          c == '~' || c == '(' || c == ')' || c == '[' || c == ']' || c == '-';
        if (safe) {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[c >> 4]);
            encoded.push_back(hex[c & 15]);
        }
    }
    return encoded;
}

inline bool NameMatches(const std::string& a, const std::string& b) {
    const std::string left = riderlooks_detail::Trim(a);
    const std::string right = riderlooks_detail::Trim(b);
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        if (riderlooks_detail::FoldAscii(left[i]) != riderlooks_detail::FoldAscii(right[i])) {
            return false;
        }
    }
    return true;
}

inline std::string BikeSwapId(const RiderLook& look, const std::string& bike) {
    for (const auto& pair : look.bikes) {
        if (NameMatches(pair.first, bike)) {
            return pair.second;
        }
    }
    return {};
}

inline bool ParseRiderLooks(const std::string& looks, std::vector<RiderLook>& out, int* skipped) {
    out.clear();
    int bad = 0;
    if (looks.empty()) {
        if (skipped) {
            *skipped = 0;
        }
        return true;
    }
    const std::vector<std::string> records = riderlooks_detail::Split(looks, ';');
    for (size_t recordIndex = 0; recordIndex < records.size(); ++recordIndex) {
        if (recordIndex >= 64) {
            ++bad;
            continue;
        }
        const std::vector<std::string> fields = riderlooks_detail::Split(records[recordIndex], '|');
        if (fields.size() < 4 || fields.size() > 20) {
            ++bad;
            continue;
        }
        RiderLook parsed;
        bool valid = riderlooks_detail::Decode(fields[0], parsed.rider) && !parsed.rider.empty();
        for (int i = 0; valid && i < 3; ++i) {
            valid = riderlooks_detail::Decode(fields[i + 1], parsed.gear[i]);
            if (valid && !parsed.gear[i].empty() && !ValidId(parsed.gear[i])) {
                parsed.gear[i].clear();
            }
        }
        for (size_t i = 4; valid && i < fields.size(); ++i) {
            const size_t colon = fields[i].find(':');
            if (colon == std::string::npos || fields[i].find(':', colon + 1) != std::string::npos) {
                valid = false;
                break;
            }
            std::string bike;
            std::string id;
            valid = riderlooks_detail::Decode(fields[i].substr(0, colon), bike) &&
                    riderlooks_detail::Decode(fields[i].substr(colon + 1), id) && !bike.empty();
            if (valid && !id.empty() && !ValidId(id)) {
                id.clear();
            }
            if (valid) {
                parsed.bikes.push_back({std::move(bike), std::move(id)});
            }
        }
        if (!valid) {
            ++bad;
            continue;
        }
        out.push_back(std::move(parsed));
    }
    if (skipped) {
        *skipped = bad;
    }
    return true;
}

inline std::string BuildRiderLooks(const std::vector<RiderLook>& looks) {
    std::string result;
    const size_t count = looks.size() < 64 ? looks.size() : 64;
    for (size_t i = 0; i < count; ++i) {
        if (i) {
            result.push_back(';');
        }
        result += PercentEncode(looks[i].rider);
        for (int part = 0; part < 3; ++part) {
            result.push_back('|');
            result += PercentEncode(ValidId(looks[i].gear[part]) ? looks[i].gear[part] : std::string());
        }
        const size_t pairs = looks[i].bikes.size() < 16 ? looks[i].bikes.size() : 16;
        for (size_t pair = 0; pair < pairs; ++pair) {
            result.push_back('|');
            result += PercentEncode(looks[i].bikes[pair].first);
            result.push_back(':');
            if (ValidId(looks[i].bikes[pair].second)) {
                result += looks[i].bikes[pair].second;
            }
        }
    }
    return result;
}

} // namespace frostmod
