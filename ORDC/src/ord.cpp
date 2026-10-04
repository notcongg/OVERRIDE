#include "ordc/ord.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <sstream>

namespace ordc {

std::string OrdError::str() const {
    std::ostringstream o;
    o << file << ":" << line;
    if (!section.empty()) o << " [" << section << "]";
    if (!key.empty()) o << " " << key;
    o << ": " << message;
    if (!expected.empty()) o << " (expected " << expected << ")";
    if (!actual.empty()) o << " [got '" << actual << "']";
    return o.str();
}

void OrdDoc::set(const std::string& section, const std::string& key,
                 const std::string& value) {
    if (!kv.count(section)) {
        sections.push_back(section);
        order[section] = {};
    }
    if (!kv[section].count(key)) order[section].push_back(key);
    kv[section][key] = value;
}

bool OrdDoc::has(const std::string& section, const std::string& key) const {
    auto it = kv.find(section);
    if (it == kv.end()) return false;
    return it->second.count(key) > 0;
}

std::string escapeValue(const std::string& v) {
    std::string o;
    for (char c : v) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    return o;
}

bool unescapeValue(const std::string& v, std::string& out) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] != '\\') {
            o += v[i];
            continue;
        }
        if (i + 1 >= v.size()) return false; // trailing backslash
        char n = v[i + 1];
        if (n == '\\') o += '\\';
        else if (n == 'n') o += '\n';
        else if (n == 'r') o += '\r';
        else return false; // unknown escape
        ++i;
    }
    out = o;
    return true;
}

static bool validToken(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '.' || c == '/' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static std::string trimRight(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(0, e);
}

bool parseOrd(const std::string& text, const std::string& filename, OrdDoc& doc,
              OrdError& err) {
    doc = OrdDoc{};
    err = OrdError{};
    err.file = filename;
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : text) {
            if (c == '\n') {
                lines.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        lines.push_back(cur);
    }
    // Line 1: "ORD <version>".
    if (lines.empty() || lines[0].rfind("ORD ", 0) != 0) {
        err.line = 1;
        err.message = "bad header (first line must be 'ORD <version>')";
        err.expected = "ORD 1";
        err.actual = lines.empty() ? "(empty file)" : lines[0];
        return false;
    }
    {
        std::string num = trimRight(lines[0].substr(4));
        if (num != "1") {
            err.line = 1;
            err.message = "unsupported ORD version (no silent downgrade)";
            err.expected = "1";
            err.actual = num;
            return false;
        }
        doc.version = 1;
    }
    std::string section; // "" = header
    bool haveType = false, haveId = false;
    for (size_t i = 1; i < lines.size(); ++i) {
        err.line = (int)i + 1;
        std::string line = lines[i];
        // Skip blanks and full-line comments.
        size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        if (s >= line.size() || line[s] == '#') continue;
        std::string body = line.substr(s);
        if (!body.empty() && body.back() == '\r') body.pop_back();
        if (body.empty()) continue;
        // Section header?
        if (body[0] == '[') {
            if (body.back() != ']') {
                err.message = "malformed section header";
                err.expected = "[name]";
                err.actual = body;
                return false;
            }
            std::string name = body.substr(1, body.size() - 2);
            if (!validToken(name)) {
                err.message = "bad section name";
                err.expected = "[A-Za-z0-9_./-]";
                err.actual = name;
                return false;
            }
            if (doc.kv.count(name)) {
                err.section = name;
                err.message = "duplicate section";
                return false;
            }
            section = name;
            doc.sections.push_back(name);
            doc.order[name] = {};
            doc.kv[name] = {};
            continue;
        }
        // Key = value (split at first '=').
        auto eq = body.find('=');
        if (eq == std::string::npos) {
            err.section = section;
            err.message = "malformed line (expected 'key = value' or '[section]')";
            err.actual = body;
            return false;
        }
        std::string key = trimRight(body.substr(0, eq));
        // Trim trailing whitespace on the key side only.
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        if (!validToken(key)) {
            err.section = section;
            err.message = "bad key";
            err.expected = "[A-Za-z0-9_./-]";
            err.actual = key;
            return false;
        }
        std::string raw = body.substr(eq + 1);
        if (!raw.empty() && raw[0] == ' ') raw.erase(0, 1); // single separator space
        std::string value;
        if (!unescapeValue(raw, value)) {
            err.section = section;
            err.key = key;
            err.message = "bad escape in value";
            err.expected = "\\\\, \\n, \\r only";
            err.actual = raw;
            return false;
        }
        if (section.empty()) {
            // Header keys: exactly type + id.
            if (key != "type" && key != "id") {
                err.message = "unknown header key (only 'type' and 'id' allowed)";
                err.actual = key;
                return false;
            }
            if (key == "type") {
                if (haveType) {
                    err.message = "duplicate header key";
                    err.actual = key;
                    return false;
                }
                haveType = true;
                doc.type = value;
            } else {
                if (haveId) {
                    err.message = "duplicate header key";
                    err.actual = key;
                    return false;
                }
                haveId = true;
                doc.id = value;
            }
            continue;
        }
        if (doc.kv[section].count(key)) {
            err.section = section;
            err.key = key;
            err.message = "duplicate key";
            return false;
        }
        doc.order[section].push_back(key);
        doc.kv[section][key] = value;
    }
    if (!haveType || !haveId) {
        err.line = 2;
        err.message = "missing required header (type + id before first section)";
        err.expected = "type = ... / id = ...";
        return false;
    }
    if (doc.type != "world" && doc.type != "achievements" && doc.type != "user" &&
        doc.type != "recovery") {
        err.line = 2;
        err.message = "invalid type";
        err.expected = "world | achievements | user | recovery";
        err.actual = doc.type;
        return false;
    }
    return true;
}

std::string serializeOrd(const OrdDoc& doc) {
    std::ostringstream o;
    o << "ORD " << doc.version << "\n";
    o << "type = " << doc.type << "\n";
    o << "id = " << doc.id << "\n";
    std::vector<std::string> secs = doc.sections;
    std::sort(secs.begin(), secs.end());
    secs.erase(std::unique(secs.begin(), secs.end()), secs.end());
    // Include sections known only via kv (defensive; set() keeps both).
    for (const auto& [s, _] : doc.kv)
        if (std::find(secs.begin(), secs.end(), s) == secs.end()) secs.push_back(s);
    std::sort(secs.begin(), secs.end());
    for (const auto& s : secs) {
        o << "\n[" << s << "]\n";
        auto it = doc.kv.find(s);
        if (it == doc.kv.end()) continue;
        std::vector<std::string> keys;
        for (const auto& [k, _] : it->second) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        for (const auto& k : keys) o << k << " = " << escapeValue(it->second.at(k)) << "\n";
    }
    return o.str();
}

static bool lookup(const OrdDoc& doc, const std::string& section, const std::string& key,
                   std::string& out, OrdError& err) {
    auto sit = doc.kv.find(section);
    if (sit == doc.kv.end()) {
        err.section = section;
        err.message = "missing required section";
        return false;
    }
    auto kit = sit->second.find(key);
    if (kit == sit->second.end()) {
        err.section = section;
        err.key = key;
        err.message = "missing required key";
        return false;
    }
    out = kit->second;
    return true;
}

bool getString(const OrdDoc& doc, const std::string& section, const std::string& key,
               std::string& out, OrdError& err) {
    return lookup(doc, section, key, out, err);
}

static bool parseStrictUint64(const std::string& s, uint64_t& out) {
    if (s.empty()) return false;
    uint64_t acc = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        uint64_t d = (uint64_t)(c - '0');
        if (acc > (UINT64_MAX - d) / 10) return false;
        acc = acc * 10 + d;
    }
    out = acc;
    return true;
}

