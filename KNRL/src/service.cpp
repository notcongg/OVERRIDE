#include "override/service.hpp"

#include <cctype>
#include <stdexcept>

namespace override {

std::string toString(ServiceState s) {
    switch (s) {
        case ServiceState::RUNNING: return "RUNNING";
        case ServiceState::STOPPED: return "STOPPED";
        case ServiceState::CRASHED: return "CRASHED";
    }
    return "UNKNOWN";
}

ServiceState serviceStateFromString(const std::string& s) {
    if (s == "RUNNING") return ServiceState::RUNNING;
    if (s == "STOPPED") return ServiceState::STOPPED;
    if (s == "CRASHED") return ServiceState::CRASHED;
    throw std::runtime_error("unknown service state: " + s);
}

std::string toString(ServicePolicy p) {
    return p == ServicePolicy::ALWAYS ? "ALWAYS" : "NEVER";
}

ServicePolicy servicePolicyFromString(const std::string& s) {
    if (s == "ALWAYS") return ServicePolicy::ALWAYS;
    if (s == "NEVER") return ServicePolicy::NEVER;
    throw std::runtime_error("unknown service policy: " + s + " (ALWAYS|NEVER)");
}

bool isValidServiceName(const std::string& name) {
    if (name.empty() || name.size() > 32) return false;
    if (name[0] < 'a' || name[0] > 'z') return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

const Service* ServiceManager::find(const std::string& name) const {
    auto it = services_.find(name);
    return it == services_.end() ? nullptr : &it->second;
}

Service* ServiceManager::find(const std::string& name) {
    auto it = services_.find(name);
    return it == services_.end() ? nullptr : &it->second;
}

bool ServiceManager::spawn(const std::string& name, const std::string& node,
                           const std::string& binary, const std::string& config,
                           ServicePolicy policy, uint64_t tick, std::string& err) {
    if (!isValidServiceName(name)) {
        err = "invalid service name '" + name + "' (lowercase, [a-z0-9_-], max 32)";
        return false;
    }
    if (services_.count(name)) {
        err = "service already exists: " + name;
        return false;
    }
    if (binary.empty()) {
        err = "service needs a binary";
        return false;
    }
    Service s;
    s.name = name;
    s.node = node;
    s.binary = binary;
    s.config = config;
    s.policy = policy;
    s.state = ServiceState::STOPPED;
    s.lastChange = tick;
    services_[name] = s;
    (void)err;
    return true;
}

bool ServiceManager::setState(const std::string& name, ServiceState to, uint64_t tick,
                              std::string& err) {
    auto it = services_.find(name);
    if (it == services_.end()) {
        err = "unknown service: " + name;
        return false;
    }
    it->second.state = to;
    it->second.lastChange = tick;
    return true;
}

bool ServiceManager::remove(const std::string& name, std::string& err) {
    if (!services_.erase(name)) {
        err = "unknown service: " + name;
        return false;
    }
    return true;
}

std::vector<std::string> ServiceManager::crashHost(const std::string& host, uint64_t tick) {
    std::vector<std::string> out;
    for (auto& [name, s] : services_) {
        if (s.node != host || s.state == ServiceState::CRASHED) continue;
        s.state = ServiceState::CRASHED;
        s.lastChange = tick;
        out.push_back(name);
    }
    return out;
}

std::vector<std::string> ServiceManager::stopHost(const std::string& host, uint64_t tick) {
    std::vector<std::string> out;
    for (auto& [name, s] : services_) {
        if (s.node != host || s.state == ServiceState::STOPPED) continue;
        s.state = ServiceState::STOPPED;
        s.lastChange = tick;
        out.push_back(name);
    }
    return out;
}

std::vector<std::string> ServiceManager::purgeHost(const std::string& host) {
    std::vector<std::string> out;
    for (auto it = services_.begin(); it != services_.end();) {
        if (it->second.node == host) {
            out.push_back(it->first);
            it = services_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

bool ServiceManager::validateAll(std::string* err) const {
    for (const auto& [name, s] : services_) {
        if (name != s.name) {
            if (err) *err = "service key/name mismatch: " + name;
            return false;
        }
        if (!isValidServiceName(name)) {
            if (err) *err = "invalid service name: " + name;
            return false;
        }
        if (s.binary.empty()) {
            if (err) *err = name + ": service needs a binary";
            return false;
        }
        if (s.pid < 0 || s.crashCount < 0) {
            if (err) *err = name + ": negative pid/crashCount";
            return false;
        }
    }
    return true;
}

void ServiceManager::appendDigest(std::ostringstream& o) const {
    for (const auto& [name, s] : services_) {
        o << "svc(" << name << "," << s.node << "," << s.binary << "," << s.config << ","
          << toString(s.policy) << "," << toString(s.state) << "," << s.pid << "," << s.crashCount
          << "," << s.lastChange << ");";
    }
}

} // namespace override
