#include "override/parser.hpp"

#include <cctype>
#include <cstdint>
#include <vector>

namespace override {

std::string toLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> toks;
    std::string cur;
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            inQuotes = !inQuotes;
            continue;
        }
        if (!inQuotes && std::isspace((unsigned char)c)) {
            if (!cur.empty()) {
                toks.push_back(cur);
                cur.clear();
            }
            continue;
        }
        if (!inQuotes && c == ';') {
            if (!cur.empty()) {
                toks.push_back(cur);
                cur.clear();
            }
            toks.push_back(";");
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) toks.push_back(cur);
    return toks;
}

ParsedCommand parse(const std::string& line) {
    ParsedCommand pc;
    pc.raw = line;
    auto toks = tokenize(line);
    if (toks.empty()) return pc;
    pc.name = toLower(toks[0]);
    for (size_t i = 1; i < toks.size(); ++i) pc.args.push_back(toks[i]);
    return pc;
}

int editDistance(const std::string& a, const std::string& b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            size_t del = prev[j] + 1, ins = cur[j - 1] + 1, sub = prev[j - 1] + cost;
            cur[j] = del < ins ? (del < sub ? del : sub) : (ins < sub ? ins : sub);
        }
        prev.swap(cur);
    }
    return (int)prev[b.size()];
}

std::string suggest(const std::string& input, const std::vector<std::string>& candidates,
                    int maxDist) {
    std::string best;
    int bestDist = maxDist + 1;
    for (const auto& c : candidates) {
        int d = editDistance(input, c);
        if (d < bestDist) {
            bestDist = d;
            best = c;
        }
    }
    return bestDist <= maxDist ? best : "";
}

bool parseIntStrict(const std::string& s, int& out) {
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
        if (acc > 2000000000LL) return false;
    }
    out = neg ? -(int)acc : (int)acc;
    return true;
}

bool parseUintStrict(const std::string& s, uint64_t& out) {
    if (s.empty()) return false;
    uint64_t acc = 0;
    for (char c : s) {
        if (!std::isdigit((unsigned char)c)) return false;
        unsigned digit = (unsigned)(c - '0');
        if (acc > (0xFFFFFFFFFFFFFFFFull - digit) / 10ull) return false; // overflow
        acc = acc * 10 + digit;
    }
    out = acc;
    return true;
}

} // namespace override
