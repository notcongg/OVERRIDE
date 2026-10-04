#pragma once

// Central terminal styling for OVERSHELL (presentation only).
//
// Rules enforced here, not scattered across call sites:
//   * no ANSI in engine strings, digests, ledger, checkpoints, or .ord files
//   * color never replaces textual markers (ERROR:/WARNING:/OK: stay put)
//   * column alignment is computed on raw text, then wrapped (never broken)
//   * headless/script/test output defaults to plain (stable, parseable)
//   * NO_COLOR=1 forces plain; `color on|off|auto` overrides per session
//
// Everything is header-inline so shell and tests share one implementation
// with no extra translation unit.
#include <cstdlib>
#include <string>
#include <vector>

namespace override {
namespace color {

// ANSI codes (kept in one place; never emitted when disabled).
inline const char* kReset = "\x1b[0m";
inline const char* kBold = "\x1b[1m";
inline const char* kDim = "\x1b[2m";
inline const char* kRed = "\x1b[31m";
inline const char* kGreen = "\x1b[32m";
inline const char* kYellow = "\x1b[33m";
inline const char* kBlue = "\x1b[34m";
inline const char* kCyan = "\x1b[36m";
inline const char* kBoldRed = "\x1b[1;31m";
inline const char* kBoldGreen = "\x1b[1;32m";
inline const char* kBoldYellow = "\x1b[1;33m";
inline const char* kBoldCyan = "\x1b[1;36m";

enum class Mode { Auto, On, Off };

inline bool& enabledFlag() {
    static bool on = false;
    return on;
}
inline Mode& modeFlag() {
    static Mode m = Mode::Auto;
    return m;
}
inline bool& interactiveFlag() {
    static bool on = false;
    return on;
}

// Recompute enabled(): NO_COLOR wins, then explicit mode, then auto rule
// (interactive sessions only).
inline void refresh() {
    bool noColor = (std::getenv("NO_COLOR") != nullptr);
    if (noColor) {
        enabledFlag() = false;
        return;
    }
    Mode m = modeFlag();
    if (m == Mode::On) enabledFlag() = true;
    else if (m == Mode::Off) enabledFlag() = false;
    else enabledFlag() = interactiveFlag();
}
inline void setMode(Mode m) {
    modeFlag() = m;
    refresh();
}
inline void setInteractive(bool on) {
    interactiveFlag() = on;
    refresh();
}
inline bool enabled() { return enabledFlag(); }

inline std::string wrap(const std::string& s, const char* code) {
    if (!enabled() || s.empty()) return s;
    return std::string(code) + s + kReset;
}
// Semantic styles (text markers are always preserved by callers).
inline std::string error(const std::string& s) { return wrap(s, kBoldRed); }
inline std::string warn(const std::string& s) { return wrap(s, kBoldYellow); }
inline std::string ok(const std::string& s) { return wrap(s, kBoldGreen); }
inline std::string info(const std::string& s) { return wrap(s, kCyan); }
inline std::string dim(const std::string& s) { return wrap(s, kDim); }
inline std::string bold(const std::string& s) { return wrap(s, kBold); }
inline std::string value(const std::string& s) { return wrap(s, kBlue); }

inline bool isWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_';
}

// Paint whole-word occurrences of any word in `words` with `code`.
// Padding-safe: replacements only widen invisible spans when enabled, and
// callers must pad raw text BEFORE calling.
inline std::string paintWords(const std::string& line, const std::vector<std::string>& words,
                              const char* code) {
    if (!enabled()) return line;
    std::string out;
    size_t i = 0;
    while (i < line.size()) {
        bool matched = false;
        for (const auto& w : words) {
            if (line.compare(i, w.size(), w) != 0) continue;
            bool left = (i == 0) || !isWordChar(line[i - 1]);
            bool right = (i + w.size() >= line.size()) || !isWordChar(line[i + w.size()]);
            if (left && right) {
                out += code;
                out += w;
                out += kReset;
                i += w.size();
                matched = true;
                break;
            }
        }
        if (!matched) out += line[i++];
    }
    return out;
}

// Semantic state palette (whole words only: SERVER_FAILED stays untouched).
inline std::string paintStates(const std::string& line) {
    if (!enabled()) return line;
    static const std::vector<std::string> good = {"ONLINE", "RUNNING",  "UP",     "READY",
                                                  "HEALTHY", "MOUNTED", "LOADED", "CLEAN"};
    static const std::vector<std::string> warnW = {"WARNING",  "DEGRADED", "CRITICAL",
                                                   "PAUSED",   "RESTARTING", "BOOTING",
                                                   "UNSTABLE", "THROTTLED", "CORRUPTED",
                                                   "LOSSY",    "SLOW"};
    static const std::vector<std::string> bad = {"FAILED",  "PANICKED", "HALTED", "OFF",
                                                 "KILLED",  "CRASHED",  "DOWN",   "MISSING",
                                                 "ZOMBIE",  "TIMEOUT",  "REFUSED", "DENIED"};
    std::string o = paintWords(line, good, kBoldGreen);
    o = paintWords(o, warnW, kBoldYellow);
    // Paint bad words without re-painting inside earlier insertions: the
    // inserted escapes contain no word chars adjacent to matches, and whole
    // words like FAILED inside "[31m" runs cannot match (no boundaries).
    o = paintWords(o, bad, kBoldRed);
    return o;
}

// Line-prefix markers: error:/WARNING:/OK:/SUCCESS:/INFO: at line start.
inline std::string paintMarkers(const std::string& line) {
    if (!enabled()) return line;
    auto starts = [&](const std::string& p) { return line.rfind(p, 0) == 0; };
    if (starts("error:") || starts("ERROR:")) return error(line);
    if (starts("WARNING:") || starts("WARN:")) return warn(line);
    if (starts("OK:") || starts("SUCCESS:") || starts("VALIDATE:")) return ok(line);
    if (starts("INFO:")) return info(line);
    return line;
}

// Full line treatment: markers first, then state words.
inline std::string paintLine(const std::string& line) {
    if (!enabled()) return line;
    return paintStates(paintMarkers(line));
}

// Achievement viewer/notification accents: [X] green, [?] yellow, titles
// bold, progress dim. Operates on rendered plain text (engine stays clean).
inline std::string paintAchievements(const std::string& text) {
    if (!enabled()) return text;
    std::string o = paintWords(text, {"[X]"}, kBoldGreen);
    o = paintWords(o, {"[?]"}, kBoldYellow);
    o = paintWords(o, {"ACHIEVEMENTS", "ACHIEVEMENT UNLOCKED"}, kBoldCyan);
    return o;
}

// Progress bar: completed '/' run green, remainder dim, percent bold.
// Falls back to the plain bar when disabled (always readable).
inline std::string paintBar(const std::string& bar) {
    if (!enabled()) return bar;
    size_t l = bar.find('['), r = bar.find(']');
    if (l == std::string::npos || r == std::string::npos || r <= l) return bar;
    std::string out = bar.substr(0, l + 1);
    for (size_t i = l + 1; i < r; ++i) {
        if (bar[i] == '/') {
            out += kBoldGreen;
            out += '/';
            out += kReset;
        } else {
            out += kDim;
            out += bar[i];
            out += kReset;
        }
    }
    out += bar.substr(r);
    return out;
}

} // namespace color
} // namespace override
