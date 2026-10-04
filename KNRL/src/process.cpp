#include "override/process.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>

namespace override {

std::string toString(ProcState s) {
    switch (s) {
        case ProcState::RUNNING: return "RUNNING";
        case ProcState::SLEEPING: return "SLEEPING";
        case ProcState::PAUSED: return "PAUSED";
        case ProcState::BLOCKED: return "BLOCKED";
        case ProcState::DEGRADED: return "DEGRADED";
        case ProcState::STOPPED: return "STOPPED";
        case ProcState::CRASHED: return "CRASHED";
        case ProcState::ZOMBIE: return "ZOMBIE";
        case ProcState::KILLED: return "KILLED";
        case ProcState::TERMINATED: return "TERMINATED";
    }
    return "UNKNOWN";
}

ProcState procStateFromString(const std::string& s) {
    if (s == "RUNNING") return ProcState::RUNNING;
    if (s == "SLEEPING") return ProcState::SLEEPING;
    if (s == "PAUSED") return ProcState::PAUSED;
    if (s == "BLOCKED") return ProcState::BLOCKED;
    if (s == "DEGRADED") return ProcState::DEGRADED;
    if (s == "STOPPED") return ProcState::STOPPED;
    if (s == "CRASHED") return ProcState::CRASHED;
    if (s == "ZOMBIE") return ProcState::ZOMBIE;
    if (s == "KILLED") return ProcState::KILLED;
    if (s == "TERMINATED") return ProcState::TERMINATED;
    throw std::runtime_error("unknown proc state: " + s);
}

namespace {
uint64_t splitmix64local(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

bool parsePid(const std::string& s, int& out) {
    if (s.empty()) return false;
    long long acc = 0;
    for (char c : s) {
        if (!std::isdigit((unsigned char)c)) return false;
        acc = acc * 10 + (c - '0');
        if (acc > 1000000) return false;
    }
    out = (int)acc;
    return true;
}

bool validProcName(const std::string& n) {
    if (n.empty() || n.size() > 32) return false;
    if (n[0] < 'a' || n[0] > 'z') return false;
    for (char c : n) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// Threads follow their process through every lifecycle transition (no strobe
// of per-thread notes: the process note carries the story). Dead mail is
// dropped when requested (death, explicit restart).
void setThreads(SimProcess& p, ProcState s, bool clearMailbox) {
    for (auto& t : p.threads) t.state = s;
    if (clearMailbox) p.mailbox.clear();
}
} // namespace

ProcessManager::ProcessManager() = default;

SimProcess* ProcessManager::find(const std::string& pidOrName) {
    int pid = 0;
    if (parsePid(pidOrName, pid)) {
        auto it = procs_.find(pid);
        if (it != procs_.end()) return &it->second;
        return nullptr;
    }
    std::string low = pidOrName;
    for (auto& c : low) c = (char)std::tolower((unsigned char)c);
    for (auto& [p, proc] : procs_) { // sorted: lowest pid wins
        if (proc.name == low) return &proc;
    }
    return nullptr;
}

const SimProcess* ProcessManager::find(const std::string& pidOrName) const {
    return const_cast<ProcessManager*>(this)->find(pidOrName);
}

SimThread* ProcessManager::findThread(int tid) {
    for (auto& [pid, p] : procs_)
        for (auto& t : p.threads)
            if (t.tid == tid) return &t;
    return nullptr;
}

const SimThread* ProcessManager::findThread(int tid) const {
    return const_cast<ProcessManager*>(this)->findThread(tid);
}

const SimProcess* ProcessManager::procOfThread(int tid) const {
    for (const auto& [pid, p] : procs_)
        for (const auto& t : p.threads)
            if (t.tid == tid) return &p;
    return nullptr;
}

int ProcessManager::spawn(const std::string& name, const std::string& type,
                          const std::string& host, int parentPid, std::vector<ProcNote>& out,
                          std::string& err) {
    std::string low = name;
    for (auto& c : low) c = (char)std::tolower((unsigned char)c);
    if (!validProcName(low)) {
        err = "invalid process name '" + name + "' (use [a-z][a-z0-9_-]*)";
        return 0;
    }
    if (parentPid != 0 && !procs_.count(parentPid)) {
        err = "unknown parent pid: " + std::to_string(parentPid);
        return 0;
    }
    SimProcess p;
    p.pid = nextPid_++;
    p.name = low;
    p.type = type.empty() ? "worker" : type;
    p.state = ProcState::RUNNING;
    p.cpu = 5;
    p.memMb = 64;
    p.priority = 0;
    p.parentPid = parentPid;
    p.host = host;
    p.runtime = 0;
    // Every process starts with its main thread (slot 1).
    SimThread main;
    main.tid = p.pid * 1000 + 1;
    main.name = low + "/main";
    main.state = ProcState::RUNNING;
    p.threads.push_back(std::move(main));
    p.threadSeq = 1;
    procs_[p.pid] = p;
    out.push_back({"PROC_SPAWNED", low, host,
                   "proc " + low + " spawned (pid " + std::to_string(p.pid) + ") on " + host});
    return p.pid;
}

bool ProcessManager::kill(const std::string& pidOrName, std::vector<ProcNote>& out,
                           std::string& err) {
    SimProcess* p = find(pidOrName);
    if (!p) {
        err = "unknown process: " + pidOrName;
        return false;
    }
    if (p->state == ProcState::TERMINATED || p->state == ProcState::KILLED) {
        err = p->name + " (pid " + std::to_string(p->pid) + ") already " + toString(p->state);
        return false;
    }
    p->state = ProcState::CRASHED; // kill = crash in-sim (watchdog may restart)
    p->cpu = 0;
    setThreads(*p, ProcState::CRASHED, false);
    out.push_back({"PROC_CRASHED", p->name, p->host,
                   "proc " + p->name + " (pid " + std::to_string(p->pid) + ") CRASHED on " +
                       p->host});
    return true;
}

// Children of a reaped process become ZOMBIE (dead but unreaped).
// Returns the number orphaned. Callers hold no iterators across this call.
static void zombifyChildren(std::map<int, SimProcess>& procs, int deadPid,
                            std::vector<ProcNote>& out) {
    for (auto& [pid, c] : procs) {
        if (c.parentPid != deadPid) continue;
        if (c.state == ProcState::TERMINATED || c.state == ProcState::KILLED ||
            c.state == ProcState::ZOMBIE)
            continue;
        c.state = ProcState::ZOMBIE;
        c.cpu = 0;
        out.push_back({"PROC_ZOMBIE", c.name, c.host,
                       "proc " + c.name + " (pid " + std::to_string(pid) +
                           ") ZOMBIE: parent reaped, awaiting reap"});
    }
}

bool ProcessManager::failAs(const std::string& pidOrName, const std::string& reason,
                           std::vector<ProcNote>& out, std::string& err) {
    SimProcess* p = find(pidOrName);
    if (!p) {
        err = "unknown process: " + pidOrName;
        return false;
    }
    std::string tag = "proc " + p->name + " (pid " + std::to_string(p->pid) + ")";
    auto terminal = [&]() {
        return p->state == ProcState::TERMINATED || p->state == ProcState::KILLED;
    };
    if (reason == "crash" || reason == "segfault") {
        if (terminal()) {
            err = p->name + " (pid " + std::to_string(p->pid) + ") already " + toString(p->state);
            return false;
        }
        p->state = ProcState::CRASHED;
        p->cpu = 0;
        setThreads(*p, ProcState::CRASHED, false);
        std::string what = (reason == "segfault") ? " SEGFAULT" : " CRASHED";
        std::string type = (reason == "segfault") ? "PROC_SEGFAULT" : "PROC_CRASHED";
        out.push_back({type, p->name, p->host, tag + what + " on " + p->host});
        return true;
    }
    if (reason == "oom") {
        if (terminal()) {
            err = p->name + " (pid " + std::to_string(p->pid) + ") already " + toString(p->state);
            return false;
        }
        int deadPid = p->pid;
        p->state = ProcState::TERMINATED; // OOM kills stay dead: no watchdog restart
        p->cpu = 0;
        setThreads(*p, ProcState::TERMINATED, true);
        out.push_back({"PROC_OOM_KILL", p->name, p->host,
                       tag + " OOM-KILLED on " + p->host + " (freed " +
                           std::to_string(p->memMb) + "MB)"});
        zombifyChildren(procs_, deadPid, out);
        return true;
    }
    if (reason == "sigkill") {
        if (terminal()) {
            err = p->name + " (pid " + std::to_string(p->pid) + ") already " + toString(p->state);
            return false;
        }
        int deadPid = p->pid;
        p->state = ProcState::KILLED; // dead by signal: kept as record, no restart
        p->cpu = 0;
        setThreads(*p, ProcState::KILLED, true);
        out.push_back({"PROC_KILLED", p->name, p->host, tag + " KILLED on " + p->host});
        zombifyChildren(procs_, deadPid, out);
        return true;
    }
    if (reason == "zombie") {
        if (terminal() || p->state == ProcState::ZOMBIE) {
            err = p->name + " is " + toString(p->state) + ", cannot zombify";
            return false;
        }
        p->state = ProcState::ZOMBIE;
        p->cpu = 0;
        setThreads(*p, ProcState::ZOMBIE, false);
        out.push_back({"PROC_ZOMBIE", p->name, p->host, tag + " ZOMBIE on " + p->host});
        return true;
    }
    if (reason == "runaway") {
        if (p->state != ProcState::RUNNING && p->state != ProcState::SLEEPING) {
            err = p->name + " is " + toString(p->state) + ", cannot run away";
            return false;
        }
        p->state = ProcState::RUNNING;
        p->cpu = 99;
        setThreads(*p, ProcState::RUNNING, false);
        out.push_back({"PROC_RUNAWAY", p->name, p->host,
                       tag + " CPU runaway (99%) on " + p->host});
        return true;
    }
    if (reason == "deadlock") {
        if (p->state != ProcState::RUNNING && p->state != ProcState::SLEEPING) {
            err = p->name + " is " + toString(p->state) + ", cannot deadlock";
            return false;
        }
        p->state = ProcState::STOPPED;
        p->cpu = 0;
        setThreads(*p, ProcState::STOPPED, false);
        out.push_back({"PROC_DEADLOCK", p->name, p->host,
                       tag + " DEADLOCKED on " + p->host + " (STOPPED)"});
        return true;
    }
    if (reason == "block") {
        if (p->state != ProcState::RUNNING && p->state != ProcState::SLEEPING &&
            p->state != ProcState::DEGRADED) {
            err = p->name + " is " + toString(p->state) + ", cannot block";
            return false;
        }
        p->state = ProcState::BLOCKED;
        p->cpu = 0;
        setThreads(*p, ProcState::BLOCKED, false);
        out.push_back({"PROC_BLOCKED", p->name, p->host,
                       tag + " BLOCKED on " + p->host + " (waiting on dependency)"});
        return true;
    }
    if (reason == "degrade") {
        if (p->state != ProcState::RUNNING && p->state != ProcState::SLEEPING) {
            err = p->name + " is " + toString(p->state) + ", cannot degrade";
            return false;
        }
        p->state = ProcState::DEGRADED;
        setThreads(*p, ProcState::DEGRADED, false);
        out.push_back({"PROC_DEGRADED", p->name, p->host,
                       tag + " DEGRADED on " + p->host + " (running slowly)"});
        return true;
    }
    err = "unknown failure reason '" + reason +
          "' (crash|oom|segfault|runaway|deadlock|sigkill|zombie|block|degrade)";
    return false;
}

bool ProcessManager::pause(const std::string& pidOrName, std::vector<ProcNote>& out,
                           std::string& err) {
    SimProcess* p = find(pidOrName);
    if (!p) {
        err = "unknown process: " + pidOrName;
        return false;
    }
    if (p->state != ProcState::RUNNING && p->state != ProcState::SLEEPING) {
        err = p->name + " is " + toString(p->state) + ", cannot pause";
        return false;
    }
    p->state = ProcState::PAUSED;
    p->cpu = 0;
    setThreads(*p, ProcState::PAUSED, false);
    out.push_back({"PROC_PAUSED", p->name, p->host, "proc " + p->name + " PAUSED"});
    return true;
}

bool ProcessManager::resume(const std::string& pidOrName, std::vector<ProcNote>& out,
                            std::string& err) {
    SimProcess* p = find(pidOrName);
    if (!p) {
        err = "unknown process: " + pidOrName;
        return false;
    }
    if (p->state != ProcState::PAUSED && p->state != ProcState::STOPPED &&
        p->state != ProcState::SLEEPING && p->state != ProcState::BLOCKED &&
        p->state != ProcState::DEGRADED) {
        err = p->name + " is " + toString(p->state) + ", cannot resume";
        return false;
    }
    p->state = ProcState::RUNNING;
    p->cpu = 5;
    setThreads(*p, ProcState::RUNNING, false);
    out.push_back({"PROC_RESUMED", p->name, p->host, "proc " + p->name + " RUNNING"});
    return true;
}

bool ProcessManager::restart(const std::string& pidOrName, std::vector<ProcNote>& out,
                             std::string& err) {
    SimProcess* p = find(pidOrName);
    if (!p) {
        err = "unknown process: " + pidOrName;
        return false;
    }
    if (p->state == ProcState::TERMINATED || p->state == ProcState::KILLED) {
        err = p->name + " is " + toString(p->state) + ", cannot restart (spawn a new one)";
        return false;
    }
    p->state = ProcState::RUNNING;
    p->cpu = 5;
    p->runtime = 0;
    setThreads(*p, ProcState::RUNNING, true); // fresh address space: mail dropped
    out.push_back({"PROC_RESTARTED", p->name, p->host, "proc " + p->name + " restarted"});
    return true;
}

int ProcessManager::threadSpawn(const std::string& pidOrName, const std::string& tname,
                                std::vector<ProcNote>& out, std::string& err) {
    SimProcess* p = find(pidOrName);
    if (!p) {
        err = "unknown process: " + pidOrName;
        return 0;
    }
    if (p->state == ProcState::TERMINATED || p->state == ProcState::KILLED) {
        err = p->name + " is " + toString(p->state) + ", cannot spawn threads";
        return 0;
    }
    if (p->threads.size() >= kMaxThreads) {
        err = p->name + " already has " + std::to_string(kMaxThreads) + " threads";
        return 0;
    }
    std::string low = tname;
    for (auto& c : low) c = (char)std::tolower((unsigned char)c);
    if (!validProcName(low)) {
        err = "invalid thread name '" + tname + "' (use [a-z][a-z0-9_-]*)";
        return 0;
    }
    SimThread t;
    t.tid = p->pid * 1000 + (++p->threadSeq);
    t.name = p->name + "/" + low;
    t.state = ProcState::RUNNING;
    p->threads.push_back(std::move(t));
    out.push_back({"PROC_THREAD_SPAWNED", p->name, p->host,
                   "thread " + low + " spawned in " + p->name + " (tid " +
                       std::to_string(p->pid * 1000 + p->threadSeq) + ")"});
    return p->pid * 1000 + p->threadSeq;
}

void ProcessManager::crashHost(const std::string& host, std::vector<ProcNote>& out) {
    for (auto& [pid, p] : procs_) {
        if (p.host != host) continue;
        if (p.state == ProcState::TERMINATED || p.state == ProcState::KILLED) continue;
        p.state = ProcState::CRASHED;
        p.cpu = 0;
        setThreads(p, ProcState::CRASHED, false);
        out.push_back({"PROC_CRASHED", p.name, host,
                       "proc " + p.name + " (pid " + std::to_string(pid) +
                           ") CRASHED: host " + host + " down"});
    }
}

void ProcessManager::restartHost(const std::string& host, std::vector<ProcNote>& out) {    for (auto& [pid, p] : procs_) {
        if (p.host != host) continue;
        // Dead records (TERMINATED/KILLED) and unreaped ZOMBIEs need explicit
        // action; a host recovery only revives the rest.
        if (p.state == ProcState::TERMINATED || p.state == ProcState::KILLED ||
            p.state == ProcState::ZOMBIE)
            continue;
        p.state = ProcState::RUNNING;
        p.cpu = 5;
        p.runtime = 0;
        setThreads(p, ProcState::RUNNING, false);
        out.push_back({"PROC_RESTARTED", p.name, host,
                       "proc " + p.name + " (pid " + std::to_string(pid) +
                           ") RUNNING: host " + host + " recovered"});
    }
}

void ProcessManager::stopHost(const std::string& host, std::vector<ProcNote>& out) {
    for (auto& [pid, p] : procs_) {
        if (p.host != host) continue;
        if (p.state == ProcState::TERMINATED || p.state == ProcState::KILLED) continue;
        p.state = ProcState::STOPPED;
        p.cpu = 0;
        setThreads(p, ProcState::STOPPED, false);
        out.push_back({"PROC_STOPPED", p.name, host,
                       "proc " + p.name + " (pid " + std::to_string(pid) +
                           ") STOPPED: host " + host + " halted"});
    }
}

int ProcessManager::purgeHost(const std::string& host, std::vector<ProcNote>& out) {
    int n = 0;
    for (auto it = procs_.begin(); it != procs_.end();) {
        if (it->second.host == host) {
            it = procs_.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    if (n > 0)
        out.push_back({"PROC_PURGED", "engine", host,
                       std::to_string(n) + " process(es) on " + host + " TERMINATED (host removed)"});
    return n;
}

int ProcessManager::runningOn(const std::string& host) const {
    int n = 0;
    for (const auto& [pid, p] : procs_) {
        if (p.host == host &&
            (p.state == ProcState::RUNNING || p.state == ProcState::SLEEPING ||
             p.state == ProcState::DEGRADED))
            ++n;
    }
    return n;
}

int ProcessManager::totalOn(const std::string& host) const {
    int n = 0;
    for (const auto& [pid, p] : procs_)
        if (p.host == host && p.state != ProcState::TERMINATED && p.state != ProcState::KILLED)
            ++n;
    return n;
}

void ProcessManager::onTick(uint64_t tick, uint64_t& rng, bool autoFaults, int autoRate,
                            const std::function<bool(const std::string&)>& serving,
                            std::vector<ProcNote>& out) {
    for (auto& [pid, p] : procs_) { // sorted: deterministic
        if (p.state == ProcState::TERMINATED || p.state == ProcState::KILLED) continue;
        if (p.state == ProcState::RUNNING || p.state == ProcState::SLEEPING ||
            p.state == ProcState::DEGRADED) {
            ++p.runtime;
            for (auto& t : p.threads)
                if (t.state == ProcState::RUNNING) ++t.cpuTicks;
            // cpu/mem drift from the shared seeded stream (deterministic)
            int wobble = (int)(splitmix64local(rng) % 11) - 5; // -5..+5
            if (p.state == ProcState::DEGRADED)
                p.cpu = std::clamp(p.cpu + wobble / 2, 1, 60); // degraded: sluggish
            else
                p.cpu = std::clamp(p.cpu + wobble, 1, 99);
            if (p.state != ProcState::DEGRADED && (splitmix64local(rng) % 100) < 12) {
                // occasional sleep/wake flicker
                p.state = (p.state == ProcState::RUNNING) ? ProcState::SLEEPING : ProcState::RUNNING;
            }
        } else if (p.state == ProcState::CRASHED) {
            // watchdog: restart crashed procs on serving hosts (recovery events),
            // but never on a down host.
            if (autoFaults && serving(p.host) && (splitmix64local(rng) % 100) < (uint64_t)(autoRate / 2)) {
                p.state = ProcState::RUNNING;
                p.cpu = 5;
                p.runtime = 0;
                setThreads(p, ProcState::RUNNING, false);
                out.push_back({"PROC_RESTARTED", p.name, p.host,
                               "proc " + p.name + " (pid " + std::to_string(pid) +
                                   ") restarted by watchdog"});
            }
        }
    }
    (void)tick;
}

bool ProcessManager::validateAll(std::string* err) const {
    for (const auto& [pid, p] : procs_) {
        if (pid <= 0 || p.pid != pid) {
            if (err) *err = "process pid mismatch";
            return false;
        }
        if (p.cpu < 0 || p.cpu > 100 || p.memMb < 0) {
            if (err) *err = p.name + ": resource out of range";
            return false;
        }
        if (p.priority < -20 || p.priority > 19) {
            if (err) *err = p.name + ": priority out of range";
            return false;
        }
        if (p.parentPid != 0 && !procs_.count(p.parentPid)) {
            if (err) *err = p.name + ": dangling parent pid";
            return false;
        }
        if ((p.state == ProcState::TERMINATED || p.state == ProcState::KILLED) && p.cpu != 0) {
            if (err) *err = p.name + ": dead process must have cpu 0";
            return false;
        }
        if (p.threadSeq < (int)p.threads.size()) {
            if (err) *err = p.name + ": thread sequence behind thread count";
            return false;
        }
        if (p.threads.size() > kMaxThreads) {
            if (err) *err = p.name + ": too many threads";
            return false;
        }
        if (p.mailbox.size() > kMaxMailbox) {
            if (err) *err = p.name + ": mailbox overflow";
            return false;
        }
        for (size_t i = 0; i < p.threads.size(); ++i) {
            const auto& t = p.threads[i];
            int slot = t.tid - p.pid * 1000; // slots are 1-based, bounded by threadSeq
            if (slot < 1 || slot > p.threadSeq) {
                if (err) *err = p.name + ": thread id out of scheme";
                return false;
            }
            for (size_t j = i + 1; j < p.threads.size(); ++j) {
                if (p.threads[j].tid == t.tid) {
                    if (err) *err = p.name + ": duplicate thread id";
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace override
