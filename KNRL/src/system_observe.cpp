// System (read-only observation and reporting). Split from system.cpp; behavior unchanged.
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

namespace {
// A time trigger only advances the simulation clock; it never explains *why*
// a state holds. Triggers are recognised generically via event metadata, never
// by command name, so future clock sources stay classified automatically.
bool isTimeTrigger(const Event& e) {
    auto it = e.metadata.find("trigger");
    return it != e.metadata.end() && it->second == "time";
}

uint64_t refOf(const std::map<std::string, std::string>& meta, const std::string& key) {
    auto it = meta.find(key);
    if (it == meta.end() || it->second.empty()) return 0;
    try {
        size_t pos = 0;
        uint64_t v = std::stoull(it->second, &pos);
        if (pos != it->second.size()) return 0;
        return v;
    } catch (...) {
        return 0;
    }
}

// Id of the latest RESTORE event (0 when the world was never rewound).
// Events at or below it belong to a discarded incarnation: the append-only
// ledger keeps them, but they no longer cause the current state.
uint64_t lastRestoreId(const EventLog& log) {
    uint64_t id = 0;
    for (const auto& e : log.all())
        if (e.type == "RESTORE") id = e.id;
    return id;
}

// Causal closure of leafId: cause links, fault-lineage edges ("fault-event"
// on transitions, "prior" on escalations), plus the sibling cone (other
// events the same target experienced under the same cause). Bounded below by
// minId (exclusive) so readers never cross a restore boundary, and above by
// the leaf's tick window so later ticks stay out. Visited set guarantees
// termination. Returned in chronological order.
std::vector<const Event*> causalClosure(const EventLog& log, uint64_t leafId,
                                        const std::string& target, uint64_t minId) {
    const Event* leaf = log.find(leafId);
    if (!leaf) return {};
    std::set<uint64_t> seen;
    std::vector<const Event*> chain;
    std::vector<uint64_t> stack{leafId};
    const uint64_t leafTick = leaf->tick;
    while (!stack.empty()) {
        uint64_t id = stack.back();
        stack.pop_back();
        if (id == 0 || id <= minId || !seen.insert(id).second) continue;
        const Event* e = log.find(id);
        if (!e) continue;
        if (e->tick > leafTick) continue;
        chain.push_back(e);
        if (e->causeId != 0) stack.push_back(e->causeId);
        uint64_t fe = refOf(e->metadata, "fault-event");
        if (fe != 0) stack.push_back(fe);
        uint64_t prior = refOf(e->metadata, "prior");
        if (prior != 0) stack.push_back(prior);
        // Sibling cone: other events this target experienced under the same
        // cause (e.g. WARNING alongside FAULT_RAISED under one inject, or
        // escalations under one tick) belong to the same story.
        for (const auto& c : log.all()) {
            if (c.causeId == id && c.target == target && c.tick <= leafTick)
                stack.push_back(c.id);
        }
    }
    std::sort(chain.begin(), chain.end(), [](const Event* a, const Event* b) {
        if (a->tick != b->tick) return a->tick < b->tick;
        return a->id < b->id;
    });
    return chain;
}

// Effective root of a chronological chain: the earliest event that is not a
// pure time trigger. A bare cause walk stops at whatever tick happened to
// process an escalation; skipping triggers generically surfaces the
// underlying intervention or fault instead. Pure-trigger chains fall back to
// their earliest event.
const Event* effectiveRoot(const std::vector<const Event*>& chain) {
    for (const Event* e : chain)
        if (!isTimeTrigger(*e)) return e;
    return chain.empty() ? nullptr : chain.front();
}

// First FAULT_* event in a chronological chain, if any: the driving fault
// behind an intervention root (e.g. FAULT_RAISED behind USER_INJECT).
const Event* drivingFault(const std::vector<const Event*>& chain) {
    for (const Event* e : chain)
        if (e->type.rfind("FAULT_", 0) == 0) return e;
    return nullptr;
}

void printRootFooter(std::ostringstream& o, const std::vector<const Event*>& chain) {
    const Event* root = effectiveRoot(chain);
    if (root) {
        o << "  effective root cause: #" << root->id << " " << root->type;
        if (!root->message.empty()) o << " (" << root->message << ")";
        o << "\n";
    }
    const Event* fault = drivingFault(chain);
    if (fault && fault != root) {
        o << "  driving fault: #" << fault->id << " " << fault->type;
        if (!fault->message.empty()) o << " (" << fault->message << ")";
        o << "\n";
    }
}
} // namespace

std::string System::psList() const {
    std::ostringstream o;
    o << "PID   NAME            TYPE      STATE       CPU  MEM     HOST      RUNTIME\n";
    for (const auto& [pid, p] : pm_.all()) {
        o << pid << std::string(pid < 100 ? "   " : pid < 1000 ? "  " : " ") << p.name
          << std::string(p.name.size() < 16 ? 16 - p.name.size() : 1, ' ') << p.type
          << std::string(p.type.size() < 10 ? 10 - p.type.size() : 1, ' ') << toString(p.state)
          << std::string(12 - toString(p.state).size(), ' ') << p.cpu << "%  " << p.memMb
          << "MB   " << p.host << std::string(p.host.size() < 10 ? 10 - p.host.size() : 1, ' ')
          << "t+" << p.runtime << "\n";
    }
    return o.str();
}

