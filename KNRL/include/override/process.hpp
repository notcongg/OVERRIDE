#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace override {

// Simulated process states (fake process subsystem; no OS interaction).
// Terminal: TERMINATED (reaped), KILLED (dead by signal, kept as record).
// ZOMBIE: dead but unreaped (orphaned); kill reaps it, restart revives it.
enum class ProcState { RUNNING, SLEEPING, PAUSED, BLOCKED, DEGRADED, STOPPED, CRASHED, ZOMBIE, KILLED, TERMINATED };

std::string toString(ProcState s);
ProcState procStateFromString(const std::string& s); // throws on unknown

// A simulated thread: real stored state inside its process (snapshotted,
// digested, validated with everything else). tid = pid*1000 + slot.
struct SimThread {
    int tid = 0;
    std::string name;
    ProcState state = ProcState::RUNNING;
    uint64_t cpuTicks = 0; // ticks spent RUNNING
};

struct SimProcess {
    int pid = 0;
    std::string name;
    std::string type = "worker"; // service | worker | daemon | shell | db ...
    ProcState state = ProcState::RUNNING;
    int cpu = 5;         // 0..100 %
    int memMb = 64;      // >= 0
    int priority = 0;    // -20..19 (nice-like)
    int parentPid = 0;   // 0 = init/none
    std::string host;    // node this process runs on
    uint64_t runtime = 0; // ticks spent alive (not TERMINATED)
    std::vector<SimThread> threads;     // task -> process -> thread(s); slot 1 = main
    int threadSeq = 0;                  // ever-spawned thread slots (tid = pid*1000+slot)
    std::vector<std::string> mailbox;   // queued IPC messages (cap kMaxMailbox)
};

// Outbox entry: ProcessManager never touches the event ledger itself.
// System::onTick emits these with the tick's causal root, so all process
// activity flows through the single unified event system.
struct ProcNote {
    std::string type;   // e.g. PROC_CRASHED
    std::string source; // proc name
    std::string target; // host node
    std::string message;
};

class ProcessManager {
public:
    static const size_t kMaxThreads = 16;
    static const size_t kMaxMailbox = 64;

    ProcessManager();

    const std::map<int, SimProcess>& all() const { return procs_; }
    int nextPid() const { return nextPid_; }
    void setNextPid(int n) { nextPid_ = n; }
    // Full-state restore (checkpoints). Replaces the table wholesale.
    // Threads/mailboxes live inside SimProcess, so they restore with it.
    void restoreSnapshot(const std::map<int, SimProcess>& procs, int nextPid) {
        procs_ = procs;
        nextPid_ = nextPid;
    }
    void clear() {
        procs_.clear();
        nextPid_ = 101;
    }

    // pidOrName: integer pid, or process name (lowest pid wins).
    SimProcess* find(const std::string& pidOrName);
    const SimProcess* find(const std::string& pidOrName) const;
    // Thread lookup across all processes (tid = pid*1000 + slot).
    SimThread* findThread(int tid);
    const SimThread* findThread(int tid) const;
    const SimProcess* procOfThread(int tid) const;

    // All return pid or 0 + note pushed on failure/success info.
    int spawn(const std::string& name, const std::string& type, const std::string& host,
              int parentPid, std::vector<ProcNote>& out, std::string& err);
    bool kill(const std::string& pidOrName, std::vector<ProcNote>& out, std::string& err);
    // Fail with a simulated cause: crash|oom|segfault|runaway|deadlock.
    // oom terminates (no watchdog restart); runaway pegs cpu at 99;
    // deadlock parks the process STOPPED until resume/restart.
    bool failAs(const std::string& pidOrName, const std::string& reason,
                std::vector<ProcNote>& out, std::string& err);
    bool pause(const std::string& pidOrName, std::vector<ProcNote>& out, std::string& err);
    bool resume(const std::string& pidOrName, std::vector<ProcNote>& out, std::string& err);
    bool restart(const std::string& pidOrName, std::vector<ProcNote>& out, std::string& err);
    // Spawn a thread inside a live process (slot = ++threadSeq).
    int threadSpawn(const std::string& pidOrName, const std::string& tname,
                    std::vector<ProcNote>& out, std::string& err);

    // Bulk host coupling (node break/repair/remove).
    void crashHost(const std::string& host, std::vector<ProcNote>& out);
    void restartHost(const std::string& host, std::vector<ProcNote>& out);
    // Orderly stop: every live process STOPPED (resumable), unlike crashHost.
    void stopHost(const std::string& host, std::vector<ProcNote>& out);
    int purgeHost(const std::string& host, std::vector<ProcNote>& out);

    int runningOn(const std::string& host) const;
    int totalOn(const std::string& host) const;

    // Per-tick evolution. serving(host) reports whether the host node can run
    // processes. Watchdog restarts CRASHED procs on serving hosts only.
    void onTick(uint64_t tick, uint64_t& rng, bool autoFaults, int autoRate,
                const std::function<bool(const std::string&)>& serving,
                std::vector<ProcNote>& out);

    bool validateAll(std::string* err) const;

private:
    std::map<int, SimProcess> procs_;
    int nextPid_ = 101;
};

} // namespace override
