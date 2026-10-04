#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "override/fault.hpp"

namespace override {

enum class NodeState {
    ONLINE,
    DEGRADED,
    FAILED,
    PAUSED,
    RESTARTING,
    CORRUPTED,
    UNKNOWN,
    WARNING,
    CRITICAL
};

std::string toString(NodeState s);
NodeState stateFromString(const std::string& s); // throws on unknown

struct Node {
    std::string name;
    std::string type = "service"; // service | database | cache | client ...
    NodeState state = NodeState::ONLINE;
    int health = 100;      // 0..100
    int latencyMs = 20;    // effective latency
    int baseLatencyMs = 20;
    int load = 10;         // 0..100 %
    int memoryMb = 512;
    int memoryUsedMb = 64;
    int connections = 10;
    int maxConnections = 256;
    // Thermal model (simulated machine).
    int tempC = 35;
    int ambientC = 25;
    int thermalLimitC = 85;
    int criticalC = 105;
    bool throttled = false;
    // Clock model (simulated frequency).
    int freqMHz = 2400;
    int baseFreqMHz = 2400;
    int maxFreqMHz = 3600;
    ClockState clockState = ClockState::STABLE;
    // Memory detail.
    bool memCorrupt = false;
    int leakMbPerTick = 0;
    int swapMb = 1024;
    int swapUsedMb = 0;
    int oomKills = 0;
    // Storage model.
    int storageMb = 8192;
    int storageUsedMb = 1024;
    int ioLoad = 5; // 0..100
    std::map<std::string, FsState> filesystems; // mount -> state
    // Kernel model.
    KernelState kernel = KernelState::RUNNING;
    int instability = 0; // 0..100, 100 = panic threshold
    std::map<std::string, std::string> modules; // name -> OK|DEGRADED|FAILED
    std::set<std::string> binaries;             // installed executables for `run`
    // Identity model (simulated privileges).
    PrivState priv = PrivState::ROOT;
    // Configuration health.
    bool configCorrupt = false;
    // Stacked active faults, keyed by fault name (coexisting, escalating).
    std::map<std::string, ActiveFault> faults;
    std::vector<std::string> dependencies; // names of nodes this node needs
    std::map<std::string, std::string> metadata;
    // Deception: beliefs held BY this node: beliefs[target][prop] = fake value
    std::map<std::string, std::map<std::string, std::string>> beliefs;
    // Simulated process lifecycle (PROCESS mode)
    std::string proc = "RUNNING"; // SPAWNED RUNNING BLOCKED CRASHED RESTARTING TERMINATED
};

// Node names: lowercase, start with letter, [a-z0-9_-], max 32 chars.
bool isValidNodeName(const std::string& name);

// Bring proc/counters in line with the node's state after a state change.
// This is the single place where state-implied fields are set.
void applyStateFixups(Node& n);

// Verify all node invariants (ranges + state/proc consistency).
// Returns true when valid; otherwise fills *err (may be null).
bool checkInvariants(const Node& n, std::string* err);

// Fill simulated-hardware defaults (filesystems, modules, binaries).
// Called for fresh nodes; snapshots preserve whatever is stored.
void initNodeHardware(Node& n);

} // namespace override