std::string System::procInspect(const std::string& pidOrName) const {
    const SimProcess* p = pm_.find(pidOrName);
    if (!p) return "unknown process: " + pidOrName;
    std::ostringstream o;
    o << "proc " << p->name << " (pid " << p->pid << ")\n";
    o << "  type:     " << p->type << "\n";
    o << "  state:    " << toString(p->state) << "\n";
    o << "  cpu:      " << p->cpu << "%\n";
    o << "  memory:   " << p->memMb << "MB\n";
    o << "  priority: " << p->priority << "\n";
    o << "  parent:   " << (p->parentPid == 0 ? "(none)" : std::to_string(p->parentPid)) << "\n";
    o << "  children: ";
    bool first = true;
    for (const auto& [q, c] : pm_.all()) {
        if (c.parentPid == p->pid) {
            if (!first) o << ", ";
            o << c.name << "(" << q << ")";
            first = false;
        }
    }
    if (first) o << "(none)";
    o << "\n  host:     " << p->host;
    if (hasNode(p->host)) o << " [" << toString(get(p->host).state) << "]";
    o << "\n  runtime:  t+" << p->runtime << "\n";
    // Task -> process -> thread(s): real stored threads, not a label.
    o << "  threads (" << p->threads.size() << "):";
    if (p->threads.empty()) {
        o << " (none)\n";
    } else {
        o << "\n";
        for (const auto& t : p->threads)
            o << "    tid " << t.tid << " " << t.name << " [" << toString(t.state)
              << "] cpu=" << t.cpuTicks << " ticks\n";
    }
    o << "  mailbox (" << p->mailbox.size() << " queued)\n";
    // own slice of the unified ledger
    o << "  events:\n";
    int shown = 0;
    for (const auto& e : events_.all()) {
        if (e.source == p->name && e.target == p->host) {
            o << "    [t=" << e.tick << " #" << e.id << "] " << e.type << ": " << e.message << "\n";
            if (++shown >= 6) break;
        }
    }
    if (shown == 0) o << "    (none)\n";
    return o.str();
}

std::string System::status() const {
    std::ostringstream o;
    std::vector<std::string> names = nodeNames();
    std::sort(names.begin(), names.end());
    // canonical order client, server, database, cache first, then rest alphabetical
    std::vector<std::string> canon = {"client", "server", "database", "cache"};
    std::vector<std::string> ordered;
    for (auto& c : canon)
        if (hasNode(c)) ordered.push_back(c);
    for (auto& n : names)
        if (std::find(ordered.begin(), ordered.end(), n) == ordered.end()) ordered.push_back(n);
    for (auto& n : ordered) {
        const Node& node = get(n);
        o << node.name << "  " << toString(node.state) << "\n";
    }
    return o.str();
}

std::string System::inspect(const std::string& target, const std::string& observer) const {
    if (!hasNode(target)) return "unknown node: " + target;
    const Node& n = get(target);
    std::ostringstream o;
    o << n.name << " [" << n.type << "]\n";
    o << "  state:       " << toString(n.state) << "\n";
    o << "  health:      " << n.health << "%\n";
    o << "  latency:     " << n.latencyMs << "ms (base " << n.baseLatencyMs << "ms)\n";
    o << "  load:        " << n.load << "%\n";
    o << "  memory:      " << n.memoryUsedMb << "/" << n.memoryMb << "MB";
    if (n.swapUsedMb > 0) o << " (swap " << n.swapUsedMb << "/" << n.swapMb << "MB)";
    if (n.memCorrupt) o << " [CORRUPTED]";
    if (n.leakMbPerTick > 0) o << " [leaking " << n.leakMbPerTick << "MB/tick]";
    if (n.oomKills > 0) o << " [oom kills: " << n.oomKills << "]";
    o << "\n";
    o << "  thermal:     " << n.tempC << "C (ambient " << n.ambientC << "C, limit "
      << n.thermalLimitC << "C, critical " << n.criticalC << "C)";
    if (n.throttled) o << " [THROTTLED]";
    o << "\n";
    o << "  clock:       " << n.freqMHz << "MHz (base " << n.baseFreqMHz << "MHz, max "
      << n.maxFreqMHz << "MHz) [" << toString(n.clockState) << "]\n";
    o << "  storage:     " << n.storageUsedMb << "/" << n.storageMb << "MB io=" << n.ioLoad
      << "%\n";
    o << "  filesystems: ";
    if (n.filesystems.empty()) {
        o << "(none)\n";
    } else {
        bool first = true;
        for (const auto& [m, st] : n.filesystems) {
            if (!first) o << ", ";
            o << m << "=" << toString(st);
            first = false;
        }
        o << "\n";
    }
    o << "  kernel:      " << toString(n.kernel);
    if (n.instability > 0) o << " (instability " << n.instability << ")";
    o << "\n";
    o << "  modules:     ";
    if (n.modules.empty()) {
        o << "(none)\n";
    } else {
        bool first = true;
        for (const auto& [m, st] : n.modules) {
            if (!first) o << ", ";
            o << m << "=" << st;
            first = false;
        }
        o << "\n";
    }
    o << "  priv:        " << toString(n.priv);
    if (n.configCorrupt) o << "  [config CORRUPTED]";
    o << "\n";
    o << "  faults:      ";
    if (n.faults.empty()) {
        o << "(none active)\n";
    } else {
        o << "\n";
        for (const auto& [fname, f] : n.faults) {
            o << "    " << fname << " [" << toString(f.kind) << "/" << toString(f.severity)
              << "] since t=" << f.sinceTick;
            if (!f.detail.empty()) o << " (" << f.detail << ")";
            o << "\n";
        }
    }
    o << "  processes:   ";
    {
        bool first = true;
        for (const auto& [pid, p] : pm_.all()) {
            if (p.host != target) continue;
            if (!first) o << ", ";
            o << p.name << "(" << pid << ":" << toString(p.state) << ")";
            first = false;
        }
        if (first) o << "(none)";
        o << "\n";
    }
    o << "  services:    ";
    {
        bool first = true;
        for (const auto& [sname, s] : svc_.all()) {
            if (s.node != target) continue;
            if (!first) o << ", ";
            o << sname << "(" << toString(s.state) << "/" << toString(s.policy) << ")";
            first = false;
        }
        if (first) o << "(none)";
        o << "\n";
    }
    o << "  connections: " << n.connections << "/" << n.maxConnections << "\n";
    o << "  proc:        " << n.proc << "\n";
    o << "  depends:     ";
    if (n.dependencies.empty()) {
        o << "(none)\n";
    } else {
        for (size_t i = 0; i < n.dependencies.size(); ++i) {
            if (i) o << ", ";
            o << n.dependencies[i];
            if (hasNode(n.dependencies[i])) o << "(" << toString(get(n.dependencies[i]).state) << ")";
            else o << "(MISSING)";
        }
        o << "\n";
    }
    auto links = net_.linksFor(target);
    o << "  links:       ";
    if (links.empty()) {
        o << "(none)\n";
    } else {
        for (size_t i = 0; i < links.size(); ++i) {
            if (i) o << ", ";
            std::string other = links[i].a == target ? links[i].b : links[i].a;
            o << other << (links[i].up ? "" : "[DOWN]") << ":" << links[i].latencyMs << "ms";
        }
        o << "\n";
    }
    if (!n.metadata.empty()) {
        o << "  metadata:    ";
        bool first = true;
        for (const auto& [k, v] : n.metadata) {
            if (!first) o << ", ";
            o << k << "=" << v;
            first = false;
        }
        o << "\n";
    }
    if (!observer.empty() && hasNode(observer)) {
        const Node& o2 = get(observer);
        auto it = o2.beliefs.find(target);
        if (it != o2.beliefs.end() && !it->second.empty()) {
            o << "  [" << observer << " believes (may differ from truth): ";
            bool first = true;
            for (const auto& [k, v] : it->second) {
                if (!first) o << ", ";
                o << k << "=" << v;
                first = false;
            }
            o << "]\n";
        }
    }
    if (!n.beliefs.empty()) {
        o << "  beliefs held by " << n.name << ":\n";
        for (const auto& [t, props] : n.beliefs)
            for (const auto& [k, v] : props) o << "    " << t << "." << k << "=" << v << "\n";
    }
    auto evs = events_.forTarget(target, 5);
    o << "  events:      " << events_.forTarget(target).size() << " (last " << evs.size()
      << " shown)\n";
    for (const auto* e : evs)
        o << "    [t=" << e->tick << " #" << e->id << "] [" << eventScope(*e) << "] " << e->type
          << ": " << e->message << "\n";
    return o.str();
}

