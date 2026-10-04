#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "override/clock.hpp"
#include "override/event.hpp"
#include "override/fault.hpp"
#include "override/hardware.hpp"
#include "override/history.hpp"
#include "override/host.hpp"
#include "override/network.hpp"
#include "override/node.hpp"
#include "override/packages.hpp"
#include "override/process.hpp"
#include "override/progress.hpp"
#include "ordc/ord.hpp"
#include "override/service.hpp"
#include "override/vfs.hpp"

namespace override {

// Structured result for every mutating engine operation.
// ok=false means the world was NOT changed (except for a *_REJECTED ledger event).
struct OpResult {
    bool ok = false;
    uint64_t eventId = 0; // root event of this operation (0 if none emitted)
    std::string error;    // set when !ok
    std::string info;     // human-readable summary when ok

    static OpResult success(uint64_t id, std::string info = "") {
        return {true, id, "", std::move(info)};
    }
    static OpResult failure(std::string err, uint64_t id = 0) {
        return {false, id, std::move(err), ""};
    }
};

struct PacketResult {
    enum class Outcome { DELIVERED, DROPPED, UNREACHABLE };
    Outcome outcome = Outcome::UNREACHABLE;
    std::string detail;
    int latencyMs = 0;
    std::vector<std::string> path; // nodes traversed, inclusive
};

// Central simulation engine. Owns all world state.
// Shell (OVERSHELL) is a thin layer over this class.
class System {
public:
    System();
    void initDefaultWorld();
    // Structured world initialization: the same work as initDefaultWorld,
    // reported as genuinely completed stages (identity/nodes/network/
    // processes/rootfs/events/validate). Returns false + err when the fresh
    // world violates invariants (never for defaults; guards future edits).
    bool initWorldPipeline(ProgressCb progress, std::string& err);

    // --- state access ---
    bool hasNode(const std::string& name) const;
    Node& get(const std::string& name); // throws if missing
    const Node& get(const std::string& name) const;
    std::vector<std::string> nodeNames() const;
    Clock& clock() { return clock_; }
    const Clock& clock() const { return clock_; }
    EventLog& events() { return events_; }
    const EventLog& events() const { return events_; }
    History& history() { return history_; }
    const History& history() const { return history_; }
    Network& network() { return net_; }
    const Network& network() const { return net_; }
    ProcessManager& processes() { return pm_; }
    const ProcessManager& processes() const { return pm_; }
    ServiceManager& services() { return svc_; }
    const ServiceManager& services() const { return svc_; }
    HardwareManager& hardware() { return hw_; }
    const HardwareManager& hardware() const { return hw_; }
    std::string mode() const { return mode_; }
    void setMode(const std::string& m) { mode_ = m; }

    // --- autonomous failures (entropy engine) ---
    bool autoFaults() const { return autoFaults_; }
    int autoRate() const { return autoRate_; }
    void setAutoFaults(bool on);
    // rate: spontaneous-event probability % per node per tick, 0..100.
    OpResult setAutoRate(int rate);

