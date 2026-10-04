#include "override/history.hpp"

namespace override {

Snapshot& History::take(uint64_t tick, bool clockPaused, uint64_t seed, uint64_t rng,
                        bool autoFaults, int autoRate, int nextPid, std::string label,
                        const std::map<std::string, Node>& nodes, const std::vector<Link>& links,
                        const std::map<int, SimProcess>& procs,
                        const std::map<std::string, Service>& services,
                        const HardwareManager& hw, const Vfs& vfs, const std::string& cwd,
                        size_t eventCount) {
    Snapshot s;
    s.id = nextId_++;
    s.tick = tick;
    s.clockPaused = clockPaused;
    s.seed = seed;
    s.rng = rng;
    s.autoFaults = autoFaults;
    s.autoRate = autoRate;
    s.nextPid = nextPid;
    s.label = std::move(label);
    s.nodes = nodes;
    s.links = links;
    s.procs = procs;
    s.services = services;
    s.hw = hw;
    s.vfs = vfs;
    s.cwd = cwd;
    s.eventCount = eventCount;
    snapshots_.push_back(std::move(s));
    return snapshots_.back();
}

const Snapshot* History::find(uint64_t id) const {
    for (const auto& s : snapshots_)
        if (s.id == id) return &s;
    return nullptr;
}

const Snapshot* History::latest() const {
    if (snapshots_.empty()) return nullptr;
    return &snapshots_.back();
}

} // namespace override
