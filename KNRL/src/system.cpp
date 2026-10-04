// System (world state, transitions, dependencies, properties, determinism digests). Split from system.cpp; behavior unchanged.
#include "override/system.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iomanip>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>

#include "detail.hpp"

namespace override {
using namespace detail;

System::System() : rng_(seed_) { initDefaultWorld(); }

std::string System::upper(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)std::toupper((unsigned char)c);
    return o;
}

void System::initDefaultWorld() {
    std::string err;
    initWorldPipeline({}, err); // defaults always validate; err unused
}

bool System::initWorldPipeline(ProgressCb progress, std::string& err) {
    static const int kStages = 7;
    auto stage = [&](int done) {
        if (progress) progress(done, kStages);
    };
    // 1. Identity: clear every subsystem back to construction defaults.
    nodes_.clear();
    net_.clear();
    events_.clear();
    history_.clear();
    clock_.reset();
    mode_ = "NORMAL";
    seed_ = 0x9E3779B97F4A7C15ull;
    rng_ = seed_;
    autoFaults_ = true;
    autoRate_ = 8;
    pm_.clear();
    svc_.clear();
    hw_ = HardwareManager();
    cwd_ = "/";
    stage(1);

    Node client, server, database, cache;
    client.name = "client";
    client.type = "client";
    client.state = NodeState::ONLINE;
    client.health = 95;
    client.latencyMs = 15;
    client.baseLatencyMs = 15;
    client.load = 20;
    client.memoryMb = 256;
    client.memoryUsedMb = 48;
    client.connections = 12;
    client.dependencies = {"server"};
    client.proc = "RUNNING";

    server.name = "server";
    server.type = "service";
    server.state = NodeState::ONLINE;
    server.health = 92;
    server.latencyMs = 41;
    server.baseLatencyMs = 41;
    server.load = 63;
    server.memoryMb = 2048;
    server.memoryUsedMb = 640;
    server.connections = 182;
    server.dependencies = {"database", "cache"};
    server.proc = "RUNNING";

    database.name = "database";
    database.type = "database";
    database.state = NodeState::ONLINE;
    database.health = 98;
    database.latencyMs = 8;
    database.baseLatencyMs = 8;
    database.load = 25;
    database.memoryMb = 4096;
    database.memoryUsedMb = 1200;
    database.connections = 64;
    database.proc = "RUNNING";

    cache.name = "cache";
    cache.type = "cache";
    cache.state = NodeState::ONLINE;
    cache.health = 99;
    cache.latencyMs = 4;
    cache.baseLatencyMs = 4;
    cache.load = 15;
    cache.memoryMb = 1024;
    cache.memoryUsedMb = 210;
    cache.connections = 96;
    cache.proc = "RUNNING";

    initNodeHardware(client);
    initNodeHardware(server);
    initNodeHardware(database);
    initNodeHardware(cache);

    nodes_[client.name] = client;
    nodes_[server.name] = server;
    nodes_[database.name] = database;
    nodes_[cache.name] = cache;
    stage(2); // nodes (2: deterministic defaults + simulated hardware init)

    net_.ensureLink("client", "server", 12);
    net_.ensureLink("server", "database", 6);
    net_.ensureLink("server", "cache", 3);
    stage(3); // network topology

    // Default simulated processes (see process.hpp; no OS interaction).
    std::vector<ProcNote> notes;
    std::string emperr;
    pm_.spawn("srv-listener", "service", "server", 0, notes, emperr);
    pm_.spawn("srv-worker", "worker", "server", 0, notes, emperr);
    pm_.spawn("cli-shell", "shell", "client", 0, notes, emperr);
    pm_.spawn("db-engine", "database", "database", 0, notes, emperr);
    pm_.spawn("cache-daemon", "daemon", "cache", 0, notes, emperr);
    stage(4); // tasks/processes/threads (main thread each)

    // OVERKNRL virtual filesystem: standard tree + per-node service configs.
    // Pure simulation state; the host filesystem is never touched.
    vfs_.seedDefaults(nodeNames());
    stage(5); // rootfs seed

    events_.emit(clock_.tickCount(), "WORLD_INIT", "engine", "system", 0,
                 "sandbox initialised: CLIENT -> SERVER -> DATABASE, SERVER -> CACHE");
    stage(6); // event/causality state

    if (!validateAll(&err)) return false;
    stage(7); // invariants hold
    return true;
}