static bool parseStrictInt64(const std::string& s, int64_t& out) {
    if (s.empty()) return false;
    bool neg = false;
    size_t i = 0;
    if (s[0] == '-') {
        neg = true;
        i = 1;
        if (s.size() == 1) return false;
    }
    uint64_t acc = 0;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c < '0' || c > '9') return false;
        uint64_t d = (uint64_t)(c - '0');
        if (acc > (UINT64_MAX - d) / 10) return false;
        acc = acc * 10 + d;
    }
    if (!neg && acc > (uint64_t)INT64_MAX) return false;
    if (neg && acc > (uint64_t)INT64_MAX + 1) return false;
    out = neg ? -(int64_t)acc : (int64_t)acc;
    return true;
}

bool getInt(const OrdDoc& doc, const std::string& section, const std::string& key, int& out,
            OrdError& err) {
    int64_t v = 0;
    if (!getInt64(doc, section, key, v, err)) return false;
    if (v < INT32_MIN || v > INT32_MAX) {
        err.section = section;
        err.key = key;
        err.message = "integer out of int range";
        return false;
    }
    out = (int)v;
    return true;
}

bool getInt64(const OrdDoc& doc, const std::string& section, const std::string& key,
              int64_t& out, OrdError& err) {
    std::string raw;
    if (!lookup(doc, section, key, raw, err)) return false;
    if (!parseStrictInt64(raw, out)) {
        err.section = section;
        err.key = key;
        err.message = "invalid integer";
        err.expected = "[-]digits";
        err.actual = raw;
        return false;
    }
    return true;
}

bool getUint64(const OrdDoc& doc, const std::string& section, const std::string& key,
               uint64_t& out, OrdError& err) {
    std::string raw;
    if (!lookup(doc, section, key, raw, err)) return false;
    if (!parseStrictUint64(raw, out)) {
        err.section = section;
        err.key = key;
        err.message = "invalid unsigned integer";
        err.expected = "digits";
        err.actual = raw;
        return false;
    }
    return true;
}

bool getBool(const OrdDoc& doc, const std::string& section, const std::string& key, bool& out,
             OrdError& err) {
    std::string raw;
    if (!lookup(doc, section, key, raw, err)) return false;
    if (raw == "true") {
        out = true;
        return true;
    }
    if (raw == "false") {
        out = false;
        return true;
    }
    err.section = section;
    err.key = key;
    err.message = "invalid boolean";
    err.expected = "true or false";
    err.actual = raw;
    return false;
}

bool getDouble(const OrdDoc& doc, const std::string& section, const std::string& key,
               double& out, OrdError& err) {
    std::string raw;
    if (!lookup(doc, section, key, raw, err)) return false;
    try {
        size_t pos = 0;
        double v = std::stod(raw, &pos);
        if (pos != raw.size()) throw std::runtime_error("trailing");
        out = v;
        return true;
    } catch (...) {
    }
    err.section = section;
    err.key = key;
    err.message = "invalid number";
    err.expected = "decimal number";
    err.actual = raw;
    return false;
}

bool requireSection(const OrdDoc& doc, const std::string& section, OrdError& err) {
    if (doc.kv.count(section)) return true;
    err.section = section;
    err.message = "missing required section";
    return false;
}

} // namespace ordc