    // --- determinism ---
    uint64_t seed() const { return seed_; }
    void setSeed(uint64_t s);
    // --- recovery guidance (read-only diagnosis; never mutates) ---
    // rec <target> [-h]: inspects live state and explains failure +
    // recovery approaches. Pure observation: no repairs, no state change.
    std::string recoveryReport(const std::string& target, const std::string& sub,
                               bool detailed) const;
    // --- world identity + lifetime (persisted in .ord files) ---
    // worldId is w1..w6 when backed by a world file, "" for an unbacked
    // session. Identity/meta/uptime never enter digests (simulation state
    // only); checkpoint/restore/rewind never roll them back.
    const std::string& worldId() const { return worldId_; }
    void setWorldId(const std::string& id) { worldId_ = id; }
    const std::string& worldName() const { return worldName_; }
    void setWorldName(const std::string& n) { worldName_ = n; }
    const std::string& worldNote() const { return worldNote_; }
    void setWorldNote(const std::string& n) { worldNote_ = n; }
    int64_t worldCreatedAt() const { return worldCreatedAt_; }
    void setWorldCreatedAt(int64_t t) { worldCreatedAt_ = t; }
    uint64_t worldCreatedTick() const { return worldCreatedTick_; }
    void setWorldCreatedTick(uint64_t t) { worldCreatedTick_ = t; }
    void stampWorldSavedTick() { worldSavedTick_ = clock_.tickCount(); }
    // Uptime sessions take an explicit wall-clock `now` (seconds) so tests
    // stay deterministic without sleeps. commit() folds elapsed time into the
    // total and re-baselines (repeated saves never double-count).
    void beginUptimeSession(int64_t now) { uptimeSessionStart_ = now; }
    void commitUptimeSession(int64_t now);
    void endUptimeSession(int64_t now);
    int64_t sessionUptimeSecs(int64_t now) const;
    uint64_t allTimeUptimeSecs(int64_t now) const;
    void setUptimeTotalSecs(uint64_t s) { uptimeTotalSecs_ = s; }
    // --- persistence (ORD documents; see ORDC) ---
    // Snapshots (checkpoints) and command history are runtime-only and are
    // NOT persisted; everything authoritative is. Serialize fails loudly
    // (false + err) when state contains unrepresentable data; deserialize
    // leaves the System unchanged on failure with a file:line diagnostic.
    bool serializeWorldDoc(ordc::OrdDoc& doc, std::string& err) const;
    bool deserializeWorldDoc(const ordc::OrdDoc& doc, std::string& err);
    // Canonical digests for replay/equivalence checks.
    std::string worldDigest() const; // nodes + links + clock + seed (no events)
    std::string digest() const;      // worldDigest + full event ledger

    // --- invariants ---
    bool validateAll(std::string* err) const;

    // --- world editing ---
    OpResult addNode(const std::string& name, const std::string& type = "service");
    OpResult removeNode(const std::string& name);
    OpResult addDependency(const std::string& node, const std::string& dep);

    // --- manipulation ---
    OpResult breakNode(const std::string& target);   // -> FAILED
    OpResult repairNode(const std::string& target);  // -> RESTARTING -> ONLINE
    OpResult overrideProp(const std::string& target, const std::string& prop,
                          const std::string& value);
    OpResult injectFault(const std::string& target, const std::string& fault,
                         const std::string& value = "");
    OpResult deceive(const std::string& observer, const std::string& target,
                     const std::string& prop, const std::string& value);
    OpResult clearBeliefs(const std::string& observer, const std::string& target = "");
    // Seeded pseudo-random fault injection (CHAOS mode).
    OpResult chaosStrike(const std::string& target = "");

    // --- simulated processes (see process.hpp; events go to the same ledger) ---
    OpResult procSpawn(const std::string& name, const std::string& type = "worker",
                       const std::string& host = "", int parentPid = 0);
    OpResult procKill(const std::string& pidOrName, const std::string& reason = "crash");
    OpResult procPause(const std::string& pidOrName);
    OpResult procResume(const std::string& pidOrName);
    OpResult procRestart(const std::string& pidOrName);
    // Simulated signals: TERM/STOP park (PAUSED), CONT resumes, KILL kills
    // (KILLED, no restart), HUP restarts. Real state transitions, same as the
    // equivalent process operations.
    OpResult procSignal(const std::string& sig, const std::string& pidOrName);
    // Threads live inside SimProcess (task -> process -> thread); the process
    // manager stays the single registry, no second manager.
    OpResult threadSpawn(const std::string& pidOrName, const std::string& tname);
    std::string threadList(const std::string& proc = "") const;
    std::string threadInspect(int tid) const;
    // Scheduler + task views, derived from live process state (deterministic).
    std::string schedView() const;
    std::string processTree() const;
    // Syscall accounting: observational counters incremented at real engine
    // call sites (excluded from digest/snapshots, like command history).
    std::string syscallView() const;
    // IPC mailboxes: bounded per-proc queues; send/recv mutate real state.
    OpResult ipcSend(const std::string& proc, const std::string& message);
    OpResult ipcRecv(const std::string& proc);
    std::string ipcList(const std::string& proc = "") const;
    std::string psList() const;
    std::string procInspect(const std::string& pidOrName) const;