std::string System::trace(const std::string& target) const {
    // HOW the node reached its current state: chronological timeline of its own
    // state transitions, then dependency context. (why = single causal chain.)
    // The ledger is append-only, so transitions from discarded incarnations
    // (at/below the latest RESTORE) are shown under an explicit history header
    // and never presented as the cause of the current state.
    if (!hasNode(target)) return "unknown node: " + target;
    std::ostringstream o;
    o << "trace " << target << " (state: " << toString(get(target).state)
      << " @t=" << clock_.tickCount() << ")\n";
    std::vector<const Event*> transitions;
    for (const auto* e : events_.forTarget(target)) {
        auto it = e->metadata.find("from");
        auto jt = e->metadata.find("to");
        if (it != e->metadata.end() && jt != e->metadata.end()) transitions.push_back(e);
    }
    const uint64_t restoreId = lastRestoreId(events_);
    std::vector<const Event*> current, history;
    for (const auto* e : transitions) (e->id > restoreId ? current : history).push_back(e);
    auto printLine = [&o](const Event* e) {
        o << "    [t=" << e->tick << " #" << e->id << "] [" << eventScope(*e) << "] "
          << e->type << ": " << e->message << "\n";
    };
    if (restoreId == 0) {
        o << "  timeline (" << transitions.size() << " transition(s)):\n";
        if (transitions.empty()) {
            o << "    no state transitions recorded.\n";
        } else {
            for (const auto* e : transitions) printLine(e); // ledger order = chronological
        }
    } else {
        const Event* r = events_.find(restoreId);
        o << "  established by: RESTORE #" << restoreId;
        if (r && !r->message.empty()) o << " (" << r->message << ")";
        o << "\n";
        o << "  current incarnation (" << current.size() << " transition(s)):\n";
        if (current.empty()) {
            o << "    no transitions since restore.\n";
        } else {
            for (const auto* e : current) printLine(e);
        }
        if (!history.empty()) {
            o << "  ledger history (" << history.size()
              << " earlier transition(s), kept append-only):\n";
            for (const auto* e : history) printLine(e);
        }
    }
    if (!current.empty() || restoreId == 0) {
        const std::vector<const Event*>& scope = (restoreId == 0) ? transitions : current;
        if (!scope.empty()) printRootFooter(o, causalClosure(events_, scope.back()->id, target, restoreId));
    }
    const Node& n = get(target);
    if (!n.dependencies.empty()) {
        o << "  dependencies:\n";
        for (const auto& d : n.dependencies) {
            if (!hasNode(d)) {
                o << "    " << upper(d) << " [MISSING]\n";
                continue;
            }
            o << "    " << upper(d) << " [" << toString(get(d).state) << "]";
            // Incarnation-aware: only post-restore events explain the present.
            const Event* last = nullptr;
            for (const auto* e : events_.forTarget(d))
                if (e->id > restoreId) last = e;
            if (!last) {
                o << " last: (none since restore)";
            } else {
                o << " last: " << last->type << " #" << last->id;
                auto chain = causalClosure(events_, last->id, d, restoreId);
                if (const Event* root = effectiveRoot(chain))
                    o << " (effective root: " << root->type << " #" << root->id << ")";
            }
            o << "\n";
        }
    }
    return o.str();
}

