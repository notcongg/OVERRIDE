#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace override {

struct ParsedCommand {
    std::string name;              // lowercased verb
    std::vector<std::string> args; // raw args
    std::string raw;               // original line
};

// Minimal tokenizer: splits on whitespace, honours double quotes.
std::vector<std::string> tokenize(const std::string& line);
ParsedCommand parse(const std::string& line);

// Levenshtein distance (for typo suggestions; no external deps).
int editDistance(const std::string& a, const std::string& b);
// Best candidate within maxDist, or "" when nothing is close.
std::string suggest(const std::string& input, const std::vector<std::string>& candidates,
                    int maxDist = 2);
// Strict whole-string integer parse (no trailing junk, overflow-safe).
bool parseIntStrict(const std::string& s, int& out);
bool parseUintStrict(const std::string& s, uint64_t& out);

std::string toLower(std::string s);
std::string trim(const std::string& s);

} // namespace override