    // --- simulated services (OVERKNRL; see service.hpp) ---
    // A service binds a binary (+ optional config file) on a node to a
    // backing process. ALWAYS-policy services are (re)started by ticks,
    // repair and reboot; a down service raises a node-level service-down
    // fault so dependency propagation sees it.
    OpResult serviceSpawn(const std::string& name, const std::string& node,
                          const std::string& binary, const std::string& config = "",
                          const std::string& policy = "ALWAYS");
    OpResult serviceStart(const std::string& name);
    OpResult serviceStop(const std::string& name);
    OpResult serviceRestart(const std::string& name);
    OpResult serviceRemove(const std::string& name);
    std::string serviceList() const;
    std::string serviceInspect(const std::string& name) const;

    // --- fault engine (unified failure model; see fault.hpp) ---
    // Raise a named fault (stacking: re-raise escalates). Returns event id.
    uint64_t raiseFault(const std::string& target, const std::string& fault, Severity severity,
                        uint64_t cause, const std::string& detail);
    // Resolve one named fault with kind-appropriate cleanup.
    OpResult resolveFault(const std::string& target, const std::string& fault);
    // Recovery operations with explicit, limited scope.
    OpResult coolNode(const std::string& target);   // thermal only
    OpResult rebootNode(const std::string& target,
                        ProgressCb progress = {}); // staged boot pipeline
    OpResult haltNode(const std::string& target);   // orderly shutdown: kernel OFF, node PAUSED
    OpResult startNode(const std::string& target,
                       ProgressCb progress = {}); // reboot if kernel dead/off, else repair
    // Simulated command execution (events for success AND failure).
    OpResult runBinary(const std::string& target, const std::string& binary,
                       const std::string& args = "");
    OpResult setPriv(const std::string& target, const std::string& state);
    OpResult ifaceSet(const std::string& node, const std::string& peer, bool up);
    // System-wide failure view (Sec.28): subsystems, faults, recent chain.
    std::string worldHealth() const;

    // --- virtual filesystem (OVERKNRL; host fs is never touched) ---
    const std::string& cwd() const { return cwd_; }
    OpResult setCwd(const std::string& path);
    std::string vfsList(const std::string& path = "") const;
    std::string vfsCat(const std::string& path) const;
    OpResult vfsMkdir(const std::string& path);
    OpResult vfsTouch(const std::string& path);
    OpResult vfsRemove(const std::string& path);
    OpResult vfsCopy(const std::string& src, const std::string& dst);
    OpResult vfsMove(const std::string& src, const std::string& dst);
    OpResult vfsWrite(const std::string& path, const std::string& content);
    OpResult vfsCorrupt(const std::string& path);
    OpResult vfsChmod(const std::string& path, bool add, const std::string& token);
    // --- recovery package loads (validated content only; transactional) ---
    // Restore /boot/kernel from validated package bytes. Quota-checked
    // before any mutation; a stored R blob is rolled back on write failure.
    OpResult loadKernelImage(const std::string& bytes, ProgressCb progress = {});
    // Overlay a validated rootfs baseline onto the live VFS (overlay, not
    // reset: world files outside the baseline are preserved). Missing dirs
    // are recreated; node-owned configs also clear config corruption with
    // standard FAULT_RESOLVED events. All-or-nothing via a rollback journal.
    OpResult loadRootfsBaseline(const RootfsContent& content, ProgressCb progress = {});

    // --- virtual hardware (V/R model; fully simulated, never host access) ---
    std::string hardwareDescribe() const;
    OpResult hardwareBackend(const std::string& resource, const std::string& backend);
    OpResult hardwareOverflow(const std::string& policy);
    // Provisioning entry point (used by tests/scenarios; no shell command).
    OpResult setHardwareProfile(const HardwareProfile& profile);