std::string System::why(const std::string& target) const {
    // WHY the current state exists: the causal closure of the latest
    // state-affecting event in the current incarnation, printed root-first
    // (chronological).
    //
    // A bare causeId walk stops at the tick that happened to process an
    // escalation, hiding the underlying fault. So besides cause links we
    // follow fault-lineage edges ("fault-event" on transitions, "prior" on
    // escalations), and we expand time triggers sideways to sibling events
    // that concern this node. Triggers stay in the printed chain with a
    // "(time trigger: ...)" annotation, but the footer reports the effective
    // root cause (earliest non-trigger event) plus the driving fault, resolved
    // generically via trigger metadata -- nothing here is specific to any
    // single command or fault category.
    //
    // The walk never crosses a RESTORE boundary: pre-restore events belong to
    // a discarded incarnation and cannot cause the current state.
    if (!hasNode(target)) return "unknown node: " + target;
    const uint64_t restoreId = lastRestoreId(events_);
    auto evs = events_.forTarget(target);
    const Event* leaf = nullptr;
    for (auto it = evs.rbegin(); it != evs.rend(); ++it) {
        if ((*it)->id <= restoreId) continue; // discarded incarnation
        const std::string& t = (*it)->type;
        if (t.find("FAILED") != std::string::npos || t.find("DEGRADED") != std::string::npos ||
            t.find("CRITICAL") != std::string::npos ||
            t.find("CORRUPT") != std::string::npos || t.find("RECOVER") != std::string::npos ||
            t.find("WARNING") != std::string::npos || t.find("TIMEOUT") != std::string::npos ||
            t.find("OVERRIDDEN") != std::string::npos || t.find("PAUSED") != std::string::npos ||
            t.find("ONLINE") != std::string::npos || t.find("RESTARTING") != std::string::npos) {
            leaf = *it;
            break;
        }
    }
    if (!leaf) {
        size_t older = 0;
        const Event* latest = nullptr;
        for (const auto* e : evs) {
            if (e->id <= restoreId) {
                ++older;
                continue;
            }
            latest = e;
        }
        std::ostringstream o;
        if (restoreId != 0 && (older > 0 || latest == nullptr)) {
            // Current state was established by the restore itself.
            const Event* r = events_.find(restoreId);
            o << target << " is " + toString(get(target).state) + " (established by RESTORE #"
              << restoreId;
            if (r && !r->message.empty()) o << ": " << r->message;
            o << "; no state-affecting events since restore, " << older
              << " earlier event(s) kept in ledger).";
            return o.str();
        }
        if (evs.empty()) return target + " is " + toString(get(target).state) + ": no events yet.";
        // The latest event did not affect node state (e.g. a rejected command):
        // presenting it as the cause would be misleading, so say so explicitly
        // while keeping the append-only ledger untouched.
        const Event* last = latest != nullptr ? latest : evs.back();
        o << target << " is " + toString(get(target).state) +
                 ": no state-affecting events recorded (latest entry #" << last->id << " "
          << last->type << " did not change node state).";
        return o.str();
    }
    auto chain = causalClosure(events_, leaf->id, target, restoreId);
    std::ostringstream o;
    o << target << " is " << toString(get(target).state) << " because:\n";
    for (size_t i = 0; i < chain.size(); ++i) {
        const Event* e = chain[i];
        o << "  [" << (i + 1) << "] #" << e->id << " [" << eventScope(*e) << "] " << e->type
          << " (t=" << e->tick << ") " << e->message;
        if (isTimeTrigger(*e))
            o << " (time trigger: advanced simulation; effects below follow from it)";
        o << "\n";
    }
    printRootFooter(o, chain);
    return o.str();
}

