#pragma once

#include <cstdint>
#include <string>

namespace override {

// Simulation clock, independent from wall-clock time.
class Clock {
public:
    uint64_t tickCount() const { return tick_; }
    bool paused() const { return paused_; }
    void pause() { paused_ = true; }
    void resume() { paused_ = false; }
    // Advance one tick. Returns false if paused (no advance).
    bool tick();
    void reset() { tick_ = 0; paused_ = false; }
    void setTick(uint64_t t) { tick_ = t; }
    std::string now() const; // "t=+<tick>"

private:
    uint64_t tick_ = 0;
    bool paused_ = false;
};

} // namespace override
