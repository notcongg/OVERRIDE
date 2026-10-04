// System (fault engine: injection, recovery, physics, kernel, chaos). Split from system.cpp; behavior unchanged.
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

OpResult System::injectFault(const std::string& target, const std::string& fault,
                             const std::string& value) {
    if (!hasNode(target)) return OpResult::failure("unknown node: " + target);
    Node& n = get(target);
    uint64_t root = emit(0, "USER_INJECT", "user", n.name,
                         "inject " + n.name + " " + fault + (value.empty() ? "" : " " + value));
    std::string f = fault;
    for (auto& c : f) c = (char)std::tolower((unsigned char)c);
    // Unified fault model first: named faults from the fault table apply
    // kind-specific effects through the engine (never direct field pokes
    // from the shell). Legacy short names below are preserved.
    if (const FaultDef* def = faultDef(f)) {
        if (!def->injectable) {
            emit(root, "INJECT_REJECTED", "engine", n.name,
                 "fault '" + def->name + "' is engine-raised (arises from service failures); "
                 "it cannot be injected directly");
            return OpResult::failure("fault '" + def->name + "' is engine-raised", root);
        }
        if (!applyFaultEffect(n.name, *def, value, root))
            return OpResult::failure("inject " + def->name + " rejected (see events)", root);
        return OpResult::success(root, "injected " + def->name + " into " + n.name);
    }
    std::string errBody;
    // Legacy faults (kept in the engine layer, not the shell).
    if (f == "latency") {
        int ms = 500;
        if (!value.empty()) {
            try {
                ms = parseMs(value);
            } catch (const std::exception& e) {
                errBody = std::string("bad latency: ") + e.what();
            }
        }
        if (errBody.empty() && ms < 0) errBody = "latency cannot be negative";
        if (!errBody.empty()) {
            emit(root, "INJECT_REJECTED", "engine", n.name, errBody);
            return OpResult::failure(errBody, root);
        }
        int before = n.latencyMs;
        n.latencyMs = ms;
        emit(root, "LATENCY_SPIKE", n.name, n.name,
             n.name + " latency " + std::to_string(before) + "ms -> " + std::to_string(ms) +
                 "ms",
             {{"from", std::to_string(before)}, {"to", std::to_string(ms)}});
    } else if (f == "packet_loss" || f == "packet-loss" || f == "loss") {
        double pct = 30.0;
        if (!value.empty()) {
            try {
                size_t pos = 0;
                pct = std::stod(value, &pos);
                if (pos != value.size()) throw std::invalid_argument("trailing text");
            } catch (const std::exception&) {
                emit(root, "INJECT_REJECTED", "engine", n.name,
                     "packet_loss must be a number within [0,100], got '" + value + "'");
                return OpResult::failure("packet_loss must be within [0,100]", root);
            }
        }
        if (pct < 0.0 || pct > 100.0) {
            emit(root, "INJECT_REJECTED", "engine", n.name, "packet_loss out of [0,100]");
            return OpResult::failure("packet_loss must be within [0,100]", root);
        }
        for (auto& l : net_.linksFor(n.name)) net_.setLoss(l.a, l.b, pct);
        emit(root, "PACKET_LOSS", n.name, n.name,
             n.name + " links packet_loss=" + std::to_string(pct) + "%",
             {{"loss", std::to_string(pct)}});
    } else if (f == "corruption" || f == "corrupt") {
        std::string from = toString(n.state);
        transitionTo(n.name, NodeState::CORRUPTED, root, "injected corruption", n.name);
        (void)from;
    } else if (f == "pressure" || f == "memory" || f == "oom" || f == "cpu") {
        n.load = 98;
        n.memoryUsedMb = (int)(n.memoryMb * 0.97);
        emit(root, "RESOURCE_PRESSURE", n.name, n.name,
             n.name + " under resource pressure (load 98%, mem " +
                 std::to_string(n.memoryUsedMb) + "MB)",
             {{"load", "98"}});
    } else if (f == "crash" || f == "kill" || f == "fail") {
        std::string from = toString(n.state);
        transitionTo(n.name, NodeState::FAILED, root, "injected crash", n.name);
        (void)from;
    } else if (f == "pause") {
        transitionTo(n.name, NodeState::PAUSED, root, "injected pause", n.name);
    } else {
        emit(root, "INJECT_REJECTED", "engine", n.name, "unknown fault: " + fault);
        return OpResult::failure("unknown fault: " + fault +
                                 " (latency|packet_loss|corruption|pressure|crash|pause, or "
                                 "`help inject` for the fault catalog)",
                                 root);
    }
    propagate(root);
    return OpResult::success(root, "injected " + f + " into " + n.name);
}

OpResult System::chaosStrike(const std::string& target) {
    std::string tgt = target;
    if (tgt.empty()) {
        auto names = nodeNames();
        if (names.empty()) return OpResult::failure("no nodes to strike");
        tgt = names[splitmix64(rng_) % names.size()]; // sorted order: deterministic
    }
    if (!hasNode(tgt)) return OpResult::failure("unknown node: " + tgt);
    static const char* faults[] = {"latency", "packet_loss", "pressure", "pause"};
    uint64_t r = splitmix64(rng_);
    std::string fault = faults[r % 4];
    std::string value;
    if (fault == std::string("latency")) value = std::to_string(200 + (r / 4) % 800);
    if (fault == std::string("packet_loss")) value = std::to_string(10 + (r / 4) % 40);
    uint64_t root = emit(0, "USER_CHAOS", "user", tgt, "chaos strike " + tgt);
    // Reuse the same fault path as inject (single causal root).
    OpResult applied = injectFault(tgt, fault, value);
    // injectFault emitted its own USER_INJECT root; re-parent note onto chaos root.
    emit(root, "CHAOS_APPLIED", "engine", tgt,
         "chaos strike: " + fault + (value.empty() ? "" : " " + value) + " on " + tgt +
             (applied.ok ? "" : " (rejected: " + applied.error + ")"),
         {{"fault", fault}, {"value", value}});
    return applied.ok ? OpResult::success(root, "chaos strike: " + fault + " on " + tgt)
                      : OpResult::failure(applied.error, root);
}

// ---- fault engine (unified failure model) ----

Severity System::worstFaultSeverity(const Node& n) {
    Severity worst = Severity::INFO;
    for (const auto& [fname, f] : n.faults) {
        if (severityRank(f.severity) > severityRank(worst)) worst = f.severity;
    }
    return worst;
}

std::string System::worstFaultName(const Node& n) {
    Severity worst = Severity::INFO;
    std::string name;
    for (const auto& [fname, f] : n.faults) { // sorted: deterministic pick
        if (severityRank(f.severity) > severityRank(worst)) {
            worst = f.severity;
            name = fname;
        }
    }
    return name;
}

uint64_t System::raiseFault(const std::string& target, const std::string& fault,
                            Severity severity, uint64_t cause, const std::string& detail) {
    Node& n = get(target);
    auto it = n.faults.find(fault);
    if (it != n.faults.end()) {
        // Stacking: same fault re-raised escalates instead of duplicating.
        if (severityRank(severity) <= severityRank(it->second.severity)) return it->second.eventId;
        Severity from = it->second.severity;
        uint64_t priorId = it->second.eventId;
        it->second.severity = severity;
        it->second.eventId =
            emit(cause, "FAULT_ESCALATED", target, target,
                 target + " fault '" + fault + "' escalated " + toString(from) + " -> " +
                     toString(severity) + (detail.empty() ? "" : " (" + detail + ")"),
                 {{"fault", fault},
                  {"from", toString(from)},
                  {"to", toString(severity)},
                  {"prior", std::to_string(priorId)},
                  {"kind", toString(faultDef(fault) ? faultDef(fault)->kind : FaultKind::DEPENDENCY)}},
                 toString(severity));
        propagate(cause);
        return it->second.eventId;
    }
    ActiveFault af;
    af.name = fault;
    const FaultDef* def = faultDef(fault);
    af.kind = def ? def->kind : FaultKind::DEPENDENCY;
    af.severity = severity;
    af.sinceTick = clock_.tickCount();
    af.detail = detail;
    af.eventId = emit(cause, "FAULT_RAISED", target, target,
                      target + " fault '" + fault + "' raised (" + toString(severity) + ")" +
                          (detail.empty() ? "" : ": " + detail),
                      {{"fault", fault},
                       {"severity", toString(severity)},
                       {"kind", toString(af.kind)}},
                      toString(severity));
    n.faults[fault] = af;
    propagate(cause);
    return af.eventId;
}

