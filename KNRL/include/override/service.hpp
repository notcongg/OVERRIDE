#pragma once

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace override {

// Simulated service states (OVERKNRL service layer; no host interaction).
enum class ServiceState { RUNNING, STOPPED, CRASHED };

std::string toString(ServiceState s);
ServiceState serviceStateFromString(const std::string& s); // throws on unknown

// Restart policy: ALWAYS tries to keep the service up (tick + repair/reboot
// attempt restarts); NEVER leaves it stopped until an operator starts it.
enum class ServicePolicy { ALWAYS, NEVER };

std::string toString(ServicePolicy p);
ServicePolicy servicePolicyFromString(const std::string& s); // throws on unknown

struct Service {
    std::string name;
    std::string node;              // host node
    std::string binary;            // must exist in node.binaries to start
    std::string config;            // vfs path or "" (must exist + uncorrupted)
    ServicePolicy policy = ServicePolicy::ALWAYS;
    ServiceState state = ServiceState::STOPPED;
    int pid = 0;                   // backing simulated process (0 = none)
    int crashCount = 0;            // consecutive failures; reset by manual start
    uint64_t lastChange = 0;       // sim tick of last transition
};

// Service names follow node naming rules: lowercase, [a-z0-9_-], max 32.
bool isValidServiceName(const std::string& name);

// Dumb store: validation + state transitions only. Gating (binaries, configs,
// rootfs, kernel) and event emission live in the simulation engine, which
// also owns tick reconciliation and node coupling.
class ServiceManager {
public:
    ServiceManager() = default;

    const std::map<std::string, Service>& all() const { return services_; }
    const Service* find(const std::string& name) const;
    Service* find(const std::string& name);
    void clear() { services_.clear(); }
    void restoreSnapshot(const std::map<std::string, Service>& services) {
        services_ = services;
    }

    bool spawn(const std::string& name, const std::string& node, const std::string& binary,
               const std::string& config, ServicePolicy policy, uint64_t tick,
               std::string& err);
    // Force a state (engine has already validated preconditions).
    // Returns false + err when the transition is illegal.
    bool setState(const std::string& name, ServiceState to, uint64_t tick, std::string& err);
    bool remove(const std::string& name, std::string& err);
    // Bulk host coupling. Returns affected service names (sorted).
    // crashHost: RUNNING/STOPPED -> CRASHED. stopHost: non-STOPPED -> STOPPED.
    std::vector<std::string> crashHost(const std::string& host, uint64_t tick);
    std::vector<std::string> stopHost(const std::string& host, uint64_t tick);
    std::vector<std::string> purgeHost(const std::string& host);

    bool validateAll(std::string* err) const;
    void appendDigest(std::ostringstream& o) const;

private:
    std::map<std::string, Service> services_; // sorted: deterministic order
};

} // namespace override