bool System::hasNode(const std::string& name) const { return nodes_.count(name) > 0; }

Node& System::get(const std::string& name) {
    auto it = nodes_.find(name);
    if (it == nodes_.end()) throw std::runtime_error("unknown node: " + name);
    return it->second;
}

const Node& System::get(const std::string& name) const {
    auto it = nodes_.find(name);
    if (it == nodes_.end()) throw std::runtime_error("unknown node: " + name);
    return it->second;
}

std::vector<std::string> System::nodeNames() const {
    std::vector<std::string> out;
    for (const auto& [k, v] : nodes_) out.push_back(k);
    return out;
}

uint64_t System::emit(uint64_t causeId, const std::string& type, const std::string& source,
                      const std::string& target, const std::string& message,
                      std::map<std::string, std::string> metadata, const std::string& severity) {
    // Return the id by value: the EventLog vector may reallocate on later
    // emits, so callers must never hold the returned reference.
    Event& e = events_.emit(clock_.tickCount(), type, source, target, causeId, message,
                            std::move(metadata), severity);
    return e.id;
}

// ---- determinism / invariants ----

void System::setSeed(uint64_t s) {
    seed_ = s;
    rng_ = s;
    emit(0, "SEED", "user", "system", "seed set to " + std::to_string(s));
}

void System::setAutoFaults(bool on) {
    autoFaults_ = on;
    emit(0, "AUTOFAULTS", "user", "system",
         std::string("autonomous failures ") + (on ? "enabled" : "disabled"));
}

OpResult System::setAutoRate(int rate) {
    if (rate < 0 || rate > 100)
        return OpResult::failure("rate must be within [0,100], got '" + std::to_string(rate) +
                                 "'");
    autoRate_ = rate;
    uint64_t id = emit(0, "AUTOFAULTS", "user", "system",
                       "autonomous failure rate set to " + std::to_string(rate) + "%");
    return OpResult::success(id, "autonomous failure rate: " + std::to_string(rate) + "%");
}