bool System::applyRecovery(const std::string& target, ActiveFault& fault, uint64_t root,
                           std::string& err) {
    Node& n = get(target);
    const std::string& f = fault.name;
    const FaultDef* def = faultDef(f);
    FaultKind kind = def ? def->kind : fault.kind;
    switch (kind) {
        case FaultKind::THERMAL:
            if (f == "cooling-fail") n.ambientC = 25;
            n.tempC = std::min(n.tempC, n.ambientC + 10);
            if (n.throttled && n.tempC < n.thermalLimitC - 10) {
                n.throttled = false;
                n.freqMHz = n.baseFreqMHz;
                if (n.clockState == ClockState::THROTTLED) n.clockState = ClockState::STABLE;
            }
            return true;
        case FaultKind::CLOCK:
        case FaultKind::POWER:
            n.freqMHz = n.baseFreqMHz;
            if (n.clockState == ClockState::THROTTLED || n.clockState == ClockState::UNSTABLE)
                n.clockState = ClockState::STABLE;
            return true;
        case FaultKind::MEMORY:
            if (f == "mem-leak") n.leakMbPerTick = 0;
            if (f == "mem-corrupt") n.memCorrupt = false;
            return true; // oom/alloc-fail leave no residue
        case FaultKind::STORAGE:
            if (f == "disk-full") {
                n.storageUsedMb = n.storageMb * 70 / 100; // resource release
                if (n.filesystems["rootfs"] == FsState::READ_ONLY &&
                    n.faults.count("rootfs-failure") == 0)
                    n.filesystems["rootfs"] = FsState::MOUNTED;
            }
            if (f == "io-error") n.ioLoad = std::min(n.ioLoad, 20);
            return true;
        case FaultKind::FILESYSTEM: {
            if (f == "rootfs-failure" || f == "rootfs-readonly") {
                n.storageUsedMb = std::min(n.storageUsedMb, n.storageMb * 80 / 100);
                n.filesystems["rootfs"] = FsState::MOUNTED;
                return true;
            }
            // Two-step fsck-like recovery: CORRUPTED -> DEGRADED -> MOUNTED.
            std::string mount = "data";
            if (n.filesystems.count("rootfs") && n.filesystems["rootfs"] == FsState::CORRUPTED)
                mount = "rootfs";
            else {
                for (const auto& [m, st] : n.filesystems) {
                    if (st == FsState::CORRUPTED) {
                        mount = m;
                        break;
                    }
                }
            }
            FsState& st = n.filesystems[mount]; // creates MOUNTED if absent; corrected below
            if (st == FsState::CORRUPTED) {
                st = FsState::DEGRADED;
                err = ""; // recovered one step; fault stays until MOUNTED
                emit(root, "FS_REPAIR_STEP", "engine", target,
                     target + " " + mount + " CORRUPTED -> DEGRADED (recover again)");
                return false; // keep the fault: needs a second pass
            }
            st = FsState::MOUNTED;
            return true;
        }
        case FaultKind::KERNEL:
            if (f == "kernel-panic") {
                err = "kernel panic cannot be recovered by `recover` (reboot required)";
                return false;
            }
            if (f == "kernel-unstable") {
                n.kernel = KernelState::RUNNING;
                n.instability = std::min(n.instability, 20);
                return true;
            }
            // module-fail: module name is the fault detail "module=<name>"
            for (auto& [m, st] : n.modules) {
                if (st == "FAILED") st = "LOADED";
            }
            return true;
        case FaultKind::PROCESS:
            return true; // dead processes stay dead; fault record clears
        case FaultKind::NETWORK:
            if (f == "link-failure" || f == "iface-down") {
                // peer recorded in detail as "peer=<name>"
                std::string peer;
                auto pos = fault.detail.find("peer=");
                if (pos != std::string::npos) peer = fault.detail.substr(pos + 5);
                if (!peer.empty() && net_.find(target, peer)) net_.setUp(target, peer, true);
                return true;
            }
            if (f == "net-congest") {
                for (auto& l : net_.linksFor(target)) {
                    if (Link* m = net_.find(l.a, l.b))
                        m->loadPct = std::max(0.0, m->loadPct - 30.0);
                }
                return true;
            }
            if (f == "conn-exhaust") {
                n.connections = n.maxConnections / 2;
                return true;
            }
            return true;
        case FaultKind::CONFIG:
            n.configCorrupt = false;
            return true;
        case FaultKind::PERMISSION:
            n.priv = PrivState::ROOT;
            return true;
        case FaultKind::SERVICE:
            return true; // services recover via start/restart, not `recover`
        case FaultKind::RESOURCE:
        case FaultKind::COMMAND:
        case FaultKind::DEPENDENCY:
            return true;
        case FaultKind::ROOTFS:
            // Root damage needs remount + space: same path as rootfs-failure.
            n.storageUsedMb = std::min(n.storageUsedMb, n.storageMb * 80 / 100);
            if (n.filesystems["rootfs"] == FsState::CORRUPTED)
                n.filesystems["rootfs"] = FsState::DEGRADED;
            else
                n.filesystems["rootfs"] = FsState::MOUNTED;
            return true;
        case FaultKind::MODULE:
            for (auto& [m, st] : n.modules) {
                if (st == "FAILED") st = "LOADED";
            }
            return true;
    }
    return true;
}

uint64_t System::resolveFaultAs(const std::string& target, const std::string& fault,
                                uint64_t userRoot) {
    Node& n = get(target);
    auto it = n.faults.find(fault);
    if (it == n.faults.end()) return 0;
    uint64_t root = userRoot;
    if (root == 0)
        root = emit(0, "USER_RECOVER", "user", target, "recover " + target + " " + fault);
    std::string err;
    ActiveFault copy = it->second;
    if (!applyRecovery(target, copy, root, err)) {
        if (!err.empty()) emit(root, "RECOVER_REJECTED", "engine", target, err);
        return 0; // fault stays (partial step already emitted inside)
    }
    n.faults.erase(it);
    return emit(root, "FAULT_RESOLVED", "engine", target,
                target + " fault '" + fault + "' resolved", {{"fault", fault}});
}

OpResult System::resolveFault(const std::string& target, const std::string& fault) {
    if (!hasNode(target)) return OpResult::failure("unknown node: " + target);
    if (get(target).faults.count(fault) == 0)
        return OpResult::failure("no active fault '" + fault + "' on " + target +
                                 " (see inspect faults)");
    // kernel-config clears only once the substrate file itself is repaired:
    // acknowledging the fault must not magically fix the disk.
    if (fault == "kernel-config") {
        const VFile* kc = vfs_.file("/kernel/kernel.conf");
        if (!kc || kc->corrupted)
            return OpResult::failure("cannot recover 'kernel-config': repair "
                                     "/kernel/kernel.conf first (remove + rewrite it)");
    }
    uint64_t root = emit(0, "USER_RECOVER", "user", target, "recover " + target + " " + fault);
    uint64_t id = resolveFaultAs(target, fault, root);
    if (id == 0) {
        const Node& n = get(target);
        auto it = n.faults.find(fault);
        if (it != n.faults.end())
            return OpResult::success(root, target + " fault '" + fault +
                                                "' partially recovered (repeat to finish)");
        return OpResult::failure("cannot recover '" + fault + "' that way", root);
    }
    propagate(root);
    return OpResult::success(root, target + " fault '" + fault + "' resolved");
}