std::string System::predict(const std::string& target) const {
    if (!hasNode(target)) return "unknown node: " + target;
    const Node& n = get(target);
    std::ostringstream o;
    o << "prediction for " << target << " [" << toString(n.state) << "]:\n";
    std::vector<std::string> signals, next;
    if (n.latencyMs > 500) {
        signals.push_back("latency critical (" + std::to_string(n.latencyMs) + "ms)");
        next.push_back("REQUEST_TIMEOUT");
    } else if (n.latencyMs > 250) {
        signals.push_back("latency rising (" + std::to_string(n.latencyMs) + "ms)");
        next.push_back("LATENCY_SPIKE");
    }
    if (n.load > 90) {
        signals.push_back("load critical (" + std::to_string(n.load) + "%)");
        next.push_back("RESOURCE_EXHAUSTION");
    } else if (n.load > 70) {
        signals.push_back("load rising (" + std::to_string(n.load) + "%)");
    }
    if (n.health < 40) {
        signals.push_back("health low (" + std::to_string(n.health) + "%)");
        next.push_back("NODE_FAILURE");
    }
    for (const auto& d : n.dependencies) {
        if (!hasNode(d)) {
            signals.push_back("dependency " + d + " is MISSING");
            next.push_back(upper(target) + "_DEGRADED");
            continue;
        }
        const Node& dep = get(d);
        if (dep.state == NodeState::FAILED || dep.state == NodeState::CORRUPTED) {
            signals.push_back("dependency " + d + " is " + toString(dep.state));
            next.push_back(upper(target) + "_DEGRADED");
        } else if (dep.state == NodeState::DEGRADED || dep.state == NodeState::WARNING ||
                   dep.state == NodeState::CRITICAL) {
            signals.push_back("dependency " + d + " pressure (" + toString(dep.state) + ")");
        }
        if (dep.latencyMs > 400)
            signals.push_back("dependency " + d + " latency high (" + std::to_string(dep.latencyMs) +
                              "ms)");
    }
    if (n.memoryUsedMb * 100 / std::max(1, n.memoryMb) > 90) {
        signals.push_back("memory pressure");
        next.push_back("OOM_RISK");
    }
    if (n.tempC >= n.criticalC - 10) {
        signals.push_back("temperature near critical (" + std::to_string(n.tempC) + "C)");
        next.push_back("THERMAL_SHUTDOWN");
    } else if (n.tempC >= n.thermalLimitC - 10) {
        signals.push_back("temperature rising (" + std::to_string(n.tempC) + "C)");
        next.push_back("CPU_THROTTLE");
    }
    if (n.clockState == ClockState::UNSTABLE || n.clockState == ClockState::CRITICAL) {
        signals.push_back("clock unstable (" + toString(n.clockState) + ")");
        next.push_back("CASCADE_RISK");
    }
    if (n.storageUsedMb * 100 / std::max(1, n.storageMb) > 90) {
        signals.push_back("storage nearly full");
        next.push_back("DISK_FULL_RISK");
    }
    if (n.kernel == KernelState::UNSTABLE || n.instability > 60) {
        signals.push_back("kernel unstable (instability " + std::to_string(n.instability) + ")");
        next.push_back("KERNEL_PANIC_RISK");
    }
    if (!n.faults.empty()) {
        signals.push_back(std::to_string(n.faults.size()) + " active fault(s), worst: " +
                          worstFaultName(n));
        next.push_back("FAULT_ESCALATION");
    }
    if (signals.empty()) {
        o << "  stable: no warning signals.\n  possible next event: NONE (steady state)\n";
    } else {
        for (auto& s : signals) o << "  - " << s << "\n";
        o << "possible next event:\n";
        if (next.empty()) next.push_back("DEGRADATION_RISK");
        std::sort(next.begin(), next.end());
        next.erase(std::unique(next.begin(), next.end()), next.end());
        for (auto& e : next) o << "  " << e << "\n";
    }
    return o.str();
}

std::string System::localInfo() const {
    // Fake local machine: simulated inventory + tick-derived load. No host access.
    int procs = (int)pm_.all().size();
    int load = localLoad(seed_, clock_.tickCount());
    int ramUsed = localRamUsed(seed_, clock_.tickCount(), procs);
    std::ostringstream o;
    o << "HOST: " << localHostname() << "  (simulated)\n";
    o << "CPU: " << localCpuCores() << " cores, load " << load << "%\n";
    o << "RAM: " << ramUsed << " / " << localRamMb() << " MB\n";
    o << "\nProcesses:  " << procs << "\n";
    o << "Interfaces: " << ::override::localInterfaces().size() << "\n";
    o << "Open ports: ";
    int open = 0;
    for (const auto& lp : ::override::localPorts())
        if (lp.state == "OPEN") ++open;
    o << open << "\n";
    o << "Sim time:   " << clock_.now() << "  seed " << seed_ << "\n";
    return o.str();
}

std::string System::localPorts() const {
    std::ostringstream o;
    o << "PORT   SERVICE    STATE\n";
    for (const auto& lp : ::override::localPorts()) {
        o << lp.port << std::string(lp.port < 1000 ? "    " : lp.port < 10000 ? "   " : "  ")
          << lp.service
          << std::string(lp.service.size() < 11 ? 11 - lp.service.size() : 1, ' ')
          << lp.state << "\n";
    }
    return o.str();
}

std::string System::localInterfaces() const {
    std::ostringstream o;
    o << "IFACE  ADDR       STATE\n";
    for (const auto& li : ::override::localInterfaces()) {
        o << li.name << std::string(li.name.size() < 7 ? 7 - li.name.size() : 1, ' ') << li.addr
          << std::string(li.addr.size() < 12 ? 12 - li.addr.size() : 1, ' ')
          << (li.up ? "UP" : "DOWN") << "\n";
    }
    return o.str();
}

std::string System::resources() const {
    std::ostringstream o;
    o << "NODE        CPU   TEMP   RAM_USED/CAP   STOR_USED/CAP  CONNS  STATE\n";
    for (const auto& name : nodeNames()) {
        const Node& n = get(name);
        std::ostringstream mem, stor, conn;
        mem << n.memoryUsedMb << "/" << n.memoryMb;
        stor << n.storageUsedMb << "/" << n.storageMb;
        conn << n.connections << "/" << n.maxConnections;
        o << name << std::string(name.size() < 12 ? 12 - name.size() : 1, ' ') << n.load << "%  "
          << n.tempC << "C  " << mem.str()
          << std::string(mem.str().size() < 13 ? 13 - mem.str().size() : 1, ' ') << stor.str()
          << std::string(stor.str().size() < 14 ? 14 - stor.str().size() : 1, ' ') << conn.str()
          << std::string(conn.str().size() < 7 ? 7 - conn.str().size() : 1, ' ')
          << toString(n.state) << "\n";
    }
    o << "LINK              LOAD  LOSS   STATE\n";
    for (const auto& l : net_.links()) {
        std::string nm = l.a + "<->" + l.b;
        o << nm << std::string(nm.size() < 18 ? 18 - nm.size() : 1, ' ') << (int)l.loadPct << "%  "
          << l.lossPct << "%  " << (l.up ? "UP" : "DOWN") << "\n";
    }
    return o.str();
}

