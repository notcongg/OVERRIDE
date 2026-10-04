#include "override/clock.hpp"

namespace override {

bool Clock::tick() {
    if (paused_) return false;
    ++tick_;
    return true;
}

std::string Clock::now() const {
    return "t=+" + std::to_string(tick_) + (paused_ ? " (paused)" : "");
}

} // namespace override
