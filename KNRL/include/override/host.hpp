#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace override {

// Fake local machine: purely simulated data, no host access.
// Dynamic stats (load) are pure functions of (seed, tick); static inventory
// (interfaces, ports) is fixed. Nothing here needs snapshotting.
struct LocalIface {
    std::string name;
    std::string addr;
    bool up = true;
};

struct LocalPort {
    int port = 0;
    std::string service;
    std::string state; // OPEN | FILTERED
};

std::string localHostname();
int localCpuCores();
int localRamMb();
std::vector<LocalIface> localInterfaces();
std::vector<LocalPort> localPorts();
// Simulated overall load 5..95, deterministic in (seed, tick).
int localLoad(uint64_t seed, uint64_t tick);
// Simulated used RAM MB, deterministic in (seed, tick, procCount).
int localRamUsed(uint64_t seed, uint64_t tick, int procCount);

} // namespace override
