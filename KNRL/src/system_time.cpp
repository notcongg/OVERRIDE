// System (tick orchestration, entropy, snapshots). Split from system.cpp; behavior unchanged.
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

int System::tick(int n, uint64_t causeId, ProgressCb progress) {
    if (n <= 0) n = 1;
    if (clock_.paused()) return 0;
    uint64_t cause = causeId;
    if (cause == 0)
        cause = emit(0, "USER_TICK", "user", "system", "tick " + std::to_string(n),
                     {{"trigger", "time"}});
    int done = 0;
    for (int i = 0; i < n; ++i) {
        clock_.tick();
        onTick(cause);
        ++done;
        if (progress) progress(done, n);
    }
    propagate(cause);
    return done;
}

int System::autoTick() {
    if (clock_.paused()) return 0;
    uint64_t cause =
        emit(0, "AUTO_TICK", "engine", "system", "simulation auto-tick", {{"trigger", "time"}});
    clock_.tick();
    onTick(cause);
    propagate(cause);
    return 1;
}

int System::jitter(const std::string& name, uint64_t tick) const {
    // Pure function of (seed, tick, name): deterministic, no stored RNG state.
    uint64_t h = seed_ ^ (tick * 0x9E3779B97F4A7C15ull) ^ (fnv1a(name) + 0x9E3779B97F4A7C15ull);
    splitmix64(h);
    return (int)(splitmix64(h) % 17) - 8; // -8..+8
}

void System::autonomousTick(uint64_t causeId) {
    // Background entropy. Everything here draws from the snapshot/restored
    // rng_ stream, so (seed, commands, ticks) fully determines the outcome.
    // Rates stay low: wear and spikes are common, crashes only strike strained
    // nodes, and stress-derived degradation can recover on its own.
    uint64_t t = clock_.tickCount();
    for (auto& [name, n] : nodes_) { // sorted: deterministic order
        if (n.state == NodeState::FAILED || n.state == NodeState::CORRUPTED ||
            n.state == NodeState::PAUSED || n.state == NodeState::RESTARTING ||
            n.state == NodeState::UNKNOWN)
            continue;
        int memPct = n.memoryUsedMb * 100 / std::max(1, n.memoryMb);
        bool strained =
            (n.load > 85 || n.latencyMs > 400 || n.health < 50 || memPct > 90);
        if ((nextRand() % 100) < (uint64_t)autoRate_) {
            uint64_t pick = nextRand() % 100;
            if (pick < 35) {
                int spike = 50 + (int)(nextRand() % 150);
                int before = n.latencyMs;
                n.latencyMs = std::min(5000, n.latencyMs + spike);
                emit(causeId, "LATENCY_SPIKE", n.name, n.name,
                     n.name + " latency " + std::to_string(before) + "ms -> " +
                         std::to_string(n.latencyMs) + "ms (cause: spontaneous_fault)",
                     {{"from", std::to_string(before)},
                      {"to", std::to_string(n.latencyMs)},
                      {"cause-detail", "spontaneous_fault"}});
            } else if (pick < 60) {
                int surge = 10 + (int)(nextRand() % 16);
                n.load = std::clamp(n.load + surge, 0, 100);
                emit(causeId, "LOAD_SURGE", n.name, n.name,
                     n.name + " load surged to " + std::to_string(n.load) +
                         "% (cause: spontaneous_fault)",
                     {{"load", std::to_string(n.load)}, {"cause-detail", "spontaneous_fault"}});
            } else if (pick < 75) {
                int creep = n.memoryMb * (5 + (int)(nextRand() % 11)) / 100;
                n.memoryUsedMb = std::min(n.memoryMb, n.memoryUsedMb + creep);
                emit(causeId, "MEMORY_PRESSURE", n.name, n.name,
                     n.name + " memory " + std::to_string(n.memoryUsedMb) + "/" +
                         std::to_string(n.memoryMb) + "MB (cause: spontaneous_fault)",
                     {{"cause-detail", "spontaneous_fault"}});
            } else if (pick < 85) {
                auto links = net_.linksFor(name);
                if (!links.empty()) {
                    std::sort(links.begin(), links.end(), [](const Link& a, const Link& b) {
                        std::string na = a.a < a.b ? a.a + a.b : a.b + a.a;
                        std::string nb = b.a < b.b ? b.a + b.b : b.b + b.a;
                        return na < nb;
                    });
                    const Link& pick2 = links[nextRand() % links.size()];
                    double add = 10.0 + (double)(nextRand() % 21);
                    double cur = 0.0;
                    if (const Link* l = net_.find(pick2.a, pick2.b)) cur = l->lossPct;
                    net_.setLoss(pick2.a, pick2.b, std::min(100.0, cur + add));
                    emit(causeId, "PACKET_LOSS", n.name, n.name,
                         pick2.a + " <-> " + pick2.b + " packet loss rising (cause: "
                         "spontaneous_fault)",
                         {{"cause-detail", "spontaneous_fault"}});
                }
            } else {
                int wear = 1 + (int)(nextRand() % 3);
                n.health = std::max(0, n.health - wear);
                emit(causeId, "HEALTH_DECAY", n.name, n.name,
                     n.name + " health decayed to " + std::to_string(n.health) +
                         "% (cause: spontaneous_fault)",
                     {{"health", std::to_string(n.health)},
                      {"cause-detail", "spontaneous_fault"}});
            }
        }
        // Strained nodes may crash outright (spontaneous fault with a cause).
        if (strained && (nextRand() % 100) < (uint64_t)autoRate_) {
            if ((nextRand() % 100) < 30) {
                transitionTo(name, NodeState::FAILED, causeId, "spontaneous fault (strain)",
                             name);
            } else {
                n.latencyMs = std::min(5000, n.latencyMs + 100);
                emit(causeId, "LATENCY_SPIKE", n.name, n.name,
                     n.name + " latency rising under strain: " + std::to_string(n.latencyMs) +
                         "ms (cause: database instability)",
                     {{"cause-detail", "instability"}});
            }
        }
        // Recovery: healthy-dependency nodes shed stress on their own.
        bool depsHealthy = true;
        for (const auto& d : n.dependencies) {
            if (!hasNode(d) || get(d).state != NodeState::ONLINE) {
                depsHealthy = false;
                break;
            }
        }
        if (depsHealthy && (n.state == NodeState::DEGRADED || n.state == NodeState::WARNING ||
                             n.state == NodeState::CRITICAL)) {
            if (n.health < 100) n.health = std::min(100, n.health + 1);
            if (n.load > 35) n.load -= 2;
            // propagate() below turns sustained recovery into *_ONLINE events.
        }
        (void)t;
        // Rare spontaneous physics faults (seeded): thermal warnings, leaks,
        // congestion, clock jitter. Same stream as everything else.
        // applyFaultEffect sets real fields, not just a record.
        if ((nextRand() % 100) < (uint64_t)(autoRate_ / 4)) {
            static const char* spon[] = {"overheat", "mem-leak", "net-congest", "clock-unstable"};
            std::string sf = spon[nextRand() % 4];
            const FaultDef* def = faultDef(sf);
            if (def && get(name).faults.count(sf) == 0) {
                emit(causeId, "RANDOM_FAULT", "engine", name,
                     "spontaneous " + sf + " on " + name + " (seed " + std::to_string(seed_) + ")",
                     {{"fault", sf}}, "WARNING");
                applyFaultEffect(name, *def, "", causeId);
            }
        }
    }
    // Link recovery: packet loss and congestion decay back toward calm.
    for (auto& l : net_.links()) {
        Link* m = net_.find(l.a, l.b);
        if (!m) continue;
        if (m->lossPct > 0.0) m->lossPct = std::max(0.0, m->lossPct - 5.0);
        if (m->loadPct > 30.0) m->loadPct = std::max(30.0, m->loadPct - 4.0);
        else if (m->loadPct < 30.0) m->loadPct = std::min(30.0, m->loadPct + 2.0);
    }
}

