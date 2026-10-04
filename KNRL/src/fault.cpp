#include "override/fault.hpp"

#include <algorithm>
#include <stdexcept>

namespace override {

std::string toString(Severity s) {
    switch (s) {
        case Severity::INFO: return "INFO";
        case Severity::WARNING: return "WARNING";
        case Severity::DEGRADED: return "DEGRADED";
        case Severity::CRITICAL: return "CRITICAL";
        case Severity::FAILED: return "FAILED";
        case Severity::PANIC: return "PANIC";
        case Severity::HALTED: return "HALTED";
    }
    return "INFO";
}

Severity severityFromString(const std::string& s) {
    if (s == "INFO") return Severity::INFO;
    if (s == "WARNING") return Severity::WARNING;
    if (s == "DEGRADED") return Severity::DEGRADED;
    if (s == "CRITICAL") return Severity::CRITICAL;
    if (s == "FAILED") return Severity::FAILED;
    if (s == "PANIC") return Severity::PANIC;
    if (s == "HALTED") return Severity::HALTED;
    throw std::runtime_error("unknown severity: " + s);
}

int severityRank(Severity s) {
    switch (s) {
        case Severity::INFO: return 0;
        case Severity::WARNING: return 1;
        case Severity::DEGRADED: return 2;
        case Severity::CRITICAL: return 3;
        case Severity::FAILED: return 4;
        case Severity::PANIC: return 5;
        case Severity::HALTED: return 6;
    }
    return 0;
}

std::string toString(FaultKind k) {
    switch (k) {
        case FaultKind::THERMAL: return "THERMAL";
        case FaultKind::CLOCK: return "CLOCK";
        case FaultKind::POWER: return "POWER";
        case FaultKind::MEMORY: return "MEMORY";
        case FaultKind::STORAGE: return "STORAGE";
        case FaultKind::FILESYSTEM: return "FILESYSTEM";
        case FaultKind::ROOTFS: return "ROOTFS";
        case FaultKind::KERNEL: return "KERNEL";
        case FaultKind::MODULE: return "MODULE";
        case FaultKind::PROCESS: return "PROCESS";
        case FaultKind::SERVICE: return "SERVICE";
        case FaultKind::NETWORK: return "NETWORK";
        case FaultKind::CONFIG: return "CONFIG";
        case FaultKind::PERMISSION: return "PERMISSION";
        case FaultKind::COMMAND: return "COMMAND";
        case FaultKind::RESOURCE: return "RESOURCE";
        case FaultKind::DEPENDENCY: return "DEPENDENCY";
    }
    return "THERMAL";
}

FaultKind faultKindFromString(const std::string& s) {
    if (s == "THERMAL") return FaultKind::THERMAL;
    if (s == "CLOCK") return FaultKind::CLOCK;
    if (s == "POWER") return FaultKind::POWER;
    if (s == "MEMORY") return FaultKind::MEMORY;
    if (s == "STORAGE") return FaultKind::STORAGE;
    if (s == "FILESYSTEM") return FaultKind::FILESYSTEM;
    if (s == "ROOTFS") return FaultKind::ROOTFS;
    if (s == "KERNEL") return FaultKind::KERNEL;
    if (s == "MODULE") return FaultKind::MODULE;
    if (s == "PROCESS") return FaultKind::PROCESS;
    if (s == "SERVICE") return FaultKind::SERVICE;
    if (s == "NETWORK") return FaultKind::NETWORK;
    if (s == "CONFIG") return FaultKind::CONFIG;
    if (s == "PERMISSION") return FaultKind::PERMISSION;
    if (s == "COMMAND") return FaultKind::COMMAND;
    if (s == "RESOURCE") return FaultKind::RESOURCE;
    if (s == "DEPENDENCY") return FaultKind::DEPENDENCY;
    throw std::runtime_error("unknown fault kind: " + s);
}

std::string toString(ClockState s) {
    switch (s) {
        case ClockState::STABLE: return "STABLE";
        case ClockState::THROTTLED: return "THROTTLED";
        case ClockState::UNSTABLE: return "UNSTABLE";
        case ClockState::CRITICAL: return "CRITICAL";
    }
    return "STABLE";
}

ClockState clockStateFromString(const std::string& s) {
    if (s == "STABLE") return ClockState::STABLE;
    if (s == "THROTTLED") return ClockState::THROTTLED;
    if (s == "UNSTABLE") return ClockState::UNSTABLE;
    if (s == "CRITICAL") return ClockState::CRITICAL;
    throw std::runtime_error("unknown clock state: " + s);
}

std::string toString(KernelState s) {
    switch (s) {
        case KernelState::RUNNING: return "RUNNING";
        case KernelState::WARNING: return "WARNING";
        case KernelState::DEGRADED: return "DEGRADED";
        case KernelState::UNSTABLE: return "UNSTABLE";
        case KernelState::BOOTING: return "BOOTING";
        case KernelState::PANICKED: return "PANICKED";
        case KernelState::HALTED: return "HALTED";
        case KernelState::OFF: return "OFF";
    }
    return "RUNNING";
}

KernelState kernelStateFromString(const std::string& s) {
    if (s == "RUNNING") return KernelState::RUNNING;
    if (s == "WARNING") return KernelState::WARNING;
    if (s == "DEGRADED") return KernelState::DEGRADED;
    if (s == "UNSTABLE") return KernelState::UNSTABLE;
    if (s == "BOOTING") return KernelState::BOOTING;
    if (s == "PANICKED") return KernelState::PANICKED;
    if (s == "HALTED") return KernelState::HALTED;
    if (s == "OFF") return KernelState::OFF;
    throw std::runtime_error("unknown kernel state: " + s);
}

std::string toString(PrivState s) {
    switch (s) {
        case PrivState::ROOT: return "ROOT";
        case PrivState::USER: return "USER";
        case PrivState::LOCKED: return "LOCKED";
        case PrivState::RESTRICTED: return "RESTRICTED";
        case PrivState::CORRUPTED: return "CORRUPTED";
    }
    return "ROOT";
}

PrivState privStateFromString(const std::string& s) {
    if (s == "ROOT") return PrivState::ROOT;
    if (s == "USER") return PrivState::USER;
    if (s == "LOCKED") return PrivState::LOCKED;
    if (s == "RESTRICTED") return PrivState::RESTRICTED;
    if (s == "CORRUPTED") return PrivState::CORRUPTED;
    throw std::runtime_error("unknown privilege state: " + s);
}

std::string toString(FsState s) {
    switch (s) {
        case FsState::MOUNTED: return "MOUNTED";
        case FsState::DEGRADED: return "DEGRADED";
        case FsState::READ_ONLY: return "READ_ONLY";
        case FsState::CORRUPTED: return "CORRUPTED";
        case FsState::UNMOUNTED: return "UNMOUNTED";
        case FsState::FAILED: return "FAILED";
    }
    return "MOUNTED";
}

FsState fsStateFromString(const std::string& s) {
    if (s == "MOUNTED") return FsState::MOUNTED;
    if (s == "DEGRADED") return FsState::DEGRADED;
    if (s == "READ_ONLY") return FsState::READ_ONLY;
    if (s == "CORRUPTED") return FsState::CORRUPTED;
    if (s == "UNMOUNTED") return FsState::UNMOUNTED;
    if (s == "FAILED") return FsState::FAILED;
    throw std::runtime_error("unknown filesystem state: " + s);
}

namespace {
const std::vector<FaultDef>& faultTable() {
    static const std::vector<FaultDef> table = {
        {"overheat", FaultKind::THERMAL, Severity::WARNING, "thermal load rising toward limit"},
        {"cooling-fail", FaultKind::THERMAL, Severity::DEGRADED, "cooling ineffective, heat builds fast"},
        {"clock-unstable", FaultKind::CLOCK, Severity::WARNING, "clock jitter, calculation errors"},
        {"clock-boost", FaultKind::CLOCK, Severity::WARNING, "frequency above safe maximum"},
        {"power-fault", FaultKind::POWER, Severity::WARNING, "power instability disturbs clock"},
        {"mem-leak", FaultKind::MEMORY, Severity::WARNING, "memory grows every tick"},
        {"mem-corrupt", FaultKind::MEMORY, Severity::DEGRADED, "corrupted pages crash processes"},
        {"oom", FaultKind::MEMORY, Severity::CRITICAL, "immediate out-of-memory kill"},
        {"alloc-fail", FaultKind::MEMORY, Severity::WARNING, "allocations start failing"},
        {"disk-full", FaultKind::STORAGE, Severity::WARNING, "storage nearly exhausted"},
        {"io-error", FaultKind::STORAGE, Severity::DEGRADED, "I/O timeouts and errors"},
        {"fs-corrupt", FaultKind::FILESYSTEM, Severity::DEGRADED, "filesystem corruption"},
        {"rootfs-failure", FaultKind::FILESYSTEM, Severity::CRITICAL, "root fs full + read-only"},
        {"rootfs-corruption", FaultKind::ROOTFS, Severity::CRITICAL, "root filesystem corrupted"},
        {"service-crash", FaultKind::SERVICE, Severity::DEGRADED, "service process crashed"},
        {"service-down", FaultKind::SERVICE, Severity::WARNING,
         "required service not running (engine-raised)", false},
        {"restart-loop", FaultKind::PROCESS, Severity::WARNING,
         "service keeps crashing on start (engine-raised)", false},
        {"rootfs-readonly", FaultKind::FILESYSTEM, Severity::WARNING, "root remounted read-only"},
        {"mount-fail", FaultKind::FILESYSTEM, Severity::DEGRADED, "mount unavailable"},
        {"kernel-panic", FaultKind::KERNEL, Severity::PANIC, "immediate kernel panic"},
        {"kernel-unstable", FaultKind::KERNEL, Severity::DEGRADED, "kernel instability grows"},
        {"boot-failure", FaultKind::KERNEL, Severity::CRITICAL,
         "boot failed: prerequisite unmet (engine-raised)", false},
        {"kernel-config", FaultKind::CONFIG, Severity::WARNING,
         "kernel configuration corrupted (engine-raised)", false},
        {"module-fail", FaultKind::KERNEL, Severity::WARNING, "kernel module failure"},
        {"module-load", FaultKind::MODULE, Severity::INFO, "load an unloaded kernel module"},
        {"module-unload", FaultKind::MODULE, Severity::WARNING, "unload a kernel module"},
        {"proc-crash", FaultKind::PROCESS, Severity::WARNING, "crash a process on the node"},
        {"proc-runaway", FaultKind::PROCESS, Severity::WARNING, "CPU runaway process"},
        {"proc-deadlock", FaultKind::PROCESS, Severity::DEGRADED, "deadlocked process blocks work"},
        {"link-failure", FaultKind::NETWORK, Severity::DEGRADED, "link to a peer goes down"},
        {"iface-down", FaultKind::NETWORK, Severity::DEGRADED, "network interface down"},
        {"net-congest", FaultKind::NETWORK, Severity::WARNING, "link congestion spike"},
        {"conn-exhaust", FaultKind::NETWORK, Severity::WARNING, "connection table exhaustion"},
        {"config-corrupt", FaultKind::CONFIG, Severity::WARNING, "corrupted configuration"},
        {"perm-lock", FaultKind::PERMISSION, Severity::WARNING, "privileges locked"},
        {"perm-corrupt", FaultKind::PERMISSION, Severity::DEGRADED, "identity state corrupted"},
    };
    return table;
}

std::string canonFault(const std::string& s) {
    std::string o = s;
    for (auto& c : o) {
        c = (char)std::tolower((unsigned char)c);
        if (c == '_') c = '-';
    }
    // Common aliases for scenario-style fault names.
    if (o == "memory-pressure") return "mem-leak";
    if (o == "memory-corruption") return "mem-corrupt";
    if (o == "kernel-instability") return "kernel-unstable";
    if (o == "diskfull") return "disk-full";
    if (o == "filesystem-corruption") return "fs-corrupt";
    if (o == "cpu-overheat" || o == "overheating") return "overheat";
    if (o == "thermal") return "overheat";
    if (o == "power") return "power-fault";
    if (o == "rootfs") return "rootfs-failure";
    return o;
}
} // namespace

const FaultDef* faultDef(const std::string& name) {
    std::string c = canonFault(name);
    for (const auto& d : faultTable()) {
        if (d.name == c) return &d;
    }
    return nullptr;
}

std::vector<std::string> faultNames() {
    std::vector<std::string> out;
    for (const auto& d : faultTable()) out.push_back(d.name);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace override