    // --- network ---
    OpResult connect(const std::string& a, const std::string& b);
    OpResult disconnect(const std::string& a, const std::string& b);
    std::string ping(const std::string& target, const std::string& observer = "") const;
    // Multi-probe ping (4 probes, deterministic loss/avg report).
    std::string pingProbes(const std::string& target, const std::string& observer = "") const;
    PacketResult sendPacket(const std::string& from, const std::string& to) const;
    static std::string formatPacket(const std::string& from, const std::string& to,
                                    const PacketResult& r);

    // --- time ---
    // Advance n ticks (honours pause). Returns ticks actually advanced.
    // `progress` fires per completed tick (genuinely stepwise work).
    int tick(int n, uint64_t causeId = 0, ProgressCb progress = {});
    // Quiet per-command advance used by SIMULATION mode (emits one AUTO_TICK root).
    int autoTick();

    // --- snapshots / time travel ---
    // Semantics: the event ledger is append-only. checkpoint captures the full
    // world state (nodes, links, clock incl. paused flag, seed); restore rewinds
    // the WORLD to the snapshot and appends a RESTORE event -- post-checkpoint
    // events are kept as history, not deleted. rewind(n) restores the nth
    // previous snapshot. branch(label) is a labelled checkpoint (linear history;
    // no snapshot tree). replay(n) only displays the last n events.
    uint64_t checkpoint(const std::string& label = "");
    bool restore(uint64_t snapshotId, uint64_t* causeOut = nullptr,
                 ProgressCb progress = {});
    bool rewindSteps(int stepsBack, uint64_t* causeOut = nullptr,
                     ProgressCb progress = {}); // restore snapshot N back

    // --- observation ---
    std::string status() const;
    std::string inspect(const std::string& target, const std::string& observer = "") const;
    std::string trace(const std::string& target) const; // HOW: chronological timeline
    std::string why(const std::string& target) const;   // WHY: causal chain to root
    std::string predict(const std::string& target) const;
    std::string topology() const;
    // Simulated devices (/dev metadata + what each gates).
    std::string deviceList() const;
    std::string deviceInspect(const std::string& dev) const;
    // Readiness probe: /dev/<dev> must exist and be uncorrupted. Gates real
    // ops: net0 -> iface/packet/ping, disk0 -> stored writes, console -> boot.
    bool deviceReady(const std::string& dev, std::string& whyNot) const;
    // Simulated kernel views (all derived from engine state + the ledger).
    std::string kernelStatus() const;
    std::string kernelInspect(const std::string& target) const;
    std::string uname(const std::string& target) const;
    std::string dmesg(const std::string& target, int limit = 30) const;
    std::string lsmod(const std::string& target) const;
    // Simulated tunables: kernel.instability(ro), thermal.limit,
    // net.base_latency, vm.swap_mb, kernel.modules(ro).
    std::string sysctlGet(const std::string& target, const std::string& key,
                          bool& ok) const;
    OpResult sysctlSet(const std::string& target, const std::string& key,
                       const std::string& value);
    // Fake local machine (simulated only, see host.hpp).
    std::string localInfo() const;
    std::string localPorts() const;
    std::string localInterfaces() const;
    // Resource overview: node CPU/RAM/connections + link congestion.
    std::string resources() const;

    // Effective property of `target` as seen by `observer`
    // (applies deception beliefs). Empty observer = ground truth.
    std::string effectiveProp(const std::string& observer, const std::string& target,
                              const std::string& prop) const;