OpResult System::coolNode(const std::string& target) {
    if (!hasNode(target)) return OpResult::failure("unknown node: " + target);
    Node& n = get(target);
    uint64_t root = emit(0, "USER_COOL", "user", target, "cool " + target);
    n.tempC = n.ambientC + 10;
    std::string info = target + " cooled to " + std::to_string(n.tempC) + "C";
    // Cooling only touches thermal state: other faults survive (by design).
    for (auto it = n.faults.begin(); it != n.faults.end();) {
        const FaultDef* def = faultDef(it->first);
        FaultKind kind = def ? def->kind : it->second.kind;
        if (kind == FaultKind::THERMAL) {
            std::string fname = it->first;
            ++it; // resolveFaultAs logic inlined via public path below is awkward;
            // erase manually after shared cleanup:
            ActiveFault copy = get(target).faults[fname];
            std::string err;
            applyRecovery(target, copy, root, err);
            get(target).faults.erase(fname);
            emit(root, "FAULT_RESOLVED", "engine", target,
                 target + " fault '" + fname + "' resolved (cooling)");
            info += "; thermal fault '" + fname + "' cleared";
        } else {
            ++it;
        }
    }
    if (n.throttled && n.tempC < n.thermalLimitC - 10) {
        n.throttled = false;
        n.freqMHz = n.baseFreqMHz;
        if (n.clockState == ClockState::THROTTLED) n.clockState = ClockState::STABLE;
        info += "; unthrottled";
    }
    propagate(root);
    return OpResult::success(root, info);
}

OpResult System::rebootNode(const std::string& target, ProgressCb progress) {
    if (!hasNode(target)) return OpResult::failure("unknown node: " + target);
    Node& n = get(target);
    uint64_t root = emit(0, "USER_REBOOT", "user", target, "reboot " + target);
    noteSyscall("reboot");
    // Staged boot pipeline: each phase emits BOOT_PHASE and runs its real
    // prerequisite check against the shared substrate. A broken prerequisite
    // stops the boot with BOOT_FAILED + a boot-failure fault; the node stays
    // FAILED (kernel stuck BOOTING) until the substrate is repaired. Phases
    // double as genuine progress units for the shell progress bar.
    static const char* phases[] = {"PREPARE",        "BOOTLOADER",  "KERNEL_LOAD",
                                  "DRIVER_INIT",    "FS_MOUNT",    "DEVICE_INIT",
                                  "IPC_INIT",       "SCHEDULER_INIT", "NETWORK_INIT",
                                  "SVC_MANAGER_INIT", "INIT",      "RUNNING"};
    static const int nPhases = 12;
    auto runPhase = [&](int done, const char* name, const std::string& detail) {
        std::map<std::string, std::string> meta{{"phase", name}};
        emit(root, "BOOT_PHASE", target, target, target + " boot [" + name + "]" +
                 (detail.empty() ? "" : ": " + detail), meta, "INFO");
        if (progress) progress(done, nPhases);
    };
    auto bootFailed = [&](const char* at, const std::string& detail) {
        emit(root, "BOOT_FAILED", target, target,
             target + " boot failed at " + at + ": " + detail, {{"phase", at}}, "FAILED");
        uint64_t fe = raiseFault(target, "boot-failure", Severity::CRITICAL, root, detail);
        transitionTo(target, NodeState::FAILED, root,
                     "boot failed at " + std::string(at) + ": " + detail, target, fe);
        propagate(root);
        return OpResult::failure("boot failed at " + std::string(at) + ": " + detail, root);
    };
    // PREPARE: orderly pre-boot. Fixes the running machine, not disks,
    // configs, or leaks: thermal/clock/kernel/power faults clear (including a
    // previous boot-failure); storage/fs/config/perm persist.
    std::string prevKernel = toString(n.kernel);
    n.kernel = KernelState::BOOTING;
    emit(root, "KERNEL_BOOT", target, target, target + " kernel BOOTING",
         {{"from", prevKernel}}, "INFO");
    n.instability = 0;
    n.tempC = n.ambientC + 15;
    n.throttled = false;
    n.freqMHz = n.baseFreqMHz;
    n.clockState = ClockState::STABLE;
    n.memCorrupt = false;
    for (auto it = n.faults.begin(); it != n.faults.end();) {
        const FaultDef* def = faultDef(it->first);
        FaultKind kind = def ? def->kind : it->second.kind;
        if (kind == FaultKind::THERMAL || kind == FaultKind::CLOCK ||
            kind == FaultKind::POWER || kind == FaultKind::KERNEL) {
            emit(root, "FAULT_RESOLVED", "engine", target,
                 target + " fault '" + it->first + "' cleared by reboot");
            it = n.faults.erase(it);
        } else {
            ++it;
        }
    }
    transitionTo(target, NodeState::RESTARTING, root, "reboot initiated", target);
    runPhase(1, phases[0], "volatile thermal/clock state cleared");
    // BOOTLOADER: kernel configuration must be intact.
    {
        const VFile* kc = vfs_.file("/kernel/kernel.conf");
        if (!kc) return bootFailed(phases[1], "invalid kernel configuration (/kernel/kernel.conf missing)");
        if (kc->corrupted)
            return bootFailed(phases[1], "invalid kernel configuration (/kernel/kernel.conf corrupted)");
        runPhase(2, phases[1], "kernel.conf ok");
    }
    // KERNEL_LOAD: the shared image must exist with valid integrity.
    {
        const VFile* ki = vfs_.file("/boot/kernel");
        if (!ki) return bootFailed(phases[2], "kernel image missing (/boot/kernel not found)");
        if (ki->corrupted)
            return bootFailed(phases[2], "kernel image invalid (/boot/kernel corrupted: integrity invalid)");
        runPhase(3, phases[2], "image verified");
    }
    // DRIVER_INIT: every expected module must load from /lib/modules. FAILED
    // modules heal when their image is present; UNLOADED (admin-disabled)
    // modules are skipped and stay that way.
    {
        std::string loaded, skipped;
        for (auto& [m, st] : n.modules) {
            std::string img = "/lib/modules/" + m + ".ko";
            const VFile* f = vfs_.file(img);
            if (st == "UNLOADED") {
                if (!skipped.empty()) skipped += ",";
                skipped += m;
                continue;
            }
            if (!f)
                return bootFailed(phases[3], "module '" + m + "' unavailable (" + img +
                                               " missing)");
            if (f->corrupted)
                return bootFailed(phases[3], "module '" + m + "' unavailable (" + img +
                                               " corrupted)");
            if (st == "FAILED") st = "LOADED"; // reprobe heals with a good image
            if (!loaded.empty()) loaded += ",";
            loaded += m;
        }
        std::string detail = "modules live: " + (loaded.empty() ? "(none)" : loaded);
        if (!skipped.empty()) detail += "; skipped (admin-disabled): " + skipped;
        runPhase(4, phases[3], detail);
    }
    // FS_MOUNT: rootfs must be mountable.
    {
        auto it = n.filesystems.find("rootfs");
        FsState st = (it == n.filesystems.end()) ? FsState::MOUNTED : it->second;
        if (st != FsState::MOUNTED && st != FsState::DEGRADED)
            return bootFailed(phases[4], "rootfs " + toString(st) + " (filesystem unavailable)");
        runPhase(5, phases[4], "rootfs " + toString(st));
    }
    // DEVICE_INIT: boot console must exist.
    {
        std::string devWhy;
        if (!deviceReady("console", devWhy))
            return bootFailed(phases[5], "console device unavailable (" + devWhy + ")");
        runPhase(6, phases[5], "console ready");
    }
    // IPC_INIT / SCHEDULER_INIT: derived subsystems, reported with live counts.
    {
        int live = 0;
        for (const auto& [pid, p] : pm_.all())
            if (p.host == target &&
                (p.state == ProcState::RUNNING || p.state == ProcState::SLEEPING)) ++live;
        runPhase(7, phases[6], std::to_string(live) + " live endpoint(s) addressable");
        runPhase(8, phases[7], std::to_string(live) + " runnable process(es) queued");
    }
    // NETWORK_INIT: report fabric state; a disabled net module stays degraded.
    {
        int up = 0, total = 0;
        for (const auto& l : net_.linksFor(target)) {
            ++total;
            if (l.up) ++up;
        }
        std::string detail =
            std::to_string(up) + "/" + std::to_string(total) + " links up";
        if (n.modules.count("net") && n.modules.at("net") == "UNLOADED")
            detail += " (net module UNLOADED: degraded by admin intent)";
        runPhase(9, phases[8], detail);
    }
    // SVC_MANAGER_INIT: ALWAYS services (re)start; crash counts preserved.
    {
        int started = 0;
        for (const auto& [sname, s] : svc_.all()) {
            if (s.node == target && s.policy == ServicePolicy::ALWAYS &&
                s.state != ServiceState::RUNNING) {
                OpResult r = serviceStartAs(sname, root, false);
                if (r.ok) ++started;
            }
        }
        runPhase(10, phases[9], std::to_string(started) + " service(s) (re)started");
    }
    // INIT: an init image must exist and be runnable. /sbin/init wins;
    // /bin/init is the documented fallback. Only losing both stops boot.
    {
        const VFile* init = vfs_.file("/sbin/init");
        std::string used = "/sbin/init";
        if (!init) {
            init = vfs_.file("/bin/init");
            used = "/bin/init";
        }
        if (!init)
            return bootFailed(phases[10], "init missing (no /sbin/init, no /bin/init)");
        if (init->corrupted)
            return bootFailed(phases[10], "init invalid (" + used + " corrupted)");
        if (!init->executable)
            return bootFailed(phases[10], "init not executable (" + used + ")");
        runPhase(11, phases[10], used + " running");
    }
    // RUNNING: reconstruct runtime state, revive host processes, come online.
    n.kernel = KernelState::RUNNING;
    emit(root, "KERNEL_UP", target, target, target + " kernel RUNNING",
         {{"from", "BOOTING"}}, "INFO");
    n.health = 90 + (int)(clock_.tickCount() % 8);
    n.latencyMs = n.baseLatencyMs;
    n.load = 20;
    n.memoryUsedMb = n.memoryMb / 4;
    n.swapUsedMb = 0;
    transitionTo(target, NodeState::ONLINE, root, "reboot complete", target);
    {
        std::vector<ProcNote> notes;
        pm_.restartHost(target, notes);
        emitProcNotes(notes, root);
    }
    runPhase(12, phases[11], "node ONLINE");
    propagate(root);
    std::string info = target + " rebooted";
    if (!n.faults.empty()) info += " (" + std::to_string(n.faults.size()) + " fault(s) persist)";
    return OpResult::success(root, info);
}