std::string System::effectiveProp(const std::string& observer, const std::string& target,
                                  const std::string& prop) const {
    if (!observer.empty() && hasNode(observer)) {
        const Node& o = get(observer);
        auto it = o.beliefs.find(target);
        if (it != o.beliefs.end()) {
            auto jt = it->second.find(prop);
            if (jt != it->second.end()) return jt->second + " (believed)";
        }
    }
    return getProp(get(target), prop);
}

namespace {
// What each simulated device gates. Single table: deviceInspect and the
// gate sites in net/vfs/hardware agree by construction (see deviceReady).
const char* deviceGates(const std::string& dev) {
    if (dev == "net0") return "iface, packet, ping";
    if (dev == "disk0") return "stored writes (write/copy/move)";
    if (dev == "console") return "boot console";
    return "(inspect only)";
}

std::string deviceTypeOf(const std::string& content) {
    // Seed form: "simulated device: <name>\ntype: <type>\n".
    auto pos = content.find("type:");
    if (pos == std::string::npos) return "?";
    std::string t = content.substr(pos + 5);
    size_t e = t.find('\n');
    if (e != std::string::npos) t.resize(e);
    size_t s = t.find_first_not_of(" \t");
    return (s == std::string::npos) ? "?" : t.substr(s);
}
} // namespace

std::string System::deviceList() const {
    std::vector<VEntry> entries;
    std::string err;
    std::ostringstream o;
    o << "devices (/dev):\n";
    if (!vfs_.list("/dev", entries, err)) return "devices (/dev): (no device tree)\n";
    for (const auto& e : entries) {
        if (e.isDir) continue;
        const VFile* f = vfs_.file("/dev/" + e.name);
        std::string state = (f && f->corrupted) ? "CORRUPTED" : "READY";
        std::string type = f ? deviceTypeOf(f->content) : "?";
        o << "  " << e.name << "  type=" << type << "  " << state
          << "  gates: " << deviceGates(e.name) << "\n";
    }
    return o.str();
}

std::string System::deviceInspect(const std::string& dev) const {
    std::string p = "/dev/" + dev;
    const VFile* f = vfs_.file(p);
    // Known devices whose metadata was deleted report MISSING (restorable);
    // truly unknown names stay an error.
    static const char* known[] = {"null", "zero", "console", "tty0",
                                  "random", "net0", "disk0", "cpu0"};
    bool knownDev = false;
    for (const char* k : known)
        if (dev == k) knownDev = true;
    if (!f && !knownDev) return "error: no such device: " + p;
    std::ostringstream o;
    o << "device " << p << "\n";
    o << "  type:   " << (f ? deviceTypeOf(f->content) : "?") << "\n";
    if (!f) {
        o << "  state:  UNAVAILABLE (/dev/" << dev << " missing)\n";
        o << "  gates:  " << deviceGates(dev) << "\n";
        o << "  detail: (metadata deleted; restore the path to recover)\n";
        return o.str();
    }
    std::string why;
    o << "  state:  " << (deviceReady(dev, why) ? "READY" : ("UNAVAILABLE (" + why + ")")) << "\n";
    o << "  gates:  " << deviceGates(dev) << "\n";
    o << "  detail:\n" << f->content;
    if (!f->content.empty() && f->content.back() != '\n') o << "\n";
    if (f->corrupted) o << "  [CORRUPTED: integrity invalid]\n";
    return o.str();
}

namespace {
// Shared substrate image state, one line: present / MISSING / CORRUPTED.
std::string imageState(const Vfs& vfs, const std::string& path) {
    const VFile* f = vfs.file(path);
    if (!f) return "MISSING";
    return f->corrupted ? "CORRUPTED" : "present";
}
} // namespace

std::string System::kernelStatus() const {
    std::ostringstream o;
    o << "kernel substrate: image=" << imageState(vfs_, "/boot/kernel")
      << " init=" << imageState(vfs_, "/sbin/init") << "/" << imageState(vfs_, "/bin/init")
      << " config=" << imageState(vfs_, "/kernel/kernel.conf") << "\n";
    o << "NODE        KERNEL     MODULES              INSTABILITY  FAULTS\n";
    for (const auto& name : nodeNames()) {
        const Node& n = get(name);
        std::string mods;
        for (const auto& [m, st] : n.modules) {
            if (!mods.empty()) mods += ",";
            mods += m + "=" + st;
        }
        o << name << std::string(name.size() < 12 ? 12 - name.size() : 1, ' ')
          << toString(n.kernel)
          << std::string(toString(n.kernel).size() < 11 ? 11 - toString(n.kernel).size() : 1,
                         ' ')
          << mods << std::string(mods.size() < 22 ? 22 - mods.size() : 1, ' ')
          << n.instability << "           " << n.faults.size() << "\n";
    }
    return o.str();
}

std::string System::kernelInspect(const std::string& target) const {
    if (!hasNode(target)) return "unknown node: " + target;
    const Node& n = get(target);
    std::ostringstream o;
    o << "kernel " << target << "\n";
    o << "  state:       " << toString(n.kernel) << "\n";
    o << "  image:       /boot/kernel [" << imageState(vfs_, "/boot/kernel") << "]\n";
    o << "  init:        /sbin/init [" << imageState(vfs_, "/sbin/init") << "] /bin/init ["
      << imageState(vfs_, "/bin/init") << "]\n";
    o << "  config:      /kernel/kernel.conf [" << imageState(vfs_, "/kernel/kernel.conf")
      << "]\n";
    o << "  instability: " << n.instability << "\n";
    o << "  modules:\n";
    for (const auto& [m, st] : n.modules) {
        o << "    " << m << " [" << st << "] image=" << imageState(vfs_, "/lib/modules/" + m + ".ko") << "\n";
    }
    o << "  last boot phases:\n";
    int shown = 0;
    for (auto it = events_.all().rbegin(); it != events_.all().rend() && shown < 12; ++it) {
        if (it->target == target && it->type == "BOOT_PHASE") {
            o << "    [t=" << it->tick << " #" << it->id << "] " << it->message << "\n";
            ++shown;
        }
    }
    if (shown == 0) o << "    (no boot recorded since world init)\n";
    return o.str();
}