    // --- helpers used by shell/scenarios ---
    // Appends to the ledger; returns the new event id (never a dangling ref).
    uint64_t emit(uint64_t causeId, const std::string& type, const std::string& source,
                  const std::string& target, const std::string& message,
                  std::map<std::string, std::string> metadata = {},
                  const std::string& severity = "INFO");
    void recordCommand(const std::string& line) { history_.pushCommand(line); }
    bool readOnly() const { return mode_ == "OBSERVE"; }
    // --- operator identity (simulated session user; never host identity) ---
    // opUser names the OVERRIDE account at the keyboard (default "root"),
    // opRoot is the simulated root flag (default true = pre-account sessions
    // behave exactly as before). The shell syncs these from the account
    // store; sudo toggles opRoot. Serialized per world, excluded from
    // digests like all meta (worldId/uptime siblings).
    const std::string& opUser() const { return opUser_; }
    void setOpUser(const std::string& u) { opUser_ = u; }
    bool opRoot() const { return opRoot_; }
    void setOpRoot(bool r) { opRoot_ = r; }

private:
    std::map<std::string, Node> nodes_;
    Network net_;
    Clock clock_;
    EventLog events_;
    History history_;
    std::string mode_ = "NORMAL";
    uint64_t seed_ = 0x9E3779B97F4A7C15ull;
    uint64_t rng_ = 0x9E3779B97F4A7C15ull; // stateful stream for chaos/autonomous faults
    ProcessManager pm_;
    ServiceManager svc_;
    HardwareManager hw_;     // V/R virtual hardware (snapshotted)
    bool autoFaults_ = true; // background entropy: wear, spikes, crashes, recovery
    int autoRate_ = 8;       // base probability % per node per tick
    Vfs vfs_;                // OVERKNRL virtual filesystem (snapshotted)
    std::string cwd_ = "/";  // shell virtual working directory (snapshotted)
    // World identity + lifetime (persisted in .ord; never digested).
    std::string worldId_;
    std::string worldName_ = "unnamed";
    // Operator identity (simulated session user; persisted, never digested).
    std::string opUser_ = "root";
    bool opRoot_ = true;
    std::string worldNote_;
    int64_t worldCreatedAt_ = 0;   // wall seconds at creation
    uint64_t worldCreatedTick_ = 0; // sim tick at creation
    uint64_t worldSavedTick_ = 0;   // sim tick at last save
    uint64_t uptimeTotalSecs_ = 0;  // committed active seconds across sessions
    int64_t uptimeSessionStart_ = 0; // wall seconds; 0 = no active session
    // Observational syscall counters (never snapshotted, never digested).
    mutable std::map<std::string, uint64_t> syscallCounts_;
    void noteSyscall(const std::string& name) const { ++syscallCounts_[name]; }

    // Centralized state transition: applies fixups, validates, and emits exactly
    // one <NAME>_<STATE> event when the state actually changes. Returns the
    // transition event id, or 0 when nothing changed. `to` equal to current
    // state is a no-op (duplicate-event prevention). `faultEvent` optionally
    // links the driving fault's event so causal readers can cross tick
    // boundaries (a tick trigger alone would otherwise hide the fault).
    uint64_t transitionTo(const std::string& name, NodeState to, uint64_t cause,
                          const std::string& reason, const std::string& source = "engine",
                          uint64_t faultEvent = 0);