OpResult System::haltNode(const std::string& target) {
    if (!hasNode(target)) return OpResult::failure("unknown node: " + target);
    Node& n = get(target);
    if (n.kernel == KernelState::OFF)
        return OpResult::failure(target + " is already OFF");
    uint64_t root = emit(0, "USER_HALT", "user", target, "halt " + target);
    // Orderly shutdown: processes stop (resumable), kernel powers off.
    // Unlike break/panic this is not a failure: node goes PAUSED, and no
    // FAILED-derived degradation propagates (dependents see PAUSED).
    {
        std::vector<ProcNote> notes;
        pm_.stopHost(target, notes);
        emitProcNotes(notes, root);
    }
    for (const auto& sname : svc_.stopHost(target, clock_.tickCount()))
        emit(root, "SERVICE_STOPPED", target, target,
             "service " + sname + " STOPPED: host " + target + " halted",
             {{"service", sname}});
    n.kernel = KernelState::OFF;
    emit(root, "KERNEL_OFF", target, target, target + " kernel OFF (orderly halt)", {},
         "INFO");
    transitionTo(target, NodeState::PAUSED, root, "orderly halt", target);
    propagate(root);
    return OpResult::success(root, target + " halted (kernel OFF, node PAUSED)");
}

OpResult System::startNode(const std::string& target, ProgressCb progress) {
    if (!hasNode(target)) return OpResult::failure("unknown node: " + target);
    const Node& n = get(target);
    if (n.kernel == KernelState::PANICKED || n.kernel == KernelState::HALTED ||
        n.kernel == KernelState::OFF)
        return rebootNode(target, progress); // only a reboot (re)boots a dead/off kernel
    if (n.state == NodeState::ONLINE) {
        uint64_t id = emit(0, "USER_START", "user", target, target + " already running");
        return OpResult::success(id, target + " already ONLINE");
    }
    return repairNode(target);
}

OpResult System::setPriv(const std::string& target, const std::string& state) {
    if (!hasNode(target)) return OpResult::failure("unknown node: " + target);
    PrivState ps;
    try {
        ps = privStateFromString(upper(state));
    } catch (const std::exception&) {
        return OpResult::failure("unknown privilege state '" + state +
                                 "' (ROOT|USER|LOCKED|RESTRICTED|CORRUPTED)");
    }
    Node& n = get(target);
    uint64_t root = emit(0, "USER_PRIV", "user", target,
                         "priv " + target + " " + toString(n.priv) + " -> " + toString(ps));
    n.priv = ps;
    emit(root, "PRIV_CHANGED", "engine", target,
         target + " privileges now " + toString(ps));
    propagate(root);
    return OpResult::success(root, target + " privileges: " + toString(ps));
}

OpResult System::ifaceSet(const std::string& node, const std::string& peer, bool up) {
    if (!hasNode(node)) return OpResult::failure("unknown node: " + node);
    const Link* l = net_.find(node, peer);
    if (!l) return OpResult::failure("no interface " + node + " <-> " + peer + " (no such link)");
    uint64_t root = emit(0, "USER_IFACE", "user", node,
                         "iface " + node + " " + peer + (up ? " up" : " down"));
    std::string devWhy;
    if (!deviceReady("net0", devWhy)) {
        emit(root, "DEVICE_UNAVAILABLE", "kernel", node,
             "iface " + node + " " + peer + " refused: device /dev/net0 unavailable (" +
                 devWhy + ")",
             {{"device", "net0"}}, "WARNING");
        return OpResult::failure("device /dev/net0 unavailable (" + devWhy + ")", root);
    }
    if ((l->up && up) || (!l->up && !up)) {
        emit(root, "IFACE_NOCHANGE", "network", node,
             "interface " + node + " <-> " + peer + " already " + (up ? "up" : "down"));
        return OpResult::failure("interface already " + std::string(up ? "up" : "down"), root);
    }
    net_.setUp(node, peer, up);
    emit(root, up ? "IFACE_UP" : "IFACE_DOWN", "network", node,
         "interface " + node + " <-> " + peer + (up ? " up" : " down"), {{"peer", peer}},
         up ? "INFO" : "CRITICAL");
    propagate(root);
    return OpResult::success(root,
                             "interface " + node + " <-> " + peer + (up ? " up" : " down"));
}