std::string System::uname(const std::string& target) const {
    std::string t = target.empty() ? "server" : target;
    if (!hasNode(t)) return "unknown node: " + t;
    const Node& n = get(t);
    return "OVERKNRL " + t + " sim-0.1.0 " + toString(n.kernel) + " (" +
           hw_.profile().cpuModel + " x" + std::to_string(hw_.profile().cpuCores) + ")";
}

std::string System::dmesg(const std::string& target, int limit) const {
    // Kernel ring: KERNEL_*, BOOT_*, MODULE_* ledger events (unified system,
    // no parallel logging). Newest first like events; node filter optional.
    if (limit < 1) limit = 1;
    std::ostringstream o;
    o << "kernel ring" << (target.empty() ? "" : " (" + target + ")") << ":\n";
    int shown = 0;
    for (auto it = events_.all().rbegin(); it != events_.all().rend() && shown < limit; ++it) {
        bool kernelish = it->type.rfind("KERNEL_", 0) == 0 || it->type.rfind("BOOT_", 0) == 0 ||
                         it->type.rfind("MODULE_", 0) == 0;
        if (!kernelish) continue;
        if (!target.empty() && it->target != target) continue;
        o << "  [t=" << it->tick << " #" << it->id << "] " << it->type << " " << it->source
          << "->" << it->target << ": " << it->message << "\n";
        ++shown;
    }
    if (shown == 0) o << "  (empty)\n";
    return o.str();
}

std::string System::lsmod(const std::string& target) const {
    std::string t = target.empty() ? "server" : target;
    if (!hasNode(t)) return "unknown node: " + t;
    const Node& n = get(t);
    std::ostringstream o;
    o << "modules on " << t << ":\n";
    for (const auto& [m, st] : n.modules) {
        o << "  " << m << " [" << st << "] image="
          << imageState(vfs_, "/lib/modules/" + m + ".ko") << "\n";
    }
    return o.str();
}

std::string System::sysctlGet(const std::string& target, const std::string& key,
                              bool& ok) const {
    ok = true;
    if (!hasNode(target)) {
        ok = false;
        return "unknown node: " + target;
    }
    const Node& n = get(target);
    if (key == "kernel.instability") return std::to_string(n.instability);
    if (key == "thermal.limit") return std::to_string(n.thermalLimitC);
    if (key == "net.base_latency") return std::to_string(n.baseLatencyMs);
    if (key == "vm.swap_mb") return std::to_string(n.swapMb);
    if (key == "kernel.modules") {
        std::string o;
        for (const auto& [m, st] : n.modules) {
            if (!o.empty()) o += ",";
            o += m + "=" + st;
        }
        return o;
    }
    ok = false;
    return "unknown key '" + key +
           "' (kernel.instability|thermal.limit|net.base_latency|vm.swap_mb|kernel.modules)";
}

OpResult System::sysctlSet(const std::string& target, const std::string& key,
                           const std::string& value) {
    if (!hasNode(target)) return OpResult::failure("unknown node: " + target);
    Node& n = get(target);
    auto rangeFail = [&](const std::string& what) {
        return OpResult::failure("invalid value '" + value + "' for " + key + " " + what);
    };
    if (key == "thermal.limit") {
        int v = 0;
        if (!parseInt(value, v) || v < 50 || v > 150) return rangeFail("(int 50..150)");
        uint64_t root =
            emit(0, "USER_SYSCTL", "user", target, "sysctl " + key + "=" + value);
        n.thermalLimitC = v;
        emit(root, "SYSCTL_SET", "kernel", target,
             key + " = " + value + " on " + target, {{"key", key}, {"value", value}});
        propagate(root);
        return OpResult::success(root, key + " = " + value);
    }
    if (key == "net.base_latency") {
        int v = 0;
        if (!parseInt(value, v) || v < 1 || v > 5000) return rangeFail("(int 1..5000)");
        uint64_t root =
            emit(0, "USER_SYSCTL", "user", target, "sysctl " + key + "=" + value);
        n.baseLatencyMs = v;
        emit(root, "SYSCTL_SET", "kernel", target,
             key + " = " + value + " on " + target, {{"key", key}, {"value", value}});
        propagate(root);
        return OpResult::success(root, key + " = " + value);
    }
    if (key == "vm.swap_mb") {
        int v = 0;
        if (!parseInt(value, v) || v < 0 || v > 8192) return rangeFail("(int 0..8192)");
        uint64_t root =
            emit(0, "USER_SYSCTL", "user", target, "sysctl " + key + "=" + value);
        n.swapMb = v;
        emit(root, "SYSCTL_SET", "kernel", target,
             key + " = " + value + " on " + target, {{"key", key}, {"value", value}});
        propagate(root);
        return OpResult::success(root, key + " = " + value);
    }
    bool ok = false;
    std::string cur = sysctlGet(target, key, ok);
    if (ok) return OpResult::failure("key '" + key + "' is read-only (current: " + cur + ")");
    return OpResult::failure(cur);
}

