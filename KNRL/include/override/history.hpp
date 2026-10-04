#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "override/hardware.hpp"
#include "override/node.hpp"
#include "override/network.hpp"
#include "override/process.hpp"
#include "override/service.hpp"
#include "override/vfs.hpp"

namespace override {

struct Snapshot {
    uint64_t id = 0;
    uint64_t tick = 0;
    bool clockPaused = false;
    uint64_t seed = 0;
    uint64_t rng = 0;
    bool autoFaults = true; // autonomous failure simulation on/off
    int autoRate = 8;       // base probability % per node per tick
    int nextPid = 101;
    std::string label;
    std::map<std::string, Node> nodes;
    std::vector<Link> links;
    std::map<int, SimProcess> procs;
    std::map<std::string, Service> services;
    HardwareManager hw;
    Vfs vfs;
    std::string cwd = "/";
    size_t eventCount = 0; // ledger size at snapshot time (informational; ledger is append-only)
};

class History {
public:
    void pushCommand(const std::string& line) { commands_.push_back(line); }
    const std::vector<std::string>& commands() const { return commands_; }

    Snapshot& take(uint64_t tick, bool clockPaused, uint64_t seed, uint64_t rng, bool autoFaults,
                   int autoRate, int nextPid, std::string label,
                   const std::map<std::string, Node>& nodes, const std::vector<Link>& links,
                   const std::map<int, SimProcess>& procs,
                   const std::map<std::string, Service>& services, const HardwareManager& hw,
                   const Vfs& vfs, const std::string& cwd, size_t eventCount);
    const Snapshot* find(uint64_t id) const;
    const Snapshot* latest() const;
    const std::vector<Snapshot>& snapshots() const { return snapshots_; }
    void clear() { commands_.clear(); snapshots_.clear(); nextId_ = 1; }

private:
    std::vector<std::string> commands_;
    std::vector<Snapshot> snapshots_;
    uint64_t nextId_ = 1;
};

} // namespace override