OpResult System::runBinary(const std::string& target, const std::string& binary,
                           const std::string& args) {
    if (!hasNode(target)) return OpResult::failure("unknown node: " + target);
    Node& n = get(target);
    uint64_t root = emit(0, "USER_RUN", "user", target,
                         "exec " + target + " " + binary + (args.empty() ? "" : " " + args));
    auto fail = [&](const std::string& type, const std::string& reason, const std::string& sev) {
        emit(root, type, "kernel", target,
             "exec " + binary + " on " + target + " failed: " + reason,
             {{"binary", binary}, {"reason", reason}}, sev);
        return OpResult::failure("exec failed: " + reason + " (see events)", root);
    };
    if (n.kernel == KernelState::PANICKED || n.kernel == KernelState::HALTED)
        return fail("CMD_KERNEL_DOWN", "kernel " + toString(n.kernel), "FAILED");
    if (n.state == NodeState::FAILED || n.state == NodeState::CORRUPTED)
        return fail("CMD_HOST_DOWN", "host " + toString(n.state), "FAILED");
    if (n.state == NodeState::PAUSED)
        return fail("CMD_HOST_PAUSED", "host paused", "WARNING");
    static const std::set<std::string> privBins = {"rebootd", "fsck", "installer", "mkfs"};
    if (privBins.count(binary) && n.priv != PrivState::ROOT)
        return fail("CMD_PERM_DENIED", "privileges " + toString(n.priv) + " (need ROOT)", "WARNING");
    std::string binErr;
    std::string binPath = resolveBinary(target, binary, binErr);
    if (binPath.empty()) return fail("CMD_NOENT", binErr, "WARNING");
    if (!binPath.empty() && binPath[0] == '/') {
        const VFile* bf = vfs_.file(binPath);
        std::string why;
        if (bf && !vfsAuthorize(bf->owner, bf->acl, false, why))
            return fail("CMD_PERM_DENIED", "permission denied (" + why + " on " + binPath + ")",
                        "WARNING");
    }
    auto rfit = n.filesystems.find("rootfs");
    FsState rootfs = (rfit == n.filesystems.end()) ? FsState::MOUNTED : rfit->second;
    if (rootfs == FsState::UNMOUNTED || rootfs == FsState::CORRUPTED || rootfs == FsState::FAILED)
        return fail("CMD_FS_UNAVAILABLE", "rootfs " + toString(rootfs), "DEGRADED");
    static const std::set<std::string> writeBins = {"db-write", "logger", "installer"};
    if (rootfs == FsState::READ_ONLY && writeBins.count(binary))
        return fail("CMD_READONLY_FS", "rootfs read-only, cannot write", "WARNING");
    if (n.faults.count("alloc-fail") && binary != "shell" && binary != "ping")
        return fail("CMD_ALLOC_FAILED", "memory allocation failed", "WARNING");
    n.load = std::min(100, n.load + 2);
    emit(root, "CMD_OK", n.name, n.name, "exec " + binary + " on " + target + ": exit 0",
         {{"binary", binary}, {"exit", "0"}});
    noteSyscall("execve");
    propagate(root);
    return OpResult::success(root, binary + " on " + target + ": exit 0");
}

std::string System::worldHealth() const {
    std::ostringstream o;
    o << "WORLD HEALTH\n";
    o << "------------------------------\n";
    auto worst = [](const std::vector<std::string>& vals) {
        // order by severity rank of subsystem state words
        int rank = -1;
        std::string cur = "ONLINE";
        for (const auto& v : vals) {
            int r = 0;
            if (v == "WARNING") r = 1;
            else if (v == "DEGRADED" || v == "UNSTABLE") r = 2;
            else if (v == "CRITICAL") r = 3;
            else if (v == "FAILED" || v == "PANICKED" || v == "HALTED" || v == "CORRUPTED") r = 4;
            if (r > rank) {
                rank = r;
                cur = v;
            }
        }
        return cur;
    };
    std::vector<std::string> kern, cpu, mem, stor, netw;
    int running = 0, crashed = 0;
    for (const auto& [name, n] : nodes_) {
        kern.push_back(toString(n.kernel));
        cpu.push_back(n.load > 92 ? "CRITICAL" : (n.load > 70 ? "WARNING" : "ONLINE"));
        int memPct = n.memoryUsedMb * 100 / std::max(1, n.memoryMb);
        mem.push_back(memPct > 90 ? "CRITICAL" : (memPct > 70 ? "WARNING" : "ONLINE"));
        int storPct = n.storageUsedMb * 100 / std::max(1, n.storageMb);
        stor.push_back(storPct >= 100 ? "FAILED" : (storPct > 85 ? "WARNING" : "ONLINE"));
    }
    for (const auto& l : net_.links()) {
        if (!l.up) netw.push_back("FAILED");
        else if (l.lossPct > 20.0 || l.loadPct > 80.0) netw.push_back("DEGRADED");
    }
    if (netw.empty()) netw.push_back("ONLINE");
    for (const auto& [pid, p] : pm_.all()) {
        if (p.state == ProcState::RUNNING || p.state == ProcState::SLEEPING) ++running;
        else if (p.state == ProcState::CRASHED) ++crashed;
    }
    o << "kernel      " << worst(kern) << "\n";
    o << "cpu         " << worst(cpu) << "\n";
    o << "memory      " << worst(mem) << "\n";
    o << "storage     " << worst(stor) << "\n";
    o << "network     " << worst(netw) << "\n";
    o << "processes   " << running << " running / " << crashed << " crashed\n";
    o << "\nACTIVE FAULTS\n";
    o << "------------------------------\n";
    int shown = 0;
    for (const auto& [name, n] : nodes_) {
        for (const auto& [fname, f] : n.faults) {
            if (shown >= 12) break;
            o << name << "  " << fname << " [" << toString(f.severity) << "]\n";
            ++shown;
        }
    }
    if (shown == 0) o << "(none)\n";
    o << "\nRECENT CAUSAL CHAIN\n";
    o << "------------------------------\n";
    const Event* leaf = nullptr;
    for (auto it = events_.all().rbegin(); it != events_.all().rend(); ++it) {
        if (it->causeId != 0) {
            leaf = &(*it);
            break;
        }
    }
    if (!leaf) {
        o << "(no causal chain yet)\n";
    } else {
        auto chain = events_.causeChain(leaf->id);
        int depth = 0;
        for (int i = (int)chain.size() - 1; i >= 0 && depth < 6; --i, ++depth) {
            o << std::string((size_t)depth * 2, ' ') << chain[(size_t)i]->type << " #" 
              << chain[(size_t)i]->id;
            if (depth == 0) o << "  (" << chain[(size_t)i]->message << ")";
            o << "\n";
        }
    }
    return o.str();
}

