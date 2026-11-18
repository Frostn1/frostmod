// The bearer token on FrostServer's one write, POST /frostserver/announce (security report A13).
//
// - A configured token shorter than kMinToken turns the endpoint off: a short password on a
//   public port is guessable.
// - The comparison takes the same time however much of the token matches.
// - An address that keeps sending wrong tokens is refused for a while before its token is even
//   looked at, so guessing costs real time.
//
// Pure logic, no Win32, so tests/announceauth_test.cpp builds and runs anywhere.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <unordered_map>

namespace announceauth {

constexpr size_t kMinToken = 24;

inline bool TokenUsable(const std::string& token) { return token.size() >= kMinToken; }

// Constant-time in the length of `expected`: every byte is compared whatever came before, and a
// wrong length is folded into the same result instead of returning early.
inline bool TokenMatches(const std::string& got, const std::string& expected) {
    unsigned diff = (unsigned)(got.size() ^ expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        const unsigned char g = i < got.size() ? (unsigned char)got[i] : 0;
        diff |= (unsigned)(g ^ (unsigned char)expected[i]);
    }
    return diff == 0;
}

// "Bearer <token>" from an Authorization header value, or "" when it is not that shape.
inline std::string BearerToken(const std::string& header) {
    static const char kPrefix[] = "Bearer ";
    if (header.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) return "";
    return header.substr(sizeof(kPrefix) - 1);
}

// Per-address backoff. After kFreeFails wrong tokens, each further one doubles the lockout,
// from kBaseMs up to kMaxMs. A right token clears the address. The table is bounded: when it is
// full, entries whose lockout has passed are forgotten first, then everything.
class FailGate {
  public:
    static constexpr int      kFreeFails = 3;
    static constexpr uint64_t kBaseMs    = 2000;
    static constexpr uint64_t kMaxMs     = 10 * 60 * 1000;
    static constexpr size_t   kMaxIps    = 1024;

    bool Blocked(uint32_t ip, uint64_t nowMs) const {
        auto it = byIp_.find(ip);
        return it != byIp_.end() && nowMs < it->second.until;
    }

    void Fail(uint32_t ip, uint64_t nowMs) {
        if (byIp_.size() >= kMaxIps && byIp_.find(ip) == byIp_.end()) Prune(nowMs);
        Entry& e = byIp_[ip];
        ++e.fails;
        if (e.fails > kFreeFails) {
            const int over = e.fails - kFreeFails - 1;
            uint64_t ms = kBaseMs << (over > 20 ? 20 : over);
            if (ms > kMaxMs) ms = kMaxMs;
            e.until = nowMs + ms;
        }
    }

    void Ok(uint32_t ip) { byIp_.erase(ip); }

    size_t Size() const { return byIp_.size(); }

  private:
    struct Entry { int fails = 0; uint64_t until = 0; };

    void Prune(uint64_t nowMs) {
        for (auto it = byIp_.begin(); it != byIp_.end();)
            it = nowMs >= it->second.until ? byIp_.erase(it) : std::next(it);
        if (byIp_.size() >= kMaxIps) byIp_.clear();
    }

    std::unordered_map<uint32_t, Entry> byIp_;
};

} // namespace announceauth