std::string System::threadList(const std::string& proc) const {
    std::ostringstream o;
    o << "threads" << (proc.empty() ? "" : " in " + proc) << ":\n";
    o << "TID      PROCESS        STATE       CPUTICKS\n";
    for (const auto& [pid, p] : pm_.all()) {
        if (!proc.empty() && std::to_string(pid) != proc && p.name != proc) continue;
        for (const auto& t : p.threads) {
            o << t.tid << std::string(std::to_string(t.tid).size() < 9 ? 9 - std::to_string(t.tid).size() : 1, ' ')
              << p.name << std::string(p.name.size() < 15 ? 15 - p.name.size() : 1, ' ')
              << toString(t.state)
              << std::string(toString(t.state).size() < 12 ? 12 - toString(t.state).size() : 1, ' ')
              << t.cpuTicks << "\n";
        }
    }
    return o.str();
}

std::string System::threadInspect(int tid) const {
    const SimThread* t = pm_.findThread(tid);
    if (!t) return "unknown thread: " + std::to_string(tid);
    const SimProcess* p = pm_.procOfThread(tid);
    std::ostringstream o;
    o << "thread " << t->tid << " (" << t->name << ")\n";
    o << "  state:    " << toString(t->state) << "\n";
    o << "  cpu time: " << t->cpuTicks << " ticks\n";
    if (p) o << "  process:  " << p->name << " (pid " << p->pid << ") on " << p->host << "\n";
    return o.str();
}

std::string System::schedView() const {
    // Deterministic priority order: lower nice value first, pid breaks ties.
    // The head of READY is the running entity; the rest are queued behind it.
    struct Entry {
        int pid;
        int priority;
    };
    std::vector<Entry> ready, blocked;
    for (const auto& [pid, p] : pm_.all()) {
        if (p.state == ProcState::RUNNING || p.state == ProcState::SLEEPING ||
            p.state == ProcState::DEGRADED)
            ready.push_back({pid, p.priority});
        else if (p.state == ProcState::PAUSED || p.state == ProcState::BLOCKED ||
                 p.state == ProcState::STOPPED)
            blocked.push_back({pid, p.priority});
    }
    auto byPri = [](const Entry& a, const Entry& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.pid < b.pid;
    };
    std::sort(ready.begin(), ready.end(), byPri);
    std::sort(blocked.begin(), blocked.end(), byPri);
    uint64_t totalCpu = 0;
    for (const auto& [pid, p] : pm_.all()) totalCpu += p.runtime;
    std::ostringstream o;
    o << "scheduler (priority order, nice -20 first):\n";
    o << "  running: ";
    if (ready.empty()) {
        o << "(idle)\n";
    } else {
        const SimProcess* r = pm_.find(std::to_string(ready.front().pid));
        o << (r ? r->name : "?") << " (pid " << ready.front().pid << ")\n";
    }
    o << "  ready (" << (ready.size() > 0 ? ready.size() - 1 : 0) << " queued):";
    for (size_t i = 1; i < ready.size(); ++i) {
        const SimProcess* p = pm_.find(std::to_string(ready[i].pid));
        o << " " << (p ? p->name : "?") << "(" << ready[i].pid << ")";
    }
    o << "\n  blocked (" << blocked.size() << "):";
    for (const auto& e : blocked) {
        const SimProcess* p = pm_.find(std::to_string(e.pid));
        o << " " << (p ? p->name : "?") << "(" << e.pid << ")";
    }
    o << "\n  cpu time: " << totalCpu << " process-ticks total\n";
    return o.str();
}

std::string System::processTree() const {
    // Task tree: roots (parentPid 0) first, children nested by pid order.
    // A root process is a task; its descendants are the task's processes.
    std::map<int, std::vector<int>> kids;
    std::vector<int> roots;
    for (const auto& [pid, p] : pm_.all()) {
        if (p.parentPid == 0) roots.push_back(pid);
        else kids[p.parentPid].push_back(pid);
    }
    std::ostringstream o;
    o << "task tree (task -> process -> thread(s)):\n";
    std::function<void(int, int)> show = [&](int pid, int depth) {
        const SimProcess* p = pm_.find(std::to_string(pid));
        if (!p) return;
        o << std::string((size_t)depth * 2, ' ') << (depth == 0 ? "task " : "proc ") << p->name
          << " (pid " << pid << ":" << toString(p->state) << ", " << p->threads.size()
          << " thread(s))\n";
        auto it = kids.find(pid);
        if (it == kids.end()) return;
        std::vector<int> sorted = it->second;
        std::sort(sorted.begin(), sorted.end());
        for (int c : sorted) show(c, depth + 1);
    };
    std::sort(roots.begin(), roots.end());
    for (int r : roots) show(r, 0);
    return o.str();
}

std::string System::syscallView() const {
    // Observational counters at real engine call sites (never snapshotted or
    // digested, like command history). Same op sequence => same counts.
    static const char* order[] = {"open",  "read",    "write",  "close",  "clone", "execve",
                                 "kill",  "sendmsg", "msgrcv", "msgsnd", "rename", "unlink",
                                 "ioctl", "reboot",  "getdents", "sendfile", nullptr};
    std::ostringstream o;
    o << "syscalls (operator-issued call-site counts):\n";
    uint64_t total = 0;
    for (const char** s = order; *s != nullptr; ++s) {
        auto it = syscallCounts_.find(*s);
        uint64_t n = (it == syscallCounts_.end()) ? 0 : it->second;
        total += n;
        o << "  " << *s << ": " << n << "\n";
    }
    for (const auto& [k, v] : syscallCounts_) {
        bool known = false;
        for (const char** s = order; *s != nullptr; ++s)
            if (k == *s) {
                known = true;
                break;
            }
        if (!known) {
            total += v;
            o << "  " << k << ": " << v << "\n";
        }
    }
    o << "  total: " << total << "\n";
    return o.str();
}

} // namespace override

