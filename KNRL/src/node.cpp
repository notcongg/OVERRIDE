#include "override/node.hpp"

#include <cctype>
#include <stdexcept>

namespace override {

std::string toString(NodeState s) {
    switch (s) {
        case NodeState::ONLINE: return "ONLINE";
        case NodeState::DEGRADED: return "DEGRADED";
        case NodeState::FAILED: return "FAILED";
        case NodeState::PAUSED: return "PAUSED";
        case NodeState::RESTARTING: return "RESTARTING";
        case NodeState::CORRUPTED: return "CORRUPTED";
        case NodeState::UNKNOWN: return "UNKNOWN";
        case NodeState::WARNING: return "WARNING";
        case NodeState::CRITICAL: return "CRITICAL";
    }
    return "UNKNOWN";
}

NodeState stateFromString(const std::string& s) {
    if (s == "ONLINE") return NodeState::ONLINE;
    if (s == "DEGRADED") return NodeState::DEGRADED;
    if (s == "FAILED") return NodeState::FAILED;
    if (s == "PAUSED") return NodeState::PAUSED;
    if (s == "RESTARTING") return NodeState::RESTARTING;
    if (s == "CORRUPTED") return NodeState::CORRUPTED;
    if (s == "UNKNOWN") return NodeState::UNKNOWN;
    if (s == "WARNING") return NodeState::WARNING;
    if (s == "CRITICAL") return NodeState::CRITICAL;
    throw std::runtime_error("unknown state: " + s);
}

bool isValidNodeName(const std::string& name) {
    if (name.empty() || name.size() > 32) return false;
    if (!std::islower((unsigned char)name[0]) && !std::isalpha((unsigned char)name[0])) return false;
    // first char must be a lowercase letter
    if (name[0] < 'a' || name[0] > 'z') return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

void applyStateFixups(Node& n) {
    switch (n.state) {
        case NodeState::FAILED:
            n.health = 0;
            n.load = 0;
            n.connections = 0;
            if (n.proc != "TERMINATED") n.proc = "CRASHED";
            break;
        case NodeState::CORRUPTED:
            if (n.health > 15) n.health = 15;
            n.load = 0;
            n.connections = 0;
            if (n.proc != "TERMINATED") n.proc = "CRASHED";
            n.metadata["corrupted"] = "true";
            break;
        case NodeState::PAUSED:
            n.load = 0;
            n.connections = 0;
            n.proc = "BLOCKED";
            break;
        case NodeState::RESTARTING:
            n.proc = "RESTARTING";
            break;
        case NodeState::ONLINE:
        case NodeState::DEGRADED:
        case NodeState::WARNING:
        case NodeState::CRITICAL:
            // Serving states require a running process.
            n.proc = "RUNNING";
            if (n.state == NodeState::ONLINE) n.metadata.erase("corrupted");
            break;
        case NodeState::UNKNOWN:
            break; // no requirements; diagnostics only
    }
}

bool checkInvariants(const Node& n, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = n.name + ": " + m;
        return false;
    };
    if (!isValidNodeName(n.name)) return fail("invalid node name");
    if (n.health < 0 || n.health > 100) return fail("health out of [0,100]");
    if (n.load < 0 || n.load > 100) return fail("load out of [0,100]");
    if (n.latencyMs < 0) return fail("latency cannot be negative");
    if (n.baseLatencyMs < 0) return fail("base latency cannot be negative");
    if (n.memoryMb <= 0) return fail("memory capacity must be positive");
    if (n.memoryUsedMb < 0 || n.memoryUsedMb > n.memoryMb)
        return fail("memory usage exceeds capacity");
    if (n.connections < 0) return fail("connections cannot be negative");
    if (n.maxConnections <= 0) return fail("max connections must be positive");
    if (n.tempC < -40 || n.tempC > 250) return fail("temperature out of range");
    if (n.ambientC < -40 || n.ambientC > 80) return fail("ambient temperature out of range");
    if (n.thermalLimitC <= n.ambientC || n.criticalC <= n.thermalLimitC)
        return fail("thermal limits inconsistent (ambient < limit < critical required)");
    if (n.freqMHz < 100 || n.freqMHz > 12000) return fail("frequency out of range");
    if (n.baseFreqMHz <= 0 || n.maxFreqMHz <= 0 || n.baseFreqMHz > n.maxFreqMHz)
        return fail("frequency base/max inconsistent");
    if (n.leakMbPerTick < 0) return fail("leak rate cannot be negative");
    if (n.swapMb < 0 || n.swapUsedMb < 0 || n.swapUsedMb > n.swapMb)
        return fail("swap usage inconsistent");
    if (n.oomKills < 0) return fail("oom kill count cannot be negative");
    if (n.storageMb <= 0) return fail("storage capacity must be positive");
    if (n.storageUsedMb < 0 || n.storageUsedMb > n.storageMb)
        return fail("storage usage exceeds capacity");
    if (n.ioLoad < 0 || n.ioLoad > 100) return fail("io load out of [0,100]");
    if (n.instability < 0 || n.instability > 100) return fail("instability out of [0,100]");
    for (const auto& [m, st] : n.modules) {
        if (st != "LOADED" && st != "DEGRADED" && st != "FAILED" && st != "UNLOADED")
            return fail("module " + m + " has invalid state");
    }
    if ((n.kernel == KernelState::PANICKED || n.kernel == KernelState::HALTED) &&
        n.state != NodeState::FAILED)
        return fail("panicked/halted kernel requires node FAILED");
    if (n.kernel == KernelState::OFF &&
        n.state != NodeState::FAILED && n.state != NodeState::PAUSED)
        return fail("powered-off kernel requires node FAILED or PAUSED");
    switch (n.state) {
        case NodeState::ONLINE:
        case NodeState::DEGRADED:
        case NodeState::WARNING:
        case NodeState::CRITICAL:
            if (n.proc != "RUNNING") return fail(toString(n.state) + " requires proc RUNNING");
            break;
        case NodeState::FAILED:
            if (n.proc != "CRASHED" && n.proc != "TERMINATED")
                return fail("FAILED requires proc CRASHED/TERMINATED");
            if (n.health != 0 || n.load != 0 || n.connections != 0)
                return fail("FAILED requires health/load/connections at zero");
            break;
        case NodeState::CORRUPTED:
            if (n.proc != "CRASHED" && n.proc != "TERMINATED")
                return fail("CORRUPTED requires proc CRASHED/TERMINATED");
            if (n.health > 15 || n.load != 0 || n.connections != 0)
                return fail("CORRUPTED requires low health and zero load/connections");
            break;
        case NodeState::PAUSED:
            if (n.proc != "BLOCKED") return fail("PAUSED requires proc BLOCKED");
            if (n.load != 0 || n.connections != 0)
                return fail("PAUSED requires zero load/connections");
            break;
        case NodeState::RESTARTING:
            if (n.proc != "RESTARTING") return fail("RESTARTING requires proc RESTARTING");
            break;
        case NodeState::UNKNOWN:
            break;
    }
    return true;
}

void initNodeHardware(Node& n) {
    if (n.filesystems.empty()) {
        n.filesystems["rootfs"] = FsState::MOUNTED;
        n.filesystems["data"] = FsState::MOUNTED;
        n.filesystems["tmp"] = FsState::MOUNTED;
        n.filesystems["cache"] = FsState::MOUNTED;
    }
    if (n.modules.empty()) {
        n.modules["sched"] = "LOADED";
        n.modules["net"] = "LOADED";
        n.modules["disk"] = "LOADED";
    }
    if (n.binaries.empty()) {
        n.binaries.insert("shell");
        n.binaries.insert("ping");
        if (n.type == "client") {
            n.binaries.insert("app");
        } else if (n.type == "service") {
            n.binaries.insert("app");
            n.binaries.insert("logger");
            n.binaries.insert("rebootd");
        } else if (n.type == "database") {
            n.binaries.insert("db-engine");
            n.binaries.insert("db-write");
            n.binaries.insert("logger");
        } else if (n.type == "cache") {
            n.binaries.insert("cache-daemon");
            n.binaries.insert("logger");
        } else {
            n.binaries.insert("app");
            n.binaries.insert("logger");
        }
    }
}

} // namespace override