bool System::applyFaultEffect(const std::string& target, const FaultDef& def,
                               const std::string& value, uint64_t root) {
    Node& n = get(target);
    int iv = 0;
    bool hasNum = parseInt(value, iv);
    switch (def.kind) {
        case FaultKind::THERMAL:
            if (def.name == "cooling-fail") {
                n.ambientC = std::min(80, n.ambientC + 15);
                raiseFault(target, def.name, def.severity, root, "cooling degraded");
            } else { // overheat
                int delta = hasNum ? iv : 0;
                n.tempC = std::min(250, n.thermalLimitC + 5 + delta);
                raiseFault(target, def.name, def.severity, root,
                           "temp forced to " + std::to_string(n.tempC) + "C");
            }
            break;
        case FaultKind::CLOCK:
            if (def.name == "clock-boost") {
                int boost = hasNum ? iv : 400;
                n.freqMHz = std::min(12000, n.maxFreqMHz + boost);
                raiseFault(target, def.name, def.severity, root,
                           "freq forced to " + std::to_string(n.freqMHz) + "MHz");
            } else {
                n.clockState = ClockState::UNSTABLE;
                n.instability = std::min(100, n.instability + 30);
                raiseFault(target, def.name, def.severity, root, "clock jitter injected");
            }
            break;
        case FaultKind::POWER:
            n.clockState = ClockState::UNSTABLE;
            n.instability = std::max(n.instability, 60);
            raiseFault(target, def.name, def.severity, root, "power rail unstable");
            break;
        case FaultKind::MEMORY:
            if (def.name == "mem-leak") {
                n.leakMbPerTick = hasNum ? std::max(0, iv) : 20;
                raiseFault(target, def.name, def.severity, root,
                           std::to_string(n.leakMbPerTick) + "MB/tick");
            } else if (def.name == "mem-corrupt") {
                n.memCorrupt = true;
                raiseFault(target, def.name, def.severity, root, "pages corrupted");
            } else if (def.name == "oom") {
                n.memoryUsedMb = n.memoryMb;
                n.swapUsedMb = n.swapMb;
                oomKiller(target, root);
                emit(root, "OOM", n.name, n.name,
                     n.name + " out of memory (cause: injected oom)",
                     {{"cause-detail", "oom-inject"}}, "CRITICAL");
            } else { // alloc-fail
                raiseFault(target, def.name, def.severity, root, "allocator failing");
            }
            break;
        case FaultKind::STORAGE:
            if (def.name == "disk-full") {
                int pct = hasNum ? std::clamp(iv, 50, 100) : 100;
                n.storageUsedMb = n.storageMb * pct / 100;
                raiseFault(target, def.name, def.severity, root,
                           "storage at " + std::to_string(pct) + "%");
            } else { // io-error
                n.ioLoad = 95;
                raiseFault(target, def.name, def.severity, root, "I/O timing out");
            }
            break;
        case FaultKind::FILESYSTEM:
            if (def.name == "rootfs-failure") {
                n.storageUsedMb = n.storageMb;
                n.filesystems["rootfs"] = FsState::READ_ONLY;
                raiseFault(target, def.name, def.severity, root, "rootfs full, remounted ro");
            } else if (def.name == "rootfs-readonly") {
                n.filesystems["rootfs"] = FsState::READ_ONLY;
                raiseFault(target, def.name, def.severity, root, "root remounted read-only");
            } else if (def.name == "mount-fail") {
                std::string mount = value.empty() ? "data" : value;
                if (n.filesystems.count(mount) == 0) {
                    emit(root, "INJECT_REJECTED", "engine", n.name,
                         "unknown mount '" + mount + "' (rootfs|data|tmp|cache)");
                    return false;
                }
                n.filesystems[mount] = FsState::UNMOUNTED;
                raiseFault(target, def.name, def.severity, root, mount + " unmounted");
            } else { // fs-corrupt
                std::string mount = value.empty() ? "data" : value;
                if (n.filesystems.count(mount) == 0) {
                    emit(root, "INJECT_REJECTED", "engine", n.name,
                         "unknown mount '" + mount + "' (rootfs|data|tmp|cache)");
                    return false;
                }
                n.filesystems[mount] = FsState::CORRUPTED;
                raiseFault(target, def.name, def.severity, root, mount + " corrupted");
            }
            break;
        case FaultKind::KERNEL:
            if (def.name == "kernel-panic") {
                kernelPanic(target, root, "injected panic");
            } else if (def.name == "kernel-unstable") {
                n.kernel = KernelState::UNSTABLE;
                n.instability = std::max(n.instability, 50);
                raiseFault(target, def.name, def.severity, root, "kernel wobbling");
            } else { // module-fail
                std::string mod = value.empty() ? "net" : value;
                if (n.modules.count(mod) == 0) {
                    emit(root, "INJECT_REJECTED", "engine", n.name,
                         "unknown module '" + mod + "' (sched|net|disk)");
                    return false;
                }
                n.modules[mod] = "FAILED";
                if (mod == "net") {
                    for (auto& l : net_.linksFor(target)) {
                        if (Link* m = net_.find(l.a, l.b))
                            m->lossPct = std::min(100.0, m->lossPct + 25.0);
                    }
                } else if (mod == "disk") {
                    n.ioLoad = 100;
                } else if (mod == "sched") {
                    n.load = 99;
                }
                raiseFault(target, def.name, Severity::WARNING, root,
                           "module " + mod + " FAILED");
            }
            break;
        case FaultKind::MODULE: {
            // Admin load/unload (no fault record): real subsystem state change.
            std::string mod = value.empty() ? "net" : value;
            if (n.modules.count(mod) == 0) {
                emit(root, "INJECT_REJECTED", "engine", n.name,
                     "unknown module '" + mod + "' (sched|net|disk)");
                return false;
            }
            if (def.name == "module-unload") {
                if (n.modules[mod] == "UNLOADED") {
                    emit(root, "INJECT_REJECTED", "engine", n.name,
                         "module '" + mod + "' already UNLOADED");
                    return false;
                }
                n.modules[mod] = "UNLOADED";
                if (mod == "net") {
                    for (auto& l : net_.linksFor(target))
                        net_.setUp(l.a, l.b, false);
                } else if (mod == "disk") {
                    n.ioLoad = 100;
                } else if (mod == "sched") {
                    n.load = 99;
                }
                emit(root, "MODULE_UNLOAD", target, target,
                     target + " module '" + mod + "' UNLOADED", {{"module", mod}}, "WARNING");
            } else { // module-load
                if (n.modules[mod] == "FAILED") {
                    emit(root, "INJECT_REJECTED", "engine", n.name,
                         "module '" + mod + "' FAILED: recover module-fail first");
                    return false;
                }
                if (n.modules[mod] == "LOADED") {
                    emit(root, "INJECT_REJECTED", "engine", n.name,
                         "module '" + mod + "' already LOADED");
                    return false;
                }
                n.modules[mod] = "LOADED";
                if (mod == "net") {
                    for (auto& l : net_.linksFor(target))
                        net_.setUp(l.a, l.b, true);
                } else if (mod == "disk") {
                    n.ioLoad = std::min(n.ioLoad, 20);
                }
                emit(root, "MODULE_LOAD", target, target,
                     target + " module '" + mod + "' LOADED", {{"module", mod}}, "INFO");
            }
            propagate(root);
            break;
        }
        case FaultKind::PROCESS:
            if (def.name == "proc-runaway") {
                // Pick the lowest-pid running process deterministically.
                std::string victim;
                for (const auto& [pid, p] : pm_.all()) {
                    if (p.host == target &&
                        (p.state == ProcState::RUNNING || p.state == ProcState::SLEEPING)) {
                        victim = std::to_string(pid);
                        break;
                    }
                }
                if (victim.empty()) {
                    emit(root, "INJECT_REJECTED", "engine", n.name,
                         "no running process on " + target);
                    return false;
                }
                std::vector<ProcNote> notes;
                std::string err;
                pm_.failAs(victim, "runaway", notes, err);
                emitProcNotes(notes, root);
                raiseFault(target, def.name, def.severity, root, "proc " + victim + " spinning");
            } else if (def.name == "proc-deadlock") {
                std::string victim = value;
                if (victim.empty()) {
                    for (const auto& [pid, p] : pm_.all()) {
                        if (p.host == target &&
                            (p.state == ProcState::RUNNING || p.state == ProcState::SLEEPING)) {
                            victim = std::to_string(pid);
                            break;
                        }
                    }
                }
                if (victim.empty() || !pm_.find(victim)) {
                    emit(root, "INJECT_REJECTED", "engine", n.name,
                         "no such running process on " + target);
                    return false;
                }
                std::vector<ProcNote> notes;
                std::string err;
                pm_.failAs(victim, "deadlock", notes, err);
                emitProcNotes(notes, root);
                raiseFault(target, def.name, def.severity, root, "proc " + victim + " wedged");
            } else { // proc-crash
                std::string victim = value;
                if (victim.empty()) {
                    for (const auto& [pid, p] : pm_.all()) {
                        if (p.host == target &&
                            (p.state == ProcState::RUNNING || p.state == ProcState::SLEEPING)) {
                            victim = std::to_string(pid);
                            break;
                        }
                    }
                }
                if (victim.empty() || !pm_.find(victim)) {
                    emit(root, "INJECT_REJECTED", "engine", n.name,
                         "no such running process on " + target);
                    return false;
                }
                std::vector<ProcNote> notes;
                std::string err;
                pm_.failAs(victim, "crash", notes, err);
                emitProcNotes(notes, root);
                raiseFault(target, def.name, def.severity, root, "proc " + victim + " crashed");
            }
            break;
        case FaultKind::NETWORK:
            if (def.name == "link-failure" || def.name == "iface-down") {
                std::string peer = value;
                if (peer.empty()) {
                    auto links = net_.linksFor(target);
                    if (links.empty()) {
                        emit(root, "INJECT_REJECTED", "engine", n.name,
                             target + " has no links to fail");
                        return false;
                    }
                    std::sort(links.begin(), links.end(), [&](const Link& a, const Link& b) {
                        std::string oa = a.a == target ? a.b : a.a;
                        std::string ob = b.a == target ? b.b : b.a;
                        return oa < ob;
                    });
                    const Link& l0 = links[0];
                    peer = (l0.a == target) ? l0.b : l0.a;
                }
                if (!net_.find(target, peer)) {
                    emit(root, "INJECT_REJECTED", "engine", n.name,
                         "no link " + target + " <-> " + peer);
                    return false;
                }
                net_.setUp(target, peer, false);
                emit(root, def.name == "link-failure" ? "LINK_DOWN" : "IFACE_DOWN", "network",
                     target, target + " <-> " + peer + " down (cause: injected " + def.name + ")",
                     {{"peer", peer}}, "CRITICAL");
                raiseFault(target, def.name, def.severity, root, "peer=" + peer);
            } else if (def.name == "net-congest") {
                int add = hasNum ? iv : 40;
                for (auto& l : net_.linksFor(target)) {
                    if (Link* m = net_.find(l.a, l.b))
                        m->loadPct = std::clamp(m->loadPct + (double)add, 0.0, 100.0);
                }
                raiseFault(target, def.name, def.severity, root,
                           "congestion +" + std::to_string(add) + "%");
            } else { // conn-exhaust
                n.connections = n.maxConnections;
                raiseFault(target, def.name, def.severity, root, "connection table full");
            }
            break;
        case FaultKind::CONFIG:
            n.configCorrupt = true;
            raiseFault(target, def.name, def.severity, root, "config checksum mismatch");
            break;
        case FaultKind::PERMISSION:
            if (def.name == "perm-lock") {
                n.priv = PrivState::LOCKED;
                raiseFault(target, def.name, def.severity, root, "privileges locked");
            } else {
                n.priv = PrivState::CORRUPTED;
                raiseFault(target, def.name, def.severity, root, "identity corrupted");
            }
            break;
        case FaultKind::ROOTFS:
            // rootfs-corruption: the root filesystem itself is damaged.
            n.filesystems["rootfs"] = FsState::CORRUPTED;
            raiseFault(target, def.name, def.severity, root, "rootfs CORRUPTED");
            break;
        case FaultKind::SERVICE: {
            // service-crash [name]: crash a real service (backing proc + record).
            std::string victim;
            if (!value.empty() && svc_.find(value) && svc_.find(value)->node == target) {
                victim = value;
            } else {
                for (const auto& [sname, s] : svc_.all()) { // sorted: deterministic pick
                    if (s.node == target && s.state == ServiceState::RUNNING) {
                        victim = sname;
                        break;
                    }
                }
            }
            if (victim.empty()) {
                emit(root, "INJECT_REJECTED", "engine", n.name,
                     "no running service on " + target + " to crash");
                return false;
            }
            Service* mut = svc_.find(victim);
            mut->state = ServiceState::CRASHED;
            mut->crashCount += 1;
            mut->lastChange = clock_.tickCount();
            const SimProcess* bp =
                (mut->pid > 0) ? pm_.find(std::to_string(mut->pid)) : nullptr;
            if (bp && (bp->state == ProcState::RUNNING || bp->state == ProcState::SLEEPING)) {
                std::vector<ProcNote> notes;
                std::string err;
                pm_.failAs(std::to_string(bp->pid), "crash", notes, err);
                emitProcNotes(notes, root);
            }
            emit(root, "SERVICE_CRASHED", target, target,
                 "service " + victim + " CRASHED on " + target + " (cause: injected)",
                 {{"service", victim}}, "CRITICAL");
            raiseFault(target, def.name, def.severity, root, "service " + victim + " crashed");
            break;
        }
        case FaultKind::COMMAND:
        case FaultKind::RESOURCE:
        case FaultKind::DEPENDENCY:
            // Handled by specific branches above; generic fallback notes the fault.
            raiseFault(target, def.name, def.severity, root, "fault noted");
            break;
    }
    propagate(root);
    return true;
}

