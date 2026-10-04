// Internal shared helpers for KNRL engine translation units.
// Not public API: pure functions with no engine coupling.
#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace override {
namespace detail {

// Strict whole-string integer parse. Returns false on junk/overflow.
inline bool parseInt(const std::string& s, int& out) {
    if (s.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') {
        neg = (s[0] == '-');
        i = 1;
        if (s.size() == 1) return false;
    }
    long long acc = 0;
    for (; i < s.size(); ++i) {
        if (!std::isdigit((unsigned char)s[i])) return false;
        acc = acc * 10 + (s[i] - '0');
        if (!neg && acc > 1000000000LL) return false;
        if (neg && acc > 1000000000LL) return false;
    }
    out = neg ? -(int)acc : (int)acc;
    return true;
}

// Accepts "500", "500ms", "2s". Throws std::invalid_argument when no digits.
inline int parseMs(const std::string& v) {
    std::string t;
    for (char c : v) {
        if (std::isdigit((unsigned char)c) || c == '-') t += c;
    }
    if (t.empty() || t == "-") throw std::invalid_argument("no numeric value in '" + v + "'");
    long long n = std::stoll(t);
    std::string low = v;
    for (auto& c : low) c = (char)std::tolower((unsigned char)c);
    if (low.find('s') != std::string::npos && low.find("ms") == std::string::npos) n *= 1000;
    if (n > 1000000000LL || n < -1000000000LL)
        throw std::out_of_range("latency value out of range: '" + v + "'");
    return (int)n;
}

// Seeded stream step (mutates state).
inline uint64_t splitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Stateless 64-bit hash (digests, deterministic rolls).
inline uint64_t fnv1a(const std::string& s) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

inline std::string hex16(uint64_t v) {
    std::ostringstream o;
    o << std::hex << std::setw(16) << std::setfill('0') << v;
    return o.str();
}

} // namespace detail
} // namespace override