    // Dependency rule (deterministic, evaluated for every non-sticky node):
    //   any dep FAILED/CORRUPTED or link-down or dep MISSING -> DEGRADED
    //   else any dep DEGRADED/WARNING/RESTARTING/PAUSED      -> DEGRADED
    //   else resource stress (health<40, latency>500, load>92) -> DEGRADED
    //   else ONLINE, unless this node is a healthy provider of a FAILED node
    //   (reverse edge) -> WARNING (upstream back-pressure).
    // Sticky manual states (FAILED/CORRUPTED/PAUSED/RESTARTING) are never
    // overwritten here; only repair/override change them. A node with several
    // deps stays DEGRADED until ALL of them are healthy again.
    NodeState derivedState(const std::string& name, std::string& reason) const;
    void propagate(uint64_t causeId);
    void onTick(uint64_t causeId);
    // Background entropy: wear/spikes/pressure/crashes/recovery, all seeded.
    void autonomousTick(uint64_t causeId);
    // Next value from the stateful seeded stream (snapshot/restored).
    uint64_t nextRand();
    // Pure hash for stateless deterministic rolls (ping probes, packet loss).
    static uint64_t hash64(uint64_t x);
    // Emit ProcessManager outbox notes into the unified ledger.
    void emitProcNotes(const std::vector<ProcNote>& notes, uint64_t cause);
    // Worst active fault severity / name (INFO / "" when no faults).
    static Severity worstFaultSeverity(const Node& n);
    static std::string worstFaultName(const Node& n);
    // If a derived-state reason names one of the node's active faults (the
    // "critical fault: X" / "degraded by fault: X" / "fault warning: X"
    // vocabulary produced by derivedState), return that fault's latest event
    // id so transitions link to their effective cause. 0 when unrelated.
    uint64_t faultEventForReason(const std::string& name, const std::string& reason) const;
    // Per-node per-tick physics: thermal, clock, memory, storage, kernel.
    // Only runs for serving states; deterministic node order.
    void stepPhysics(const std::string& name, uint64_t cause);
    // Service reconciliation each tick + after lifecycle ops.
    void serviceTick(uint64_t cause);
    // Attempt one gated service start (binary/config/rootfs/kernel checks).
    // Emits SERVICE_START or SERVICE_START_FAILED.
    OpResult serviceStartAs(const std::string& name, uint64_t cause, bool resetCrashes);
    void kernelPanic(const std::string& name, uint64_t cause, const std::string& reason);
    // OOM killer: terminate the hungriest process. True if one was killed.
    bool oomKiller(const std::string& name, uint64_t cause);
    // Kind-specific effect of an injected fault (called with USER_INJECT root).
    // Returns false when rejected (an INJECT_REJECTED event was emitted).
    bool applyFaultEffect(const std::string& target, const FaultDef& def, const std::string& value,
                          uint64_t root);
    // Kind-specific cleanup for `recover`/`cool` (called with that root).
    // Returns false + err when the fault cannot be recovered that way.
    bool applyRecovery(const std::string& target, ActiveFault& fault, uint64_t root,
                       std::string& err);
    uint64_t resolveFaultAs(const std::string& target, const std::string& fault,
                            uint64_t userRoot);
    // Deterministic per-(seed,tick,node) jitter in [-8,+8]; pure function.
    int jitter(const std::string& name, uint64_t tick) const;
    static std::string upper(const std::string& s);
    bool setProp(Node& n, const std::string& prop, const std::string& value, std::string& err);
    std::string getProp(const Node& n, const std::string& prop) const;
    // BFS route over UP links avoiding unhealthy transit nodes.
    // Returns empty path when unreachable.
    std::vector<std::string> findRoute(const std::string& from, const std::string& to) const;
    static bool routable(const Node& n);
    // VFS helpers: normalize against cwd; infer node owner from basename stem.
    std::string vfsAbs(const std::string& path) const;
    std::string inferOwner(const std::string& abs) const;
    // Permission check for one VFS entry. Simulated root bypasses; anyone
    // else needs the owner token (or/ow) when operating as the entry owner,
    // else the user token (ur/uw). Denials name the missing token.
    bool vfsAuthorize(const std::string& owner, const std::set<std::string>& acl,
                      bool needWrite, std::string& whyNot) const;
    // Resolve an executable name for `exec`/service start: the node's baked-in
    // set first, then the shared rootfs images (/bin, /sbin: must exist, be
    // executable, and uncorrupted). Returns "" with *err set when unusable.
    std::string resolveBinary(const std::string& node, const std::string& binary,
                              std::string& err) const;
    // Rootfs writability gate for node-owned paths. True when writable.
    bool rootfsWritable(const std::string& owner, std::string& whyNot) const;
    // Storage quota gate for a pending VFS write of `content` to `abs`.
    // Runs the hardware overflow policy (may record overflow in isolated R);
    // returns "" on success or an error. VFS state is untouched on failure.
    std::string hwAccountWrite(const std::string& abs, const std::string& content,
                               std::string& actionOut);
};

} // namespace override
