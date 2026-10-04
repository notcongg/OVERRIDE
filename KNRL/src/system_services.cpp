#include "override/system.hpp"

#include <algorithm>
#include <sstream>

namespace override {

// ---- simulated services (OVERKNRL service layer) ----

namespace {
// Rootfs states that accept writes (pidfiles, logs).
bool rootfsWritableState(FsState st) {
    return st == FsState::MOUNTED || st == FsState::DEGRADED;
}

bool servingNodeState(NodeState s) {
    return s == NodeState::ONLINE || s == NodeState::WARNING || s == NodeState::DEGRADED ||
           s == NodeState::CRITICAL;
}

bool procAliveForService(ProcState s) {
    return s == ProcState::RUNNING || s == ProcState::SLEEPING || s == ProcState::DEGRADED;
}

std::string backingName(const std::string& service) {
    std::string n = "svc-" + service;
    if (n.size() > 32) n.resize(32);
    return n;
}
} // namespace

OpResult System::serviceSpawn(const std::string& name, const std::string& node,
                              const std::string& binary, const std::string& config,
                              const std::string& policy) {
    std::string low = name;
    for (auto& c : low) c = (char)std::tolower((unsigned char)c);
    if (!hasNode(node)) return OpResult::failure("unknown node: " + node);
    ServicePolicy pol = ServicePolicy::ALWAYS;
    if (!policy.empty()) {
        std::string up = policy;
        for (auto& c : up) c = (char)std::toupper((unsigned char)c);
        try {
            pol = servicePolicyFromString(up);
        } catch (const std::exception&) {
            return OpResult::failure("unknown policy '" + policy + "' (ALWAYS|NEVER)");
        }
    }
    uint64_t root = emit(0, "USER_SERVICE_SPAWN", "user", node,
                         "service spawn " + low + " on " + node + " (" + binary + ")");
    std::string err;
    if (!svc_.spawn(low, node, binary, config, pol, clock_.tickCount(), err)) {
        emit(root, "SERVICE_REJECTED", "engine", node, err);
        return OpResult::failure(err, root);
    }
    emit(root, "SERVICE_SPAWNED", "engine", node,
         "service " + low + " declared on " + node + " [" + toString(pol) + "]" +
             (config.empty() ? "" : " config=" + config));
    return OpResult::success(root, "spawned service " + low + " on " + node);
}

OpResult System::serviceStartAs(const std::string& name, uint64_t cause, bool resetCrashes) {
    const Service* found = svc_.find(name);
    if (!found) return OpResult::failure("unknown service: " + name);
    if (!hasNode(found->node)) return OpResult::failure("service host gone: " + found->node);
    Node& n = get(found->node);
    auto fail = [&](const std::string& type, const std::string& reason, const std::string& sev) {
        emit(cause, type, found->node, found->node,
             "service " + name + " start failed: " + reason,
             {{"service", name}, {"reason", reason}}, sev);
        return OpResult::failure("service " + name + " cannot start: " + reason, cause);
    };
    if (n.kernel == KernelState::PANICKED || n.kernel == KernelState::HALTED ||
        n.kernel == KernelState::OFF)
        return fail("SERVICE_START_FAILED", "kernel " + toString(n.kernel), "FAILED");
    if (n.state == NodeState::FAILED || n.state == NodeState::CORRUPTED)
        return fail("SERVICE_START_FAILED", "host " + toString(n.state), "FAILED");
    if (n.state == NodeState::PAUSED || n.state == NodeState::RESTARTING)
        return fail("SERVICE_START_FAILED", "host " + toString(n.state), "WARNING");
    std::string binErr;
    std::string binPath = resolveBinary(found->node, found->binary, binErr);
    if (binPath.empty()) return fail("SERVICE_START_FAILED", binErr, "WARNING");
    if (!binPath.empty() && binPath[0] == '/') {
        const VFile* bf = vfs_.file(binPath);
        std::string why;
        if (bf && !vfsAuthorize(bf->owner, bf->acl, false, why))
            return fail("SERVICE_START_FAILED",
                        "permission denied (" + why + " on " + binPath + ")", "WARNING");
    }
    if (!found->config.empty()) {
        const VFile* f = vfs_.file(found->config);
        if (!f) return fail("SERVICE_START_FAILED", "missing config " + found->config, "WARNING");
        if (f->corrupted)
            return fail("SERVICE_START_FAILED", "config corrupted: " + found->config, "WARNING");
    }
    auto rfit = n.filesystems.find("rootfs");
    FsState rootfs = (rfit == n.filesystems.end()) ? FsState::MOUNTED : rfit->second;
    if (!rootfsWritableState(rootfs))
        return fail("SERVICE_START_FAILED", "rootfs " + toString(rootfs), "WARNING");
    // Reuse a live backing proc, else spawn one.
    int pid = found->pid;
    const SimProcess* bp = (pid > 0) ? pm_.find(std::to_string(pid)) : nullptr;
    bool live = bp && procAliveForService(bp->state) && bp->host == found->node;
    std::vector<ProcNote> notes;
    std::string err;
    if (!live) {
        pid = pm_.spawn(backingName(name), "service", found->node, 0, notes, err);
        if (pid == 0) return fail("SERVICE_START_FAILED", "proc spawn failed: " + err, "WARNING");
        noteSyscall("clone");
        emitProcNotes(notes, cause);
    }
    Service* mut = svc_.find(name);
    mut->pid = pid;
    mut->state = ServiceState::RUNNING;
    mut->lastChange = clock_.tickCount();
    if (resetCrashes) mut->crashCount = 0;
    emit(cause, "SERVICE_STARTED", found->node, found->node,
         "service " + name + " RUNNING on " + found->node + " (pid " + std::to_string(pid) + ")",
         {{"service", name}, {"pid", std::to_string(pid)}});
    propagate(cause);
    return OpResult::success(cause, "service " + name + " started (pid " +
                                        std::to_string(pid) + ")");
}

OpResult System::serviceStart(const std::string& name) {
    if (!svc_.find(name)) return OpResult::failure("unknown service: " + name);
    const Service* s = svc_.find(name);
    uint64_t root = emit(0, "USER_SERVICE_START", "user", s->node, "service start " + name);
    OpResult r = serviceStartAs(name, root, true);
    if (!r.ok) return r;
    return OpResult::success(root, r.info);
}

OpResult System::serviceStop(const std::string& name) {
    const Service* found = svc_.find(name);
    if (!found) return OpResult::failure("unknown service: " + name);
    if (found->state == ServiceState::STOPPED)
        return OpResult::failure("service already STOPPED: " + name);
    uint64_t root =
        emit(0, "USER_SERVICE_STOP", "user", found->node, "service stop " + name);
    // Park the backing proc too (orderly stop, resumable).
    const SimProcess* bp =
        (found->pid > 0) ? pm_.find(std::to_string(found->pid)) : nullptr;
    if (bp && (bp->state == ProcState::RUNNING || bp->state == ProcState::SLEEPING)) {
        std::vector<ProcNote> notes;
        std::string err;
        std::string who = std::to_string(bp->pid);
        pm_.pause(who, notes, err);
        emitProcNotes(notes, root);
    }
    Service* mut = svc_.find(name);
    mut->state = ServiceState::STOPPED;
    mut->lastChange = clock_.tickCount();
    emit(root, "SERVICE_STOPPED", mut->node, mut->node, "service " + name + " STOPPED");
    propagate(root);
    return OpResult::success(root, "service " + name + " stopped");
}

OpResult System::serviceRestart(const std::string& name) {
    if (!svc_.find(name)) return OpResult::failure("unknown service: " + name);
    const Service* s = svc_.find(name);
    uint64_t root = emit(0, "USER_SERVICE_RESTART", "user", s->node, "service restart " + name);
    OpResult r = serviceStartAs(name, root, true);
    if (!r.ok) return r;
    return OpResult::success(root, r.info);
}

OpResult System::serviceRemove(const std::string& name) {
    const Service* found = svc_.find(name);
    if (!found) return OpResult::failure("unknown service: " + name);
    uint64_t root =
        emit(0, "USER_SERVICE_REMOVE", "user", found->node, "service remove " + name);
    std::string err;
    if (!svc_.remove(name, err)) return OpResult::failure(err, root);
    emit(root, "SERVICE_REMOVED", "engine", found->node, "service " + name + " removed");
    return OpResult::success(root, "service " + name + " removed");
}

std::string System::serviceList() const {
    std::ostringstream o;
    o << "NAME            NODE      BINARY      POLICY  STATE     PID   CRASHES\n";
    for (const auto& [name, s] : svc_.all()) {
        o << name << std::string(name.size() < 16 ? 16 - name.size() : 1, ' ') << s.node
          << std::string(s.node.size() < 10 ? 10 - s.node.size() : 1, ' ') << s.binary
          << std::string(s.binary.size() < 12 ? 12 - s.binary.size() : 1, ' ')
          << toString(s.policy)
          << std::string(toString(s.policy).size() < 8 ? 8 - toString(s.policy).size() : 1, ' ')
          << toString(s.state)
          << std::string(toString(s.state).size() < 10 ? 10 - toString(s.state).size() : 1, ' ')
          << s.pid << "    " << s.crashCount << "\n";
    }
    if (svc_.all().empty()) o << "(no services; spawn one with `service spawn`)";
    return o.str();
}

std::string System::serviceInspect(const std::string& name) const {
    const Service* s = svc_.find(name);
    if (!s) return "unknown service: " + name;
    std::ostringstream o;
    o << "service " << s->name << "\n";
    o << "  host:     " << s->node;
    if (hasNode(s->node)) o << " [" << toString(get(s->node).state) << "]";
    o << "\n";
    o << "  binary:   " << s->binary << "\n";
    o << "  config:   " << (s->config.empty() ? "(none)" : s->config) << "\n";
    o << "  policy:   " << toString(s->policy) << "\n";
    o << "  state:    " << toString(s->state) << "\n";
    o << "  pid:      ";
    const SimProcess* bp = (s->pid > 0) ? processes().find(std::to_string(s->pid)) : nullptr;
    if (bp) o << bp->pid << " (" << toString(bp->state) << ")\n";
    else o << "(none)\n";
    o << "  crashes:  " << s->crashCount << "\n";
    return o.str();
}

void System::serviceTick(uint64_t cause) {
    // Reconcile every service in deterministic name order.
    for (const auto& [name, svc] : svc_.all()) {
        if (!hasNode(svc.node)) {
            if (svc.state == ServiceState::RUNNING) {
                Service* mut = const_cast<Service*>(svc_.find(name));
                mut->state = ServiceState::CRASHED;
                mut->crashCount += 1;
                mut->lastChange = clock_.tickCount();
                emit(cause, "SERVICE_CRASHED", "engine", svc.node,
                     "service " + name + " CRASHED: host gone",
                     {{"service", name}}, "CRITICAL");
            }
            continue;
        }
        const Node& n = get(svc.node);
        bool hostDown = (n.state == NodeState::FAILED || n.state == NodeState::CORRUPTED ||
                         n.kernel == KernelState::PANICKED || n.kernel == KernelState::HALTED ||
                         n.kernel == KernelState::OFF);
        if (svc.state == ServiceState::RUNNING) {
            const SimProcess* bp =
                (svc.pid > 0) ? pm_.find(std::to_string(svc.pid)) : nullptr;
            bool alive = bp && procAliveForService(bp->state) && bp->host == svc.node;
            if (hostDown || !alive) {
                Service* mut = svc_.find(name);
                mut->state = ServiceState::CRASHED;
                mut->crashCount += 1;
                mut->lastChange = clock_.tickCount();
                emit(cause, "SERVICE_CRASHED", svc.node, svc.node,
                     "service " + name + " CRASHED: " +
                         std::string(hostDown ? "host down" : "backing proc dead"),
                     {{"service", name}}, "CRITICAL");
                if (mut->crashCount >= 3) {
                    raiseFault(svc.node, "restart-loop", Severity::WARNING, cause,
                               "service " + name + " crashed " +
                                   std::to_string(mut->crashCount) + " times");
                }
            }
            continue;
        }
        // STOPPED/CRASHED + ALWAYS: the supervisor tries to (re)start on a
        // serving host with a live kernel. Failures stay CRASHED (counted).
        bool servingHost =
            servingNodeState(n.state) && n.kernel == KernelState::RUNNING;
        if (svc.policy == ServicePolicy::ALWAYS && servingHost) {
            OpResult r = serviceStartAs(name, cause, false);
            if (!r.ok) {
                Service* mut = svc_.find(name);
                if (mut->state != ServiceState::CRASHED) {
                    mut->state = ServiceState::CRASHED;
                    mut->lastChange = clock_.tickCount();
                }
                mut->crashCount += 1;
                if (mut->crashCount >= 3) {
                    raiseFault(svc.node, "restart-loop", Severity::WARNING, cause,
                               "service " + name + " keeps failing to start");
                }
            }
        }
    }
    // Node coupling: an ALWAYS service that is not RUNNING degrades its host
    // through a service-down fault (resolved when all are healthy again).
    for (const auto& nodeName : nodeNames()) {
        bool need = false;
        std::string worst;
        for (const auto& [name, s] : svc_.all()) {
            if (s.node == nodeName && s.policy == ServicePolicy::ALWAYS &&
                s.state != ServiceState::RUNNING) {
                need = true;
                worst = name;
                break;
            }
        }
        Node& n = get(nodeName);
        bool have = n.faults.count("service-down") > 0;
        if (need && !have) {
            raiseFault(nodeName, "service-down", Severity::WARNING, cause,
                       "service " + worst + " down");
        } else if (!need && have) {
            n.faults.erase("service-down");
            emit(cause, "FAULT_RESOLVED", "engine", nodeName,
                 nodeName + " fault 'service-down' resolved (services healthy)",
                 {{"fault", "service-down"}});
        }
    }
}

} // namespace override
