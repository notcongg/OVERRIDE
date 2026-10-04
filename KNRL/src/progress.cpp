#include "override/progress.hpp"

#include <iostream>

namespace override {

std::string progressBar(int done, int total, int width) {
    if (width < 1) width = 1;
    if (total < 1) total = 1;
    if (done < 0) done = 0;
    if (done > total) done = total;
    int pct = (total == 0) ? 100 : (done * 100) / total;
    int filled = (done * width) / total;
    std::string o = "[";
    for (int i = 0; i < width; ++i) o += (i < filled) ? '/' : '-';
    o += "] " + std::to_string(pct) + "%";
    return o;
}

ProgressOut::ProgressOut(int minTotal, int width) : minTotal_(minTotal), width_(width) {}

void ProgressOut::setPainter(std::function<std::string(const std::string&)> painter) {
    painter_ = std::move(painter);
}

void ProgressOut::update(int done, int total) {
    if (total < 1) total = 1;
    if (total < minTotal_) return; // instant op: normal output instead
    if (done < 0) done = 0;
    if (done > total) done = total;
    int pct = (done * 100) / total;
    if (pct == lastPct_) return; // one line, rewritten in place
    lastPct_ = pct;
    active_ = true;
    std::string bar = progressBar(done, total, width_);
    if (painter_) bar = painter_(bar);
    std::cout << "\r" << bar << std::flush;
}

void ProgressOut::finish() {
    if (!active_) return;
    std::cout << "\n" << std::flush;
    active_ = false;
}

} // namespace override