uint64_t System::nextRand() {
    uint64_t z = (rng_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

uint64_t System::hash64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

bool System::validateAll(std::string* err) const {
    for (const auto& [name, n] : nodes_) {
        if (!checkInvariants(n, err)) return false;
    }
    for (const auto& l : net_.links()) {
        if (l.a == l.b) {
            if (err) *err = "self-link not allowed: " + l.a + " <-> " + l.b;
            return false;
        }
        if (!hasNode(l.a) || !hasNode(l.b)) {
            if (err) *err = "dangling link: " + l.a + " <-> " + l.b;
            return false;
        }
        if (l.latencyMs < 0 || l.lossPct < 0.0 || l.lossPct > 100.0 || l.loadPct < 0.0 ||
            l.loadPct > 100.0) {
            if (err) *err = "link out of range: " + l.a + " <-> " + l.b;
            return false;
        }
    }
    if (!pm_.validateAll(err)) return false;
    if (!svc_.validateAll(err)) return false;
    if (!hw_.validateAll(err)) return false;
    if (!vfs_.validate(err)) return false;
    if (cwd_.empty() || cwd_[0] != '/' || !vfs_.isDir(cwd_)) {
        if (err) *err = "cwd is not a virtual directory: " + cwd_;
        return false;
    }
    return true;
}

std::string System::worldDigest() const {
    std::ostringstream o;
    o << "seed=" << seed_ << ";rng=" << rng_ << ";auto=" << (autoFaults_ ? 1 : 0)
      << ";rate=" << autoRate_ << ";tick=" << clock_.tickCount()
      << ";paused=" << (clock_.paused() ? 1 : 0) << ";nextpid=" << pm_.nextPid() << ";cwd=" << cwd_
      << ";";
    hw_.appendDigest(o);
    vfs_.appendDigest(o);
    for (const auto& [name, n] : nodes_) { // std::map: sorted order
        o << "node(" << n.name << "," << n.type << "," << toString(n.state) << "," << n.health
          << "," << n.latencyMs << "," << n.baseLatencyMs << "," << n.load << "," << n.memoryMb
          << "," << n.memoryUsedMb << "," << n.connections << "," << n.proc << ",deps[";
        for (size_t i = 0; i < n.dependencies.size(); ++i) {
            if (i) o << ",";
            o << n.dependencies[i];
        }
        o << "],meta[";
        for (const auto& [k, v] : n.metadata) o << k << "=" << v << ";";
        o << "],beliefs[";
        for (const auto& [t, props] : n.beliefs)
            for (const auto& [k, v] : props) o << t << "." << k << "=" << v << ";";
        o << "],thermal(" << n.tempC << "," << n.ambientC << "," << n.thermalLimitC << ","
          << n.criticalC << "," << (n.throttled ? 1 : 0) << ")";
        o << ",clock(" << n.freqMHz << "," << n.baseFreqMHz << "," << n.maxFreqMHz << ","
          << toString(n.clockState) << ")";
        o << ",mem2(" << (n.memCorrupt ? 1 : 0) << "," << n.leakMbPerTick << "," << n.swapMb
          << "," << n.swapUsedMb << "," << n.oomKills << ")";
        o << ",stor(" << n.storageMb << "," << n.storageUsedMb << "," << n.ioLoad << ")";
        o << ",fs[";
        for (const auto& [m, s] : n.filesystems) o << m << "=" << toString(s) << ";";
        o << "],kern(" << toString(n.kernel) << "," << n.instability << ")";
        o << ",mods[";
        for (const auto& [m, s] : n.modules) o << m << "=" << s << ";";
        o << "],bins[";
        for (const auto& b : n.binaries) o << b << ";";
        o << "],priv(" << toString(n.priv) << "),cfg(" << (n.configCorrupt ? 1 : 0) << ")";
        o << ",faults[";
        for (const auto& [fname, f] : n.faults)
            o << fname << "=" << toString(f.kind) << ":" << toString(f.severity) << ";";
        o << "],maxconn(" << n.maxConnections << ")";
        o << "]);";
    }
    auto links = net_.links();
    std::sort(links.begin(), links.end(), [](const Link& a, const Link& b) {
        if (a.a != b.a) return a.a < b.a;
        return a.b < b.b;
    });
    for (const auto& l : links) {
        o << "link(" << l.a << "," << l.b << "," << l.latencyMs << "," << std::fixed
          << std::setprecision(6) << l.lossPct << "," << l.loadPct << ","
          << (l.up ? "up" : "down") << ");";
    }
    o.unsetf(std::ios::floatfield);
    for (const auto& [pid, p] : pm_.all()) {
        o << "proc(" << pid << "," << p.name << "," << p.type << "," << toString(p.state) << ","
          << p.cpu << "," << p.memMb << "," << p.priority << "," << p.parentPid << "," << p.host
          << "," << p.runtime << ",thr[";
        for (const auto& t : p.threads)
            o << t.tid << ":" << t.name << ":" << toString(t.state) << ":" << t.cpuTicks << ";";
        size_t mbBytes = 0;
        for (const auto& m : p.mailbox) mbBytes += m.size();
        o << "],mb[" << p.mailbox.size() << ":" << mbBytes << "]);";
    }
    svc_.appendDigest(o);
    return hex16(fnv1a(o.str()));
}

std::string System::digest() const {
    std::ostringstream o;
    o << worldDigest() << ";events=" << events_.size() << ";";
    for (const auto& e : events_.all()) {
        o << "ev(" << e.id << "," << e.tick << "," << e.type << "," << e.source << "," << e.target
          << "," << e.causeId << "," << e.severity << "," << e.message << ");";
    }
    return hex16(fnv1a(o.str()));
}

// ---- centralized transition ----

uint64_t System::transitionTo(const std::string& name, NodeState to, uint64_t cause,
                              const std::string& reason, const std::string& source,
                              uint64_t faultEvent) {
    Node& n = get(name);
    if (n.state == to) return 0; // no-op: prevents duplicate transition events
    NodeState from = n.state;
    n.state = to;
    applyStateFixups(n);
    std::string err;
    if (!checkInvariants(n, &err)) {
        // Engine bug if this ever fires: fixups must yield a valid node.
        // Surfaced loudly; never silently repaired.
        emit(cause, "ENGINE_INVARIANT", "engine", name,
             "invariant violated after transition: " + err);
        return 0;
    }
    std::string msg = name + ": " + toString(from) + " -> " + toString(to);
    if (!reason.empty()) msg += " (" + reason + ")";
    std::map<std::string, std::string> meta{
        {"from", toString(from)}, {"to", toString(to)}, {"reason", reason}};
    if (faultEvent != 0 && events_.find(faultEvent) != nullptr)
        meta["fault-event"] = std::to_string(faultEvent);
    return emit(cause, upper(name) + "_" + toString(to), source, name, msg, std::move(meta));
}

uint64_t System::faultEventForReason(const std::string& name,
                                     const std::string& reason) const {
    // Vocabulary produced by derivedState() for fault-driven outcomes.
    static const char* prefixes[] = {"critical fault: ", "degraded by fault: ", "fault warning: "};
    if (!hasNode(name)) return 0;
    const Node& n = get(name);
    for (const char* p : prefixes) {
        std::string pre(p);
        if (reason.rfind(pre, 0) == 0) {
            auto it = n.faults.find(reason.substr(pre.size()));
            if (it != n.faults.end()) return it->second.eventId;
            return 0;
        }
    }
    return 0;
}

NodeState System::derivedState(const std::string& name, std::string& reason) const {
    const Node& n = get(name);
    bool depFailed = false, depDegraded = false;
    std::string worst;
    std::string worstReason;
    for (const auto& d : n.dependencies) {
        if (!hasNode(d)) {
            depFailed = true;
            worst = d;
            worstReason = "dependency " + d + " is MISSING";
            continue;
        }
        NodeState ds = get(d).state;
        if (ds == NodeState::FAILED || ds == NodeState::CORRUPTED) {
            depFailed = true;
            worst = d;
            worstReason = "dependency " + d + " is " + toString(ds);
            continue;
        }
        if (ds == NodeState::DEGRADED || ds == NodeState::WARNING ||
            ds == NodeState::CRITICAL || ds == NodeState::RESTARTING || ds == NodeState::PAUSED ||
            ds == NodeState::UNKNOWN) {
            if (!depDegraded) {
                depDegraded = true;
                worst = d;
                worstReason = "dependency " + d + " is " + toString(ds);
            }
        }
        const Link* l = net_.find(name, d);
        if (l && !l->up) {
            depFailed = true;
            worst = d;
            worstReason = "link to dependency " + d + " is DOWN";
        }
    }
    // Rank-max escalation: every applicable rule votes a level, the worst
    // wins. ONLINE < WARNING < DEGRADED < CRITICAL. Fault severity maps:
    // WARNING -> WARNING, DEGRADED -> DEGRADED, CRITICAL+ -> CRITICAL.
    int level = 0; // 0 online
    std::string why;
    if (depFailed) {
        level = 2;
        why = worstReason.empty() ? "dependency failure" : worstReason;
    } else if (depDegraded) {
        level = 2;
        why = worstReason;
    }
    if (n.health < 40 || n.latencyMs > 500 || n.load > 92) {
        if (level < 2) {
            level = 2;
            why = "resource stress";
        }
    }
    Severity fs = worstFaultSeverity(n);
    if (severityRank(fs) >= severityRank(Severity::CRITICAL)) {
        level = 3;
        why = "critical fault: " + worstFaultName(n);
    } else if (fs == Severity::DEGRADED) {
        if (level < 2) {
            level = 2;
            why = "degraded by fault: " + worstFaultName(n);
        }
    } else if (fs == Severity::WARNING) {
        if (level < 1) {
            level = 1;
            why = "fault warning: " + worstFaultName(n);
        }
    }
    if (level == 0) {
        // Back-pressure: a healthy provider of a FAILED node reports WARNING
        // (retry storm / idle pressure). Only applies when own deps are healthy.
        for (const auto& [uname, u] : nodes_) {
            if (u.state == NodeState::FAILED &&
                std::find(u.dependencies.begin(), u.dependencies.end(), name) !=
                    u.dependencies.end()) {
                reason = uname + " FAILED: upstream pressure";
                return NodeState::WARNING;
            }
        }
        reason.clear();
        return NodeState::ONLINE;
    }
    (void)worst;
    reason = why;
    if (level == 1) return NodeState::WARNING;
    if (level == 3) return NodeState::CRITICAL;
    return NodeState::DEGRADED;
}

void System::propagate(uint64_t causeId) {
    // Latency contagion: dependents inherit spikes. Bounded passes so
    // dependency cycles always terminate.
    for (int pass = 0; pass < 8; ++pass) {
        bool changed = false;
        for (auto& [name, n] : nodes_) { // sorted
            if (n.state == NodeState::FAILED || n.state == NodeState::CORRUPTED ||
                n.state == NodeState::PAUSED || n.state == NodeState::RESTARTING)
                continue;
            int depMax = 0;
            for (const auto& d : n.dependencies) {
                if (!hasNode(d)) continue;
                depMax = std::max(depMax, get(d).latencyMs);
            }
            if (depMax > 200 && n.latencyMs < depMax + 10) {
                n.latencyMs = std::min(5000, depMax + 10);
                changed = true;
            }
        }
        if (!changed) break;
    }

    // State recompute until fixpoint (bounded: cycles terminate).
    for (int pass = 0; pass < 12; ++pass) {
        bool changed = false;
        for (const auto& [name, ignored] : nodes_) {
            (void)ignored;
            NodeState cur = get(name).state;
            // Sticky manual states are never auto-overwritten here.
            if (cur == NodeState::FAILED || cur == NodeState::CORRUPTED ||
                cur == NodeState::PAUSED || cur == NodeState::RESTARTING)
                continue;
            std::string reason;
            NodeState want = derivedState(name, reason);
            if (want != cur) {
                transitionTo(name, want, causeId, reason, "engine",
                             faultEventForReason(name, reason));
                changed = true;
            }
        }
        if (!changed) break;
    }
}

// ---- world editing ----

OpResult System::addNode(const std::string& name, const std::string& type) {
    if (!isValidNodeName(name))
        return OpResult::failure(
            "invalid node name '" + name +
            "' (lowercase letter, then [a-z0-9_-], max 32 chars)");
    if (hasNode(name)) {
        uint64_t root = emit(0, "USER_CREATE", "user", name, "create " + name);
        emit(root, "CREATE_REJECTED", "engine", name, "node already exists: " + name);
        return OpResult::failure("node already exists: " + name, root);
    }
    uint64_t root = emit(0, "USER_CREATE", "user", name, "create " + name + " [" + type + "]");
    Node n;
    n.name = name;
    n.type = type.empty() ? "service" : type;
    n.state = NodeState::ONLINE;
    n.health = 100;
    n.baseLatencyMs = 10;
    n.latencyMs = 10;
    n.load = 5;
    n.memoryMb = 512;
    n.memoryUsedMb = 32;
    n.connections = 1;
    n.proc = "RUNNING";
    initNodeHardware(n);
    nodes_[name] = n;
    emit(root, upper(name) + "_SPAWNED", "engine", name, name + " spawned ONLINE");
    propagate(root);
    return OpResult::success(root, "created " + name + " [" + nodes_[name].type + "]");
}

OpResult System::removeNode(const std::string& name) {
    if (!hasNode(name)) return OpResult::failure("unknown node: " + name);
    uint64_t root = emit(0, "USER_REMOVE", "user", name, "remove " + name);
    // Explicit semantics: links touching the node are dropped, the node is
    // pruned from every dependency list, and all beliefs about it are purged,
    // so no dangling references remain. Past snapshots/events keep their
    // copies (restoring a snapshot can bring the node back).
    size_t prunedDeps = 0, droppedLinks = 0, purgedBeliefs = 0;
    for (const auto& l : net_.links()) {
        if (l.a == name || l.b == name) {
            net_.removeLink(l.a, l.b);
            ++droppedLinks;
        }
    }
    for (auto& [k, n] : nodes_) {
        if (k == name) continue;
        auto& deps = n.dependencies;
        size_t before = deps.size();
        deps.erase(std::remove(deps.begin(), deps.end(), name), deps.end());
        prunedDeps += before - deps.size();
        auto bit = n.beliefs.find(name);
        if (bit != n.beliefs.end()) {
            n.beliefs.erase(bit);
            ++purgedBeliefs;
        }
    }
    nodes_.erase(name);
    std::ostringstream msg;
    msg << name << " removed (links dropped: " << droppedLinks
        << ", dependency refs pruned: " << prunedDeps << ", beliefs purged: " << purgedBeliefs;
    { // hosted processes are terminated with the node
        std::vector<ProcNote> notes;
        int purged = pm_.purgeHost(name, notes);
        emitProcNotes(notes, root);
        msg << ", processes purged: " << purged;
    }
    { // ... and its services disappear with it
        auto gone = svc_.purgeHost(name);
        if (!gone.empty()) {
            std::string names;
            for (size_t i = 0; i < gone.size(); ++i) {
                if (i) names += ", ";
                names += gone[i];
            }
            emit(root, "SERVICE_PURGED", "engine", name, "services purged: " + names);
            msg << ", services purged: " << gone.size();
        }
    }
    msg << ")";
    emit(root, "NODE_REMOVED", "engine", name, msg.str(),
         {{"links", std::to_string(droppedLinks)},
          {"deps", std::to_string(prunedDeps)},
          {"beliefs", std::to_string(purgedBeliefs)}});
    propagate(root);
    return OpResult::success(root, msg.str());
}

OpResult System::addDependency(const std::string& node, const std::string& dep) {
    if (!hasNode(node)) return OpResult::failure("unknown node: " + node);
    if (!hasNode(dep)) return OpResult::failure("unknown dependency: " + dep);
    if (node == dep) return OpResult::failure("node cannot depend on itself");
    Node& n = get(node);
    if (std::find(n.dependencies.begin(), n.dependencies.end(), dep) != n.dependencies.end())
        return OpResult::failure(node + " already depends on " + dep);
    uint64_t root =
        emit(0, "USER_DEPEND", "user", node, node + " now depends on " + dep);
    n.dependencies.push_back(dep);
    emit(root, "DEPENDENCY_ADDED", "engine", node, node + " -> " + dep);
    propagate(root);
    return OpResult::success(root, node + " now depends on " + dep);
}

// ---- manipulation ----

OpResult System::breakNode(const std::string& target) {
    if (!hasNode(target)) return OpResult::failure("unknown node: " + target);
    uint64_t root = emit(0, "USER_BREAK", "user", target, "user broke " + target);
    Node& n = get(target);
    if (n.state == NodeState::FAILED) {
        emit(root, upper(target) + "_ALREADY_FAILED", target, target,
             target + " already FAILED");
        return OpResult::failure(target + " is already FAILED", root);
    }
    std::string from = toString(n.state);
    transitionTo(target, NodeState::FAILED, root, "caused by user.break", target);
    { // node failure crashes its simulated processes (same ledger)
        std::vector<ProcNote> notes;
        pm_.crashHost(target, notes);
        emitProcNotes(notes, root);
    }
    { // ... and its services
        auto crashed = svc_.crashHost(target, clock_.tickCount());
        for (const auto& sname : crashed)
            emit(root, "SERVICE_CRASHED", target, target,
                 "service " + sname + " CRASHED: host " + target + " down",
                 {{"service", sname}}, "CRITICAL");
        if (!crashed.empty())
            raiseFault(target, "service-down", Severity::WARNING, root,
                       "service " + crashed.front() + " down");
    }
    propagate(root);
    return OpResult::success(root, target + ": " + from + " -> FAILED");
}

OpResult System::repairNode(const std::string& target) {
    if (!hasNode(target)) return OpResult::failure("unknown node: " + target);
    uint64_t root = emit(0, "USER_REPAIR", "user", target, "user repaired " + target);
    Node& n = get(target);
    if (n.kernel == KernelState::PANICKED || n.kernel == KernelState::HALTED ||
        n.kernel == KernelState::OFF) {
        emit(root, "REPAIR_REJECTED", "engine", target,
             "kernel " + toString(n.kernel) + ": service repair cannot fix a dead/off kernel "
             "(start or reboot required)");
        return OpResult::failure("kernel " + toString(n.kernel) + ": start or reboot required, "
                                 "repair cannot fix a dead/off kernel",
                                 root);
    }
    std::string from = toString(n.state);
    transitionTo(target, NodeState::RESTARTING, root, "repair initiated", target);
    // Simulated lifecycle: restarting completes within this call.
    n.health = 90 + (int)(clock_.tickCount() % 8); // deterministic ~90-97
    n.latencyMs = n.baseLatencyMs;
    n.load = 20;
    n.connections = target == "server" ? 120 : (target == "client" ? 10 : 40);
    n.memoryUsedMb = n.memoryMb / 4;
    n.metadata.erase("corrupted");
    transitionTo(target, NodeState::ONLINE, root, "restart complete", target);
    { // recovery restarts the host's simulated processes
        std::vector<ProcNote> notes;
        pm_.restartHost(target, notes);
        emitProcNotes(notes, root);
    }
    { // ... and tries to restart ALWAYS services (crash counts preserved)
        for (const auto& [sname, s] : svc_.all()) {
            if (s.node == target && s.policy == ServicePolicy::ALWAYS &&
                s.state != ServiceState::RUNNING)
                serviceStartAs(sname, root, false);
        }
    }
    std::string info = target + ": " + from + " -> ONLINE";
    if (n.configCorrupt) {
        // Service restarts do not heal configuration: say so explicitly.
        emit(root, "REPAIR_PARTIAL", "engine", target,
             target + " restarted but configuration is still corrupted (recover config-corrupt)");
        info += " (PARTIAL: configuration still corrupted)";
    }
    propagate(root);
    return OpResult::success(root, info);
}

bool System::setProp(Node& n, const std::string& prop, const std::string& value,
                     std::string& err) {
    std::string p = prop;
    for (auto& c : p) c = (char)std::tolower((unsigned char)c);
    int iv = 0;
    if (p == "health") {
        if (!parseInt(value, iv) || iv < 0 || iv > 100) {
            err = "health must be an integer within [0,100], got '" + value + "'";
            return false;
        }
        n.health = iv;
    } else if (p == "latency" || p == "latency_ms") {
        try {
            iv = parseMs(value);
        } catch (const std::exception& e) {
            err = std::string("bad latency: ") + e.what();
            return false;
        }
        if (iv < 0) {
            err = "latency cannot be negative, got '" + value + "'";
            return false;
        }
        n.latencyMs = iv;
    } else if (p == "load") {
        if (!parseInt(value, iv) || iv < 0 || iv > 100) {
            err = "load must be an integer within [0,100], got '" + value + "'";
            return false;
        }
        n.load = iv;
    } else if (p == "connections") {
        if (!parseInt(value, iv) || iv < 0) {
            err = "connections cannot be negative, got '" + value + "'";
            return false;
        }
        n.connections = iv;
    } else if (p == "memory" || p == "memory_used" || p == "memory_used_mb") {
        if (!parseInt(value, iv) || iv < 0 || iv > n.memoryMb) {
            err = "memory usage must be within [0," + std::to_string(n.memoryMb) + "], got '" +
                  value + "'";
            return false;
        }
        n.memoryUsedMb = iv;
    } else if (p == "temp" || p == "temperature" || p == "temp_c") {
        if (!parseInt(value, iv) || iv < -40 || iv > 250) {
            err = "temperature must be within [-40,250], got '" + value + "'";
            return false;
        }
        n.tempC = iv;
    } else if (p == "freq" || p == "frequency" || p == "freq_mhz") {
        if (!parseInt(value, iv) || iv < 100 || iv > 12000) {
            err = "frequency must be within [100,12000] MHz, got '" + value + "'";
            return false;
        }
        n.freqMHz = iv;
    } else if (p == "storage" || p == "storage_used" || p == "storage_used_mb") {
        if (!parseInt(value, iv) || iv < 0 || iv > n.storageMb) {
            err = "storage usage must be within [0," + std::to_string(n.storageMb) + "], got '" +
                  value + "'";
            return false;
        }
        n.storageUsedMb = iv;
    } else if (p == "io" || p == "io_load") {
        if (!parseInt(value, iv) || iv < 0 || iv > 100) {
            err = "io load must be within [0,100], got '" + value + "'";
            return false;
        }
        n.ioLoad = iv;
    } else if (p == "state") {        try {
            n.state = stateFromString(upper(value));
        } catch (const std::exception&) {
            err = "unknown state '" + value +
                  "' (ONLINE|DEGRADED|FAILED|PAUSED|RESTARTING|CORRUPTED|UNKNOWN|WARNING|"
                  "CRITICAL)";
            return false;
        }
        applyStateFixups(n);
    } else if (p == "proc") {
        static const std::set<std::string> valid = {"SPAWNED", "RUNNING",  "BLOCKED", "CRASHED",
                                                    "RESTARTING", "TERMINATED"};
        std::string pv = upper(value);
        if (!valid.count(pv)) {
            err = "unknown proc '" + value + "' (SPAWNED|RUNNING|BLOCKED|CRASHED|RESTARTING|TERMINATED)";
            return false;
        }
        n.proc = pv;
    } else if (p.rfind("meta.", 0) == 0) {
        n.metadata[p.substr(5)] = value;
    } else {
        err = "unknown property: " + prop +
              " (try health|latency|load|connections|memory|temp|freq|storage|io|state|proc)";
        return false;
    }
    if (!checkInvariants(n, &err)) return false;
    return true;
}

std::string System::getProp(const Node& n, const std::string& prop) const {
    std::string p = prop;
    for (auto& c : p) c = (char)std::tolower((unsigned char)c);
    if (p == "health") return std::to_string(n.health);
    if (p == "latency" || p == "latency_ms") return std::to_string(n.latencyMs) + "ms";
    if (p == "load") return std::to_string(n.load) + "%";
    if (p == "connections") return std::to_string(n.connections);
    if (p == "memory" || p == "memory_used") return std::to_string(n.memoryUsedMb) + "MB";
    if (p == "temp" || p == "temperature" || p == "temp_c") return std::to_string(n.tempC) + "C";
    if (p == "freq" || p == "frequency" || p == "freq_mhz") return std::to_string(n.freqMHz) + "MHz";
    if (p == "storage" || p == "storage_used")
        return std::to_string(n.storageUsedMb) + "MB";
    if (p == "io" || p == "io_load") return std::to_string(n.ioLoad) + "%";
    if (p == "kernel") return toString(n.kernel);
    if (p == "clock") return toString(n.clockState);
    if (p == "priv") return toString(n.priv);
    if (p == "state") return toString(n.state);
    if (p == "proc") return n.proc;
    auto it = n.metadata.find(p);
    if (it != n.metadata.end()) return it->second;
    auto it2 = n.metadata.find(prop);
    if (it2 != n.metadata.end()) return it2->second;
    return "?";
}

OpResult System::overrideProp(const std::string& target, const std::string& prop,
                              const std::string& value) {
    if (!hasNode(target)) return OpResult::failure("unknown node: " + target);
    Node& n = get(target);
    std::string before = getProp(n, prop);
    uint64_t root =
        emit(0, "USER_OVERRIDE", "user", n.name, "override " + n.name + " " + prop + "=" + value);
    std::string err;
    if (!setProp(n, prop, value, err)) {
        emit(root, "OVERRIDE_REJECTED", "engine", n.name, err);
        return OpResult::failure(err, root);
    }
    std::string after = getProp(n, prop);
    emit(root, upper(n.name) + "_OVERRIDDEN", "user", n.name,
         n.name + "." + prop + ": " + before + " -> " + after,
         {{"prop", prop}, {"from", before}, {"to", after}});
    propagate(root);
    return OpResult::success(root, n.name + "." + prop + ": " + before + " -> " + after);
}

OpResult System::deceive(const std::string& observer, const std::string& target,
                         const std::string& prop, const std::string& value) {
    if (!hasNode(observer)) return OpResult::failure("unknown observer: " + observer);
    if (!hasNode(target)) return OpResult::failure("unknown target: " + target);
    if (prop.empty()) return OpResult::failure("no property given (use <target>.<prop>)");
    if (value.empty()) return OpResult::failure("no value given");
    Node& obs = get(observer);
    uint64_t root = emit(0, "USER_DECEIVE", "user", observer,
                         "deceive " + observer + " " + target + "." + prop + "=" + value);
    obs.beliefs[target][prop] = value;
    emit(root, "BELIEF_PLANTED", "engine", observer,
         observer + " now believes " + target + "." + prop + "=" + value + " (ground truth " +
             getProp(get(target), prop) + ")",
         {{"observer", observer}, {"target", target}, {"prop", prop}, {"value", value}});
    return OpResult::success(root, observer + " now believes " + target + "." + prop + "=" +
                                        value);
}

OpResult System::clearBeliefs(const std::string& observer, const std::string& target) {
    if (!hasNode(observer)) return OpResult::failure("unknown observer: " + observer);
    Node& o = get(observer);
    uint64_t root = emit(0, "USER_UNDECEIVE", "user", observer,
                         "undeceive " + observer + (target.empty() ? "" : " " + target));
    if (target.empty()) {
        size_t n = 0;
        for (const auto& [t, props] : o.beliefs) n += props.size();
        o.beliefs.clear();
        emit(root, "BELIEFS_CLEARED", "engine", observer,
             observer + " beliefs cleared (" + std::to_string(n) + " removed)");
        return OpResult::success(root, "cleared " + std::to_string(n) + " belief(s)");
    }
    size_t erased = o.beliefs.erase(target);
    emit(root, "BELIEFS_CLEARED", "engine", observer,
         observer + " beliefs about " + target + " cleared");
    return OpResult::success(
        root, erased ? ("beliefs about " + target + " cleared") : ("no beliefs about " + target));
}


} // namespace override