void System::kernelPanic(const std::string& name, uint64_t cause, const std::string& reason) {
    Node& n = get(name);
    if (n.kernel == KernelState::PANICKED || n.kernel == KernelState::HALTED) return;
    n.kernel = KernelState::PANICKED;
    n.instability = 100;
    std::vector<ProcNote> notes;
    pm_.crashHost(name, notes);
    emitProcNotes(notes, cause);
    emit(cause, "KERNEL_PANIC", name, name,
         "KERNEL PANIC on " + name + ": " + reason + " (reboot required)",
         {{"reason", reason}, {"cause-detail", "kernel-panic"}}, "PANIC");
    uint64_t fe = raiseFault(name, "kernel-panic", Severity::PANIC, cause, reason);
    transitionTo(name, NodeState::FAILED, cause, "kernel panic", name, fe);
    propagate(cause);
}

bool System::oomKiller(const std::string& name, uint64_t cause) {
    // Terminate the hungriest live process (deterministic: mem desc, pid asc).
    const SimProcess* victim = nullptr;
    for (const auto& [pid, p] : pm_.all()) {
        if (p.host != name || p.state == ProcState::TERMINATED) continue;
        if (!victim || p.memMb > victim->memMb) victim = &p;
    }
    if (!victim) return false;
    std::vector<ProcNote> notes;
    std::string err;
    std::string who = std::to_string(victim->pid);
    int freed = victim->memMb;
    if (!pm_.failAs(who, "oom", notes, err)) return false;
    Node& n = get(name);
    n.oomKills += 1;
    n.memoryUsedMb = std::max(0, n.memoryUsedMb - freed);
    n.swapUsedMb = std::max(0, n.swapUsedMb - freed / 2);
    emitProcNotes(notes, cause);
    emit(cause, "OOM_KILL", "kernel", name,
         "OOM killer terminated " + victim->name + " (pid " + who + "), freed " +
             std::to_string(freed) + "MB",
         {{"pid", who}, {"freed", std::to_string(freed)}}, "CRITICAL");
    return true;
}