void System::onTick(uint64_t causeId) {
    uint64_t t = clock_.tickCount();
    for (auto& [name, n] : nodes_) { // sorted: deterministic order
        if (n.state == NodeState::FAILED || n.state == NodeState::CORRUPTED ||
            n.state == NodeState::PAUSED) {
            n.connections = 0;
            n.load = 0;
            continue;
        }
        if (n.state == NodeState::ONLINE || n.state == NodeState::DEGRADED ||
            n.state == NodeState::WARNING || n.state == NodeState::CRITICAL) {
            // Resource physics first: may degrade or fail the node outright.
            stepPhysics(name, causeId);
            Node& after = get(name);
            if (after.state == NodeState::FAILED || after.state == NodeState::CORRUPTED ||
                after.state == NodeState::PAUSED) {
                after.connections = 0;
                after.load = 0;
                continue;
            }
            int wobble = jitter(name, t);
            int base = 35;
            if (n.state == NodeState::DEGRADED) base = 70;
            else if (n.state == NodeState::WARNING) base = 55;
            else if (n.state == NodeState::CRITICAL) base = 85;
            n.load = std::clamp(base + wobble, 5, 99);
            if (n.latencyMs > n.baseLatencyMs) {
                n.latencyMs = std::max(n.baseLatencyMs, n.latencyMs - 25);
            } else {
                n.latencyMs = std::max(1, n.baseLatencyMs + wobble / 2);
            }
            // Resource coupling: pressure feeds back into behavior.
            int memPct = n.memoryUsedMb * 100 / std::max(1, n.memoryMb);
            bool strainedRes = (n.load > 85 || memPct > 90);
            if (n.load > 85) n.latencyMs = std::min(5000, n.latencyMs + 10); // CPU -> latency
            if (memPct > 90 && n.health > 0) n.health -= 1; // memory strain -> decay
            if (n.state == NodeState::ONLINE && n.health < 100 && !strainedRes)
                n.health = std::min(100, n.health + 1); // no self-heal under strain
            // Process starvation (always on): a serving node whose processes
            // are all dead decays until the stress rule degrades it.
            if ((n.state == NodeState::ONLINE || n.state == NodeState::WARNING) &&
                pm_.totalOn(name) > 0 && pm_.runningOn(name) == 0 && n.health > 25) {
                n.health -= 5;
                emit(causeId, "HEALTH_DECAY", n.name, n.name,
                     n.name + " health " + std::to_string(n.health) +
                         "%: no running processes (cause: process starvation)",
                     {{"health", std::to_string(n.health)},
                      {"cause-detail", "starvation"}});
            }
        }
    }
    // Link congestion drifts with the seeded stream.
    for (auto& l : net_.links()) {
        Link* m = net_.find(l.a, l.b);
        if (!m || !m->up) continue;
        int wob = (int)(nextRand() % 21) - 10; // -10..+10
        m->loadPct = std::clamp(m->loadPct + (double)wob, 0.0, 100.0);
    }
    // Simulated processes evolve (drift + watchdog restarts).
    {
        std::vector<ProcNote> notes;
        auto serving = [this](const std::string& host) {
            return hasNode(host) && routable(get(host));
        };
        pm_.onTick(t, rng_, autoFaults_, autoRate_, serving, notes);
        emitProcNotes(notes, causeId);
    }
    if (autoFaults_) autonomousTick(causeId);
    // Services reconcile (supervisor restarts, crash counting, node coupling).
    serviceTick(causeId);
    // Client experiences timeouts while server is down (causal traffic events).
    if (hasNode("client") && hasNode("server")) {
        const Node& srv = get("server");
        if (srv.state == NodeState::FAILED || srv.state == NodeState::CORRUPTED) {
            emit(causeId, "REQUEST_TIMEOUT", "client", "client",
                 "client request failed: server " + toString(srv.state) + " (t=" +
                     std::to_string(t) + ")");
        } else if (srv.latencyMs > 500) {
            emit(causeId, "REQUEST_SLOW", "client", "client",
                 "client request slow: server latency " + std::to_string(srv.latencyMs) + "ms");
        }
    }
}

