#pragma once

#include <functional>
#include <string>

namespace override {

// Progress callback: `done` units of `total` finished (0 <= done <= total,
// total >= 1). The engine invokes it for genuinely stepwise simulated work
// (ticks advanced, boot phases, restore stages, copy chunks); the shell
// decides whether rendering a bar is worthwhile. No simulation internals
// leak: counts only.
using ProgressCb = std::function<void(int done, int total)>;

// Pure deterministic bar renderer, e.g. "[/////-----] 50%".
// `/` = completed, `-` = remaining. Unit-testable, no I/O.
std::string progressBar(int done, int total, int width = 10);

// Render-worthy only for meaningful multi-step work, never instant ops.
inline bool wantProgressBar(int total) { return total >= 5; }

// Terminal renderer: rewrites one line in place (`\r`), emitting only when
// the shown percent changes, and finishes the line on completion. Silent
// unless the observed total reaches `minTotal`.
class ProgressOut {
public:
    explicit ProgressOut(int minTotal = 5, int width = 10);
    // Optional presentation painter for the finished bar string (identity by
    // default, so engine output stays plain ASCII unless the shell injects
    // styling).
    void setPainter(std::function<std::string(const std::string&)> painter);
    void update(int done, int total);
    void finish();
    bool active() const { return active_; }

private:
    int minTotal_;
    int width_;
    int lastPct_ = -1;
    bool active_ = false;
    std::function<std::string(const std::string&)> painter_;
};

} // namespace override