void System::stepPhysics(const std::string& name, uint64_t cause) {
    Node& n = get(name);
    // --- thermal: temp relaxes toward equilibrium set by load + frequency ---
    double equilibrium =
        n.ambientC + n.load * 0.6 + std::max(0, n.freqMHz - n.baseFreqMHz) / 100.0;
    double drift = (equilibrium - n.tempC) * 0.25;
    if (drift > 0) drift = std::max(1.0, drift);
    else drift = std::min(-1.0, drift);
    if (std::abs(equilibrium - n.tempC) < 1.0) n.tempC = (int)equilibrium;
    else n.tempC = std::clamp(n.tempC + (int)drift, -40, 250);
    if (n.tempC >= n.criticalC) {
        // Emergency thermal shutdown: escalate, then fail fast.
        // The transition links the fault event so `why` crosses the tick.
        uint64_t fe = raiseFault(name, "overheat", Severity::FAILED, cause,
                                 "temp " + std::to_string(n.tempC) + "C >= critical " +
                                     std::to_string(n.criticalC) + "C");
        transitionTo(name, NodeState::FAILED, cause, "thermal shutdown", name, fe);
        return;
    }
    if (n.tempC >= n.thermalLimitC && !n.throttled) {
        n.throttled = true;
        n.freqMHz = n.baseFreqMHz * 6 / 10;
        n.clockState = ClockState::THROTTLED;
        emit(cause, "CPU_THROTTLE", n.name, n.name,
             n.name + " throttling: temp " + std::to_string(n.tempC) + "C >= limit " +
                 std::to_string(n.thermalLimitC) + "C, freq -> " + std::to_string(n.freqMHz) +
                 "MHz",
             {{"temp", std::to_string(n.tempC)}}, "WARNING");
        raiseFault(name, "overheat", Severity::WARNING, cause, "thermal limit crossed");
    }
    if (n.throttled) {
        n.latencyMs = std::min(5000, n.latencyMs + 12); // throttled CPUs answer slowly
        if (n.tempC < n.thermalLimitC - 10 && n.faults.count("overheat") == 0 &&
            n.faults.count("cooling-fail") == 0) {
            n.throttled = false;
            n.freqMHz = n.baseFreqMHz;
            if (n.clockState == ClockState::THROTTLED) n.clockState = ClockState::STABLE;
            emit(cause, "CPU_UNTHROTTLE", n.name, n.name,
                 n.name + " temperature normalized, full frequency restored");
        }
    }
    // --- clock: over-boosted frequency corrupts computation ---
    if (n.freqMHz > n.maxFreqMHz) {
        if (n.clockState != ClockState::UNSTABLE && n.clockState != ClockState::CRITICAL) {
            n.clockState = ClockState::UNSTABLE;
            emit(cause, "CLOCK_UNSTABLE", n.name, n.name,
                 n.name + " clock unstable at " + std::to_string(n.freqMHz) + "MHz (max " +
                     std::to_string(n.maxFreqMHz) + "MHz)",
                 {}, "WARNING");
        }
        n.instability = std::min(100, n.instability + 8);
        if ((nextRand() % 100) < 25) {
            // Calculation error crashes a random running process.
            std::vector<std::string> cands;
            for (const auto& [pid, p] : pm_.all()) {
                if (p.host == name &&
                    (p.state == ProcState::RUNNING || p.state == ProcState::SLEEPING))
                    cands.push_back(std::to_string(pid));
            }
            if (!cands.empty()) {
                std::string victim = cands[nextRand() % cands.size()];
                std::vector<ProcNote> notes;
                std::string err;
                pm_.failAs(victim, "crash", notes, err);
                emitProcNotes(notes, cause);
                emit(cause, "CALC_ERROR", n.name, n.name,
                     n.name + " calculation error crashed proc " + victim +
                         " (cause: clock instability)",
                     {{"cause-detail", "clock"}}, "WARNING");
            }
        }
        if (n.health > 0) n.health -= 1;
    }
    // Runaway processes pin host CPU.
    {
        bool spinning = false;
        for (const auto& [pid, p] : pm_.all()) {
            if (p.host == name && p.cpu >= 95 &&
                (p.state == ProcState::RUNNING || p.state == ProcState::SLEEPING)) {
                spinning = true;
                break;
            }
        }
        if (spinning) n.load = std::max(n.load, 90);
    }
    // --- memory: leaks accumulate, corruption bites, swap then OOM ---
    if (n.leakMbPerTick > 0 && n.state != NodeState::FAILED) {
        int room = (n.memoryMb - n.memoryUsedMb) + (n.swapMb - n.swapUsedMb);
        int add = std::min(n.leakMbPerTick, std::max(0, room));
        int toMem = std::min(add, n.memoryMb - n.memoryUsedMb);
        n.memoryUsedMb += toMem;
        n.swapUsedMb += (add - toMem);
        if (room <= 0) {
            if (!oomKiller(name, cause)) {
                // The leak only accumulates while mem-leak is active, so link it.
                uint64_t fe = 0;
                auto fit = n.faults.find("mem-leak");
                if (fit != n.faults.end()) fe = fit->second.eventId;
                transitionTo(name, NodeState::FAILED, cause, "out of memory (nothing to kill)",
                             name, fe);
                return;
            }
        }
    }
    if (n.memCorrupt && (nextRand() % 100) < 15) {
        uint64_t roll = nextRand() % 100;
        if (roll < 50) {
            std::vector<std::string> cands;
            for (const auto& [pid, p] : pm_.all()) {
                if (p.host == name &&
                    (p.state == ProcState::RUNNING || p.state == ProcState::SLEEPING))
                    cands.push_back(std::to_string(pid));
            }
            if (!cands.empty()) {
                std::string victim = cands[nextRand() % cands.size()];
                std::vector<ProcNote> notes;
                std::string err;
                pm_.failAs(victim, "segfault", notes, err);
                emitProcNotes(notes, cause);
                emit(cause, "MEM_FAULT", n.name, n.name,
                     n.name + " invalid page killed proc " + victim + " (cause: memory corruption)",
                     {{"cause-detail", "mem-corrupt"}}, "DEGRADED");
            }
        } else if (roll < 80) {
            n.instability = std::min(100, n.instability + 10);
            if (n.kernel == KernelState::RUNNING) {
                n.kernel = KernelState::WARNING;
                emit(cause, "KERNEL_WARNING", n.name, n.name,
                     n.name + " kernel memory error (cause: memory corruption)",
                     {{"cause-detail", "mem-corrupt"}}, "WARNING");
            }
        }
    }
    // --- storage: full disks choke writes, corrupt mounts bite ---
    int storPct = n.storageUsedMb * 100 / std::max(1, n.storageMb);
    if (storPct >= 100) {
        auto it = n.filesystems.find("rootfs");
        if (it != n.filesystems.end() && it->second == FsState::MOUNTED) {
            it->second = FsState::READ_ONLY;
            emit(cause, "FS_READONLY", n.name, n.name,
                 n.name + " rootfs full, remounted read-only (writes fail)",
                 {{"mount", "rootfs"}}, "WARNING");
        }
        raiseFault(name, "disk-full", Severity::WARNING, cause, "storage exhausted");
    }
    bool badRoot = false;
    {
        auto it = n.filesystems.find("rootfs");
        if (it != n.filesystems.end() &&
            (it->second == FsState::CORRUPTED || it->second == FsState::FAILED ||
             it->second == FsState::UNMOUNTED))
            badRoot = true;
    }
    if (badRoot) {
        // The kernel pages in broken data: instability climbs toward panic.
        n.instability = std::min(100, n.instability + 3);
        if (n.health > 0) n.health -= 1;
    }
    bool anyCorrupt = false;
    for (const auto& [m, st] : n.filesystems) {
        if (st == FsState::CORRUPTED || st == FsState::FAILED) {
            anyCorrupt = true;
            break;
        }
    }
    if (anyCorrupt) {
        if ((nextRand() % 100) < 10) {
            std::vector<std::string> cands;
            for (const auto& [pid, p] : pm_.all()) {
                if (p.host == name &&
                    (p.state == ProcState::RUNNING || p.state == ProcState::SLEEPING))
                    cands.push_back(std::to_string(pid));
            }
            if (!cands.empty()) {
                std::string victim = cands[nextRand() % cands.size()];
                std::vector<ProcNote> notes;
                std::string err;
                pm_.failAs(victim, "crash", notes, err);
                emitProcNotes(notes, cause);
            }
        }
        if (n.health > 0) n.health -= 1;
    }
    // Database nodes cannot serve writes without a writable rootfs.
    if (n.type == "database") {
        auto it = n.filesystems.find("rootfs");
        bool writable = (it == n.filesystems.end() || it->second == FsState::MOUNTED ||
                         it->second == FsState::DEGRADED);
        if (!writable && n.state != NodeState::FAILED) {
            if (n.health > 30) n.health -= 3;
            if (n.health <= 50 && n.metadata["db_write_warned"] != "1") {
                n.metadata["db_write_warned"] = "1";
                emit(cause, "DB_WRITE_FAIL", n.name, n.name,
                     n.name + " cannot write: rootfs not writable (logs failing)",
                     {{"cause-detail", "rootfs"}}, "DEGRADED");
            }
            if (n.health > 50) n.metadata["db_write_warned"] = "0";
        }
    }
    // --- kernel: instability resolved or ends in panic ---
    if (n.kernel == KernelState::UNSTABLE) {
        n.instability = std::min(100, n.instability + 2);
        n.latencyMs = std::min(5000, n.latencyMs + 20);
        if ((nextRand() % 100) < 20) {
            std::vector<std::string> cands;
            for (const auto& [pid, p] : pm_.all()) {
                if (p.host == name &&
                    (p.state == ProcState::RUNNING || p.state == ProcState::SLEEPING))
                    cands.push_back(std::to_string(pid));
            }
            if (!cands.empty()) {
                std::string victim = cands[nextRand() % cands.size()];
                std::vector<ProcNote> notes;
                std::string err;
                pm_.failAs(victim, "crash", notes, err);
                emitProcNotes(notes, cause);
            }
        }
        if (n.instability >= 100 && (nextRand() % 100) < 30) {
            kernelPanic(name, cause, "instability reached 100");
            return;
        }
    } else if (n.kernel == KernelState::RUNNING) {
        bool driven = n.memCorrupt || badRoot || n.clockState == ClockState::UNSTABLE ||
                      n.clockState == ClockState::CRITICAL || n.faults.count("kernel-unstable") ||
                      n.faults.count("power-fault");
        if (!driven && n.instability > 0) n.instability = std::max(0, n.instability - 5);
    }
    // Module side-effects while failed.
    auto modNet = n.modules.find("net");
    if (modNet != n.modules.end() && modNet->second == "FAILED") {
        for (auto& l : net_.linksFor(name)) {
            if (Link* m = net_.find(l.a, l.b))
                m->lossPct = std::min(60.0, m->lossPct + 2.0);
        }
    }
}

// ---- network ----


} // namespace override

