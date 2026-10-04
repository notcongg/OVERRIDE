#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace override {

// Fault severity ladder. Severity describes how bad a condition is; it is
// recorded on events and drives gradual degradation (WARNING -> DEGRADED ->
// CRITICAL -> FAILED), never an instant unexplained death.
enum class Severity { INFO, WARNING, DEGRADED, CRITICAL, FAILED, PANIC, HALTED };

std::string toString(Severity s);
Severity severityFromString(const std::string& s); // throws on unknown
int severityRank(Severity s);                      // INFO=0 .. HALTED=6

// Fault categories. Commands name concrete faults (e.g. "overheat"); each
// fault maps to one category, and the engine applies per-category effects.
// New faults reuse categories instead of growing a switch statement.
enum class FaultKind {
    THERMAL,   // overheating, throttling, cooling failure
    CLOCK,     // frequency/instability, boost abuse
    POWER,     // power instability
    MEMORY,    // leak, corruption, OOM, allocation failure
    STORAGE,   // disk full, I/O errors
    FILESYSTEM,// fs corruption, read-only, mount failure
    ROOTFS,    // root filesystem failure (full system impact)
    KERNEL,    // panic, instability, subsystem failure
    MODULE,    // kernel module failure (net/disk/sched/...)
    PROCESS,   // crash, runaway, deadlock, restart loop
    SERVICE,   // service down, restart loop, bad service config
    NETWORK,   // loss, congestion, link/iface/route failure
    CONFIG,    // invalid/missing/corrupted configuration
    PERMISSION,// privilege loss, corrupted identity
    COMMAND,   // simulated command execution failure
    RESOURCE,  // generic resource exhaustion/starvation
    DEPENDENCY // missing/failed dependency (derived, rarely injected)
};

std::string toString(FaultKind k);
FaultKind faultKindFromString(const std::string& s); // throws on unknown

// Simulated clock states.
enum class ClockState { STABLE, THROTTLED, UNSTABLE, CRITICAL };
std::string toString(ClockState s);
ClockState clockStateFromString(const std::string& s); // throws on unknown

// Simulated kernel states.
enum class KernelState { RUNNING, WARNING, DEGRADED, UNSTABLE, BOOTING, PANICKED, HALTED, OFF };
std::string toString(KernelState s);
KernelState kernelStateFromString(const std::string& s); // throws on unknown

// Simulated privilege states (OVERSHELL identity is itself simulated).
enum class PrivState { ROOT, USER, LOCKED, RESTRICTED, CORRUPTED };
std::string toString(PrivState s);
PrivState privStateFromString(const std::string& s); // throws on unknown

// Simulated filesystem mount states.
enum class FsState { MOUNTED, DEGRADED, READ_ONLY, CORRUPTED, UNMOUNTED, FAILED };
std::string toString(FsState s);
FsState fsStateFromString(const std::string& s); // throws on unknown

// One stacked condition on a node. Faults coexist (stacking) keyed by name;
// re-applying the same fault escalates severity instead of duplicating it.
struct ActiveFault {
    std::string name;            // e.g. "overheat", "disk-full"
    FaultKind kind = FaultKind::THERMAL;
    Severity severity = Severity::WARNING;
    uint64_t sinceTick = 0;
    uint64_t eventId = 0;        // raising event (causal anchor)
    std::string detail;
};

// Static fault definition: name -> category, default severity, description,
// and whether user recovery via `recover` is meaningful.
struct FaultDef {
    std::string name;
    FaultKind kind;
    Severity severity;
    std::string description;
    // Engine-raised faults (service-down, restart-loop) describe real
    // conditions but cannot be injected directly: `inject` rejects them.
    bool injectable = true;
};

// Lookup by fault name; accepts dashes and underscores. Returns nullptr when
// the name is not a known injectable fault.
const FaultDef* faultDef(const std::string& name);
// All known fault names (for help/tests), sorted.
std::vector<std::string> faultNames();

} // namespace override
