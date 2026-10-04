// System (recovery guidance: read-only, state-aware diagnosis).
// rec <target> explains the failure, the dependencies, the approaches, and
// the verification. It never mutates simulation, achievement, or world
// state: no repairs, no spawns, no restores. The operator performs the work.
#include "override/system.hpp"

#include <algorithm>
#include <sstream>
#include <vector>

namespace override {

namespace {
std::string imgState(const Vfs& vfs, const std::string& path) {
    const VFile* f = vfs.file(path);
    if (!f) return "MISSING";
    return f->corrupted ? "CORRUPTED" : "present";
}
} // namespace

std::string System::recoveryReport(const std::string& target, const std::string& sub,
                                   bool detailed) const {
    std::string t = target;
    for (auto& c : t) c = (char)std::tolower((unsigned char)c);
    std::string s = sub;
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    // Bare service names resolve to their service analyzer.
    if (svc_.find(t) != nullptr && t != "kernel" && t != "boot" && t != "filesystem" &&
        t != "rootfs" && t != "module" && t != "service" && t != "network" &&
        t != "device" && t != "node") {
        s = t;
        t = "service";
    }
    std::ostringstream o;
    o << "RECOVERY ANALYSIS\ntarget: " << target << (s.empty() ? "" : " " + sub) << "\n";
    auto cmds = [&](const std::vector<std::string>& list) {
        o << "\nRelevant commands:\n";
        for (const auto& c : list) o << "  " << c << "\n";
    };
    auto verify = [&](const std::vector<std::string>& list) {
        o << "\nVerification:\n";
        for (const auto& c : list) o << "  - " << c << "\n";
    };
    if (t == "kernel") {
        std::string ki = imgState(vfs_, "/boot/kernel");
        o << "\nCondition:\n  kernel image (/boot/kernel): " << ki << "\n";
        int bad = 0;
        for (const auto& name : nodeNames()) {
            const Node& n = get(name);
            if (n.kernel != KernelState::RUNNING || n.faults.count("boot-failure"))
                ++bad;
        }
        o << "  nodes with non-running kernel or boot fault: " << bad << "\n";
        if (ki == "present" && bad == 0) {
            o << "\nNo kernel problem detected: image present, all kernels RUNNING.\n";
            if (!detailed) return o.str();
            o << "\nAffected subsystems (for reference): boot pipeline (KERNEL_LOAD),\n";
            o << "  all nodes at reboot. Dependencies: /boot/kernel -> bootability.\n";
            return o.str();
        }
        o << "\nPossible recovery approaches:\n";
        o << "  1. obtain a known-good recovery package\n";
        o << "  2. load the verified kernel package\n";
        o << "  3. reboot and verify boot\n";
        o << "\nSuggested investigation:\n";
        o << "  - inspect /boot (is the image missing, corrupted, or misplaced?)\n";
        o << "  - obtain the package first; loading without one is refused\n";
        o << "  - verify the destination path before writing\n";
        o << "  - reboot only after a valid kernel exists\n";
        cmds({"sudo get kernel", "sudo load kernel", "reboot <node>", "why <node>",
              "trace <node>"});
        verify({"confirm kernel exists (ls /boot)", "verify boot path (kernel inspect)",
                "reboot node", "node ONLINE, kernel RUNNING"});
        if (detailed) {
            o << "\nAffected subsystems: boot pipeline (KERNEL_LOAD), all nodes at reboot.\n";
            o << "Dependencies: /boot/kernel -> bootability; running kernels continue\n";
            o << "  until reboot (by design), then fail.\n";
            o << "Prerequisites: root mode (sudo -a); a retrieved kernel package;\n";
            o << "  write access to /boot.\n";
            o << "Risks: loading without a retrieved package is refused outright;\n";
            o << "  checkpoint/restore rewinds the whole world, not just the file.\n";
        }
        return o.str();
    }
    if (t == "boot") {
        int failed = 0;
        std::string lastFail;
        for (auto it = events_.all().rbegin(); it != events_.all().rend(); ++it) {
            if (it->type == "BOOT_FAILED") {
                ++failed;
                if (lastFail.empty()) lastFail = it->message;
            }
        }
        o << "\nCondition:\n  recorded boot failures: " << failed << "\n";
        if (!lastFail.empty()) o << "  latest: " << lastFail << "\n";
        o << "  prerequisite checklist:\n";
        o << "    /boot/kernel: " << imgState(vfs_, "/boot/kernel") << "\n";
        o << "    /kernel/kernel.conf: " << imgState(vfs_, "/kernel/kernel.conf") << "\n";
        o << "    /sbin/init|/bin/init: " << imgState(vfs_, "/sbin/init") << "|"
          << imgState(vfs_, "/bin/init") << "\n";
        o << "    /dev/console: " << imgState(vfs_, "/dev/console") << "\n";
        if (failed == 0) o << "\nNo boot failure recorded in the ledger.\n";
        o << "\nPossible recovery approaches:\n";
        o << "  1. repair the exact failed prerequisite named by BOOT_FAILED\n";
        o << "  2. verify the full checklist above before rebooting\n";
        cmds({"dmesg [node]", "kernel inspect <node>", "ls /boot", "ls /sbin",
              "reboot <node>", "why <node>"});
        verify({"BOOT_FAILED cause addressed", "reboot reaches RUNNING phase",
                "node ONLINE"});
        if (detailed) {
            o << "\nAffected subsystems: kernel, drivers, mounts, devices, services.\n";
            o << "Dependencies: kernel image -> KERNEL_LOAD; modules -> DRIVER_INIT;\n";
            o << "  rootfs -> FS_MOUNT; console -> DEVICE_INIT; init -> INIT.\n";
            o << "Prerequisites: each checklist item present and uncorrupted.\n";
            o << "Risks: rebooting with an unrepaired prerequisite fails again and\n";
            o << "  raises another boot-failure fault (ledger grows; state unchanged).\n";
        }
        return o.str();
    }
    if (t == "filesystem" || t == "rootfs" || t == "fs") {
        o << "\nCondition:\n";
        int badMounts = 0;
        for (const auto& name : nodeNames()) {
            const Node& n = get(name);
            auto it = n.filesystems.find("rootfs");
            FsState st = (it == n.filesystems.end()) ? FsState::MOUNTED : it->second;
            if (st != FsState::MOUNTED && st != FsState::DEGRADED) {
                o << "  " << name << " rootfs: " << toString(st) << "\n";
                ++badMounts;
            }
        }
        int corrupt = 0;
        for (const auto& name : nodeNames()) {
            if (get(name).configCorrupt) {
                o << "  " << name << " configuration: CORRUPTED\n";
                ++corrupt;
            }
        }
        if (badMounts == 0 && corrupt == 0)
            o << "  mounts healthy; no corrupted node configs.\n";
        o << "\nPossible recovery approaches:\n";
        o << "  1. obtain a known-good rootfs package (sudo get rootfs)\n";
        o << "  2. load the verified baseline (sudo load rootfs overlays system\n";
        o << "     files; world files outside the baseline are preserved)\n";
        o << "  3. recover any remaining named fault, then reboot if needed\n";
        cmds({"sudo get rootfs", "sudo load rootfs", "inspect <node>", "faults <node>",
              "recover <node> <fault>", "reboot <node>"});
        verify({"rootfs MOUNTED/DEGRADED", "config flags clear", "writes succeed"});
        if (detailed) {
            o << "\nAffected subsystems: storage writes, service starts, boot FS_MOUNT.\n";
            o << "Dependencies: /etc/<node>.conf -> node config; rootfs mount -> writes.\n";
            o << "Prerequisites: root mode (sudo -a); a retrieved rootfs package.\n";
            o << "Risks: loading without a retrieved package is refused outright;\n";
            o << "  load is an overlay, not a reset (clock, ledger, links persist).\n";
        }
        return o.str();
    }
    if (t == "module") {
        if (s.empty()) {
            o << "\nLoaded modules per node (image state in parentheses):\n";
            for (const auto& name : nodeNames()) {
                const Node& n = get(name);
                o << "  " << name << ":";
                for (const auto& [m, st] : n.modules)
                    o << " " << m << "[" << st << "/" << imgState(vfs_, "/lib/modules/" + m + ".ko")
                      << "]";
                o << "\n";
            }
            o << "\nNarrow with: rec module <name> (-h for guidance).\n";
            return o.str();
        }
        o << "\nCondition (module '" << s << "'):\n";
        bool known = false;
        for (const auto& name : nodeNames()) {
            const Node& n = get(name);
            auto it = n.modules.find(s);
            if (it == n.modules.end()) continue;
            known = true;
            o << "  " << name << ": " << it->second << " (image "
              << imgState(vfs_, "/lib/modules/" + s + ".ko") << ")\n";
        }
        if (!known) {
            o << "  unknown module '" << s << "' (known: sched, net, disk).\n";
            return o.str();
        }
        o << "\nPossible recovery approaches:\n";
        o << "  1. module <node> load " << s << " (needs the image present)\n";
        o << "  2. recover a module-fail fault first when the module is FAILED\n";
        o << "  3. restore a deleted image, then load\n";
        cmds({("module <node> load " + s).c_str(), "lsmod <node>", "ls /lib/modules"});
        verify({"module LOADED", "dependent subsystem recovers (links up, io normal)"});
        if (detailed) {
            o << "\nAffected subsystems: net -> links/routes; disk -> io load;\n";
            o << "  sched -> cpu load.\n";
            o << "Dependencies: /lib/modules/<m>.ko -> load/reload and DRIVER_INIT.\n";
            o << "Prerequisites: image present and uncorrupted.\n";
            o << "Risks: unloading net drops the node's links immediately.\n";
        }
        return o.str();
    }
    if (t == "service") {
        const Service* sv = s.empty() ? nullptr : svc_.find(s);
        if (!sv) {
            if (s.empty()) {
                o << "\nServices:\n" << serviceList();
                o << "\nNarrow with: rec <service-name> (-h for guidance).\n";
                return o.str();
            }
            o << "\nNo such service '" << s << "'.\n";
            return o.str();
        }
        o << "\nCondition (service '" << sv->name << "'):\n";
        o << "  state: " << toString(sv->state) << "  host: " << sv->node;
        if (hasNode(sv->node)) o << " [" << toString(get(sv->node).state) << "]";
        o << "\n  binary: " << sv->binary;
        std::string berr;
        o << (resolveBinary(sv->node, sv->binary, berr).empty() ? " (UNRESOLVABLE: " + berr + ")"
                                                               : " (resolvable)");
        o << "\n  config: " << (sv->config.empty() ? "(none)" : sv->config);
        if (!sv->config.empty()) {
            const VFile* f = vfs_.file(sv->config);
            o << (f ? (f->corrupted ? " (CORRUPTED)" : " (present)") : " (MISSING)");
        }
        o << "\n  crashes: " << sv->crashCount << "  policy: " << toString(sv->policy) << "\n";
        o << "\nPossible recovery approaches:\n";
        o << "  1. fix binary/config/rootfs blockers named above, then start\n";
        o << "  2. service restart " << sv->name << " (resets the crash counter)\n";
        cmds({("service inspect " + sv->name).c_str(), ("service start " + sv->name).c_str(),
              "ps", "faults <node>"});
        verify({"service RUNNING", "backing pid alive (ps)", "host ONLINE"});
        if (detailed) {
            o << "\nAffected subsystems: host node (service-down fault while down).\n";
            o << "Dependencies: binary -> /bin|/sbin images; config -> VFS file;\n";
            o << "  host kernel/state -> start gates; rootfs writability.\n";
            o << "Prerequisites: every blocker above cleared in order.\n";
            o << "Risks: 3+ crash loops raise restart-loop (restart resets the count).\n";
        }
        return o.str();
    }
    if (t == "network" || t == "net" || t == "link") {
        o << "\nCondition:\n";
        int down = 0;
        for (const auto& l : net_.links()) {
            if (!l.up) {
                o << "  DOWN: " << l.a << " <-> " << l.b << "\n";
                ++down;
            } else if (l.lossPct > 0.0) {
                o << "  lossy: " << l.a << " <-> " << l.b << " (" << l.lossPct << "%)\n";
            }
        }
        if (down == 0) o << "  all links UP.\n";
        std::string devWhy;
        if (!deviceReady("net0", devWhy)) o << "  device /dev/net0 unavailable (" << devWhy << ")\n";
        o << "\nPossible recovery approaches:\n";
        o << "  1. iface <node> <peer> up (for admin-down links)\n";
        o << "  2. recover the fault behind loss/congestion (faults <node>)\n";
        o << "  3. restore /dev/net0 when the device is the cause\n";
        cmds({"network", "iface <node> <peer> up", "ping <node>", "packet <a> <b>",
              "faults <node>"});
        verify({"link UP", "ping clean", "dependents recover from DEGRADED"});
        if (detailed) {
            o << "\nAffected subsystems: dependents of partitioned nodes (DEGRADED).\n";
            o << "Dependencies: net module -> links; /dev/net0 -> all traffic.\n";
            o << "Prerequisites: distinguish link DOWN (topology) from node FAILED.\n";
            o << "Risks: none beyond the outage itself; links are cheap to toggle.\n";
        }
        return o.str();
    }
    if (t == "device" || t == "dev") {
        if (s.empty()) {
            o << "\n" << deviceList();
            o << "\nNarrow with: rec device <name> (-h for guidance).\n";
            return o.str();
        }
        o << "\n" << deviceInspect(s);
        o << "\nPossible recovery approaches:\n";
        o << "  1. restore a deleted entry (rewrite its metadata path)\n";
        o << "  2. rewrite corrupted metadata (fresh bytes restore integrity)\n";
        cmds({"dev inspect <name>", "ls /dev", "cat /dev/<name>"});
        verify({"device READY", "gated operations succeed again"});
        if (detailed) {
            o << "\nAffected subsystems: see the device's gates line above.\n";
            o << "Dependencies: /dev/<name> -> gated operations (net0, disk0, console).\n";
            o << "Prerequisites: know which operations the device gates.\n";
            o << "Risks: none; device metadata is ordinary VFS state.\n";
        }
        return o.str();
    }
    if (t == "node") {
        if (!hasNode(s)) {
            o << "\nUnknown node '" << s << "'. Narrow with: rec node <name>.\n";
            return o.str();
        }
        const Node& n = get(s);
        o << "\nCondition (" << s << "): state " << toString(n.state) << ", kernel "
          << toString(n.kernel) << ", " << n.faults.size() << " active fault(s).\n";
        for (const auto& [fname, f] : n.faults)
            o << "  fault: " << fname << " [" << toString(f.kind) << "/" << toString(f.severity)
              << "]\n";
        o << "\nPossible recovery approaches follow the faults above; start here:\n";
        cmds({("why " + s).c_str(), ("trace " + s).c_str(), ("faults " + s).c_str(),
              ("inspect " + s).c_str()});
        verify({"node ONLINE", "faults clear", "dependents recover"});
        if (detailed) {
            o << "\nSee why/trace for the effective cause; recovery addresses faults,\n";
            o << "not symptoms. Reboot clears thermal/clock/kernel/power faults only.\n";
        }
        return o.str();
    }
    o << "\nUnknown target '" << target
      << "'. Try: kernel | boot | filesystem | module [name] | service [name] |\n"
         "  network | device [name] | node <name>  (rec -h <target> for depth).\n";
    return o.str();
}

} // namespace override
