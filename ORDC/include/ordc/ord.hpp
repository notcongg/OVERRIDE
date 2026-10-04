#pragma once

// ORDC: deterministic persistence document model for OVERRIDE (.ord files).
// Pure data + parsing + serialization. No simulation logic, no filesystem
// access, no host interaction. The engine (KNRL) builds/consumes OrdDocs;
// the persistence layer (worlds) moves bytes; the shell only invokes.
//
// Format (versioned, human-readable, parser-friendly):
//   ORD 1
//   type = world
//   id = w1
//
//   [section]
//   key = value
//   ...
// Values escape `\` -> `\\`, newline -> `\n`, CR -> `\r`. Keys/sections use
// [A-Za-z0-9_./-]. Full-line `#` comments and blank lines are skipped.
// Duplicate sections/keys, unknown header keys, and bad escapes are errors,
// never silently discarded.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ordc {

constexpr int kOrdVersion = 1;

struct OrdError {
    std::string file;
    int line = 0;
    std::string section;
    std::string key;
    std::string message;
    std::string expected;
    std::string actual;
    std::string str() const;
};

struct OrdDoc {
    int version = kOrdVersion;
    std::string type; // "world" | "achievements" | "user" | "recovery"
    std::string id;
    std::vector<std::string> sections;                          // first-seen order
    std::map<std::string, std::vector<std::string>> order;      // section -> keys
    std::map<std::string, std::map<std::string, std::string>> kv; // section -> key -> value

    void set(const std::string& section, const std::string& key, const std::string& value);
    bool has(const std::string& section, const std::string& key) const;
};

// Parse text into doc. False + err on any malformed input (file:line diag).
bool parseOrd(const std::string& text, const std::string& filename, OrdDoc& doc,
              OrdError& err);
// Deterministic serialization: sorted sections and keys.
std::string serializeOrd(const OrdDoc& doc);

// Value escaping for serialization (exported for round-trip tests).
std::string escapeValue(const std::string& v);
bool unescapeValue(const std::string& v, std::string& out);

// Strict typed readers (false + err.message on mismatch).
bool getString(const OrdDoc& doc, const std::string& section, const std::string& key,
               std::string& out, OrdError& err);
bool getInt(const OrdDoc& doc, const std::string& section, const std::string& key, int& out,
            OrdError& err);
bool getInt64(const OrdDoc& doc, const std::string& section, const std::string& key,
              int64_t& out, OrdError& err);
bool getUint64(const OrdDoc& doc, const std::string& section, const std::string& key,
               uint64_t& out, OrdError& err);
bool getBool(const OrdDoc& doc, const std::string& section, const std::string& key, bool& out,
             OrdError& err);
bool getDouble(const OrdDoc& doc, const std::string& section, const std::string& key,
               double& out, OrdError& err);
bool requireSection(const OrdDoc& doc, const std::string& section, OrdError& err);

} // namespace ordc