// ---- snapshots ----

uint64_t System::checkpoint(const std::string& label) {
    uint64_t id = history_
                      .take(clock_.tickCount(), clock_.paused(), seed_, rng_, autoFaults_,
                            autoRate_, pm_.nextPid(),
                            label.empty() ? ("snap@" + std::to_string(clock_.tickCount())) : label,
                            nodes_, net_.links(), pm_.all(), svc_.all(), hw_, vfs_, cwd_,
                            events_.size())
                      .id;
    emit(0, "CHECKPOINT", "user", "system",
         "checkpoint #" + std::to_string(id) + " at t=" + std::to_string(clock_.tickCount()));
    return id;
}

bool System::restore(uint64_t snapshotId, uint64_t* causeOut, ProgressCb progress) {
    const Snapshot* s = history_.find(snapshotId);
    if (!s) return false;
    auto stage = [&](int done, int total) {
        if (progress) progress(done, total);
    };
    nodes_ = s->nodes;
    stage(1, 7);
    net_.clear();
    for (const auto& l : s->links) net_.ensureLink(l.a, l.b, l.latencyMs);
    for (const auto& l : s->links) {
        net_.setUp(l.a, l.b, l.up);
        net_.setLoss(l.a, l.b, l.lossPct);
        net_.setLatency(l.a, l.b, l.latencyMs);
        if (Link* m = net_.find(l.a, l.b)) m->loadPct = l.loadPct;
    }
    stage(2, 7);
    pm_.restoreSnapshot(s->procs, s->nextPid);
    stage(3, 7);
    svc_.restoreSnapshot(s->services);
    stage(4, 7);
    hw_ = s->hw;
    stage(5, 7);
    vfs_ = s->vfs;
    cwd_ = s->cwd;
    stage(6, 7);
    clock_.setTick(s->tick);
    if (s->clockPaused) clock_.pause();
    else clock_.resume();
    seed_ = s->seed;
    rng_ = s->rng;
    autoFaults_ = s->autoFaults;
    autoRate_ = s->autoRate;
    stage(7, 7);
    uint64_t root = emit(0, "RESTORE", "user", "system",
                         "restored checkpoint #" + std::to_string(s->id) + " '" + s->label + "'");
    if (causeOut) *causeOut = root;
    return true;
}

bool System::rewindSteps(int stepsBack, uint64_t* causeOut, ProgressCb progress) {
    const auto& snaps = history_.snapshots();
    if (snaps.empty()) return false;
    int idx = (int)snaps.size() - 1 - stepsBack;
    if (idx < 0) idx = 0;
    return restore(snaps[(size_t)idx].id, causeOut, progress);
}

// ---- observation ----


} // namespace override

