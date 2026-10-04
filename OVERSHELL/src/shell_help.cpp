#include "override/shell.hpp"

#include <iostream>

#include "override/fault.hpp"
#include "override/parser.hpp"

namespace override {

std::vector<std::string> overshellCommands() {
    return {"help",     "clear",      "exit",     "status",    "list",      "inspect",
            "watch",    "break",      "repair",   "override",  "inject",    "deceive",
            "undeceive", "beliefs",   "trace",    "why",       "network",   "topology",
            "connect",  "disconnect", "depend",   "ping",      "packet",    "time",
            "tick",     "pause",      "resume",   "history",   "events",    "checkpoint",
            "snapshot", "restore",    "rewind",   "branch",    "replay",    "predict",
            "race",     "benchmark",  "scenario", "experiment", "mode",     "create",
            "remove",   "run",        "script",   "echo",      "seed",      "chaos",
            "validate", "digest",     "ps",       "process",   "autofail",  "local",
            "resources", "cool",       "reboot",   "start",     "recover",   "exec",
            "priv",     "iface",      "world",    "faults",   "ls",       "cd",
            "pwd",      "cat",        "mkdir",    "touch",    "rm",       "cp",
            "mv",       "edit",       "write",    "corrupt",  "restart",  "dir",
            "type",     "md",         "del",      "copy",     "move",     "rename",
            "service",  "halt",       "shutdown", "poweroff",
            "hardware", "hw",       "dev",      "device",   "kernel",   "uname",
            "dmesg",    "lsmod",    "modprobe", "module",   "sysctl",   "threads",
            "thread",   "sched",    "syscalls", "ipc",      "signal",   "create",
            "crt",      "joinw",    "join",     "save",     "reset",    "deletew",
            "note",     "worlds",   "world",    "upt",      "uptime",   "rec",
            "recovery", "color",    "colour",   "achievement", "achievements", "achmt",
            "sudo",     "chmod",    "get",      "load"};
}

void Shell::help(const std::string& topic) const {
    std::string t = toLower(topic);
    if (t.empty() || t == "all") {
        std::cout << "OVERRIDE -- OVERSHELL commands\n"
                     "  CONTROL:      break <n> | repair <n> | override <n> <prop> <val>\n"
                     "                inject <n> <fault> [val] | deceive <obs> <tgt.prop> <val>\n"
                     "                create <n> [type] | remove <n> | depend <n> <dep>\n"
                     "                chaos strike [n]  (CHAOS mode only)\n"
                     "  RECOVER:      cool <n> | reboot <n> | start <n> | recover <n> <fault>\n"
                     "                faults [n] | world\n"
                     "  OBSERVE:      status | list | inspect <n> | watch <n>|system\n"
                     "                network | ping <n> [observer] | packet <a> <b>\n"
                     "                beliefs [n] | validate | digest | seed | resources\n"
                     "                exec <n> <binary> [args] | priv <n> [state] | iface <n> <peer> up|down\n"
                     "  PROCESS:      ps | process list | process inspect <pid|name>\n"
                     "                process spawn <name> [type] [host] [parent]\n"
                     "                process kill|pause|resume|restart <pid|name>\n"
                      "  SERVICE:      service list | service inspect <name>\n"
                      "                service spawn <name> <node> <binary> [config] [policy]\n"
                      "                service start|stop|restart|remove <name>\n"
                      "  SUBSTRATE:    kernel [status|inspect] | uname | dmesg | lsmod |\n"
                      "                modprobe | module | sysctl | dev [list|inspect]\n"
                      "                threads | thread | sched | syscalls | ipc | signal\n"
                      "  AUTH:         sudo -i|-v|-a|-e|-st | sudo <cmd> | chmod [+/-]<perm>\n"
                      "                get|load <kernel|rootfs|..> (root mode; else sudo <cmd>)\n"
                      "                (simulated account/sudo/ACLs; host untouched)\n"
                      "  HOST:         local [info] | local ports | local interfaces\n"
                     "                autofail [on|off|rate <0-100>|status]\n"
                     "  HARDWARE:     hardware | hardware backend <res> <V|R>\n"
                     "                hardware overflow <keep-r|reclaim|cancel>\n"
                     "                (simulated profile; never touches host hardware)\n"
                     "  VFS:          ls [path] | cd <path> | pwd | cat <path> | mkdir <path>\n"
                     "                touch|rm|cp|mv <path...> | edit <path> <text> | corrupt <path>\n"
                     "                (virtual tree only -- the host disk is never touched)\n"
                     "  TRACE:        trace <n> (HOW: timeline) | why <n> (WHY: causal chain)\n"
                     "                events [n] [limit] | history\n"
                     "  TIME:         time | tick [n] | pause | resume\n"
                     "                checkpoint [label] | restore <id> | rewind [n]\n"
                     "                replay [n] | branch <label>\n"
                     "  WORLDS:       create w | crt w | joinw <w> | save | reset [--force]\n"
                     "                deletew <w> [--force] | note [text] | worlds | world\n"
                     "                upt [-at]  (persistent w1..w6 slots, .ord files)\n"
                     "  EXPERIMENT:   predict <n> | race <cmd...> | benchmark [ticks]\n"
                     "                scenario list | scenario run <name>\n"
                     "                experiment run <scenario> | experiment compare\n"
                     "  MODES:        mode [name]  (enforced: normal observe debug forensic\n"
                     "                               chaos override safe simulation time_travel\n"
                     "                               watch; others reserved, behave as normal)\n"
                      "  SHELL:        help [cmd] | help warnings | clear | run <file> | exit\n"
                      "                color [on|off|auto] | achievement | achmt\n"
                      "Type `help <cmd>` for details (e.g. `help break`).\n";
        return;
    }
    if (t == "break" || t == "kill" || t == "fail")
        std::cout << "break <node>: node -> FAILED; dependents DEGRADED via propagation.\n"
                     "  rejected when already FAILED. blocked in OBSERVE/FORENSIC/TIME_TRAVEL/SAFE.\n";
    else if (t == "repair" || t == "fix" || t == "heal")
        std::cout << "repair <node>: FAILED/* -> RESTARTING -> ONLINE, then dependents recover.\n"
                     "  refused when the kernel panicked (reboot required). partial when config\n"
                     "  is corrupted. does NOT clear thermal/storage/fs faults (use cool/recover).\n";
    else if (t == "override" || t == "set")
        std::cout << "override <node> <prop> <value>: direct state edit.\n"
                     "  props: health [0,100] | latency (ms) | load [0,100] |\n"
                     "         connections (>=0) | memory (<= capacity) | temp [-40,250]C |\n"
                     "         freq [100,12000]MHz | storage (<= capacity) | io [0,100] |\n"
                     "         state | proc | meta.<k>\n"
                     "  state/proc edits require OVERRIDE mode. out-of-range values are rejected.\n";
    else if (t == "inject") {
        std::cout << "inject <node> <fault> [value]: controlled fault from the fault catalog.\n"
                     "  legacy: latency [ms] | packet_loss [0-100] | corruption | pressure |\n"
                     "          crash | pause\n"
                     "  thermal is an alias for overheat (value: extra degrees C).\n"
                     "  catalog:\n";
        for (const auto& fn : faultNames()) {
            const FaultDef* d = faultDef(fn);
            std::cout << "    " << fn << " [" << toString(d->kind) << "/" << toString(d->severity)
                      << "] " << d->description;
            if (!d->injectable) std::cout << "  (engine-raised, not injectable)";
            std::cout << "\n";
        }
        std::cout << "  value is fault-specific (delta, MB/tick, peer, mount, module, proc).\n"
                     "  recover one fault with `recover <node> <fault>`.\n";
    }
    else if (t == "deceive" || t == "lie" || t == "spoof")
        std::cout << "deceive <observer> <target.property> <value>:\n"
                     "  plants a false belief (ground truth unchanged).\n"
                     "  e.g. deceive client server.latency 20\n";
    else if (t == "undeceive")
        std::cout << "undeceive <observer> [target]: clear planted beliefs.\n";
    else if (t == "create" || t == "spawn" || t == "add")
        std::cout << "create <name> [type]: spawn a node (names: [a-z][a-z0-9_-]*, max 32).\n";
    else if (t == "remove" || t == "delete" || t == "destroy" || t == "terminate")
        std::cout << "remove <name>: drop links, prune dependency refs, purge beliefs.\n"
                     "  past snapshots/events keep copies; restoring one brings the node back.\n";
    else if (t == "depend")
        std::cout << "depend <node> <dep>: add a dependency edge (both must exist).\n";
    else if (t == "trace")
        std::cout << "trace <node>: HOW the node reached its state -- chronological timeline\n"
                     "  of its transitions plus dependency context. (why = single causal chain.)\n"
                     "  after a restore, shows the current incarnation separately from kept\n"
                     "  ledger history; ends with the effective root cause (never a trigger).\n";
    else if (t == "why")
        std::cout << "why <node>: WHY the current state exists -- causal chain, root first.\n"
                     "  each line is tagged [NODE]/[LINK]/[PROCESS]/[SERVICE]/[VFS]/[SYSTEM]:\n"
                     "  node-scoped lines read node->node (never a self-link); time-driven\n"
                     "  steps are marked (time trigger) and kept in the chain, while the\n"
                     "  footer names the effective root cause plus the driving fault.\n";
    else if (t == "events" || t == "log")
        std::cout << "events [node] [limit]: ledger, newest first (ledger is append-only;\n"
                     "  restore never deletes events). lines carry [scope] tags, see help why.\n";
    else if (t == "tick" || t == "step" || t == "ff")
        std::cout << "tick [n]: advance simulation clock n>=1 ticks (blocked when paused).\n";
    else if (t == "pause" || t == "resume")
        std::cout << "pause | resume: freeze/continue the simulation clock.\n";
    else if (t == "checkpoint" || t == "snapshot" || t == "snap")
        std::cout << "checkpoint [label]: capture nodes, links, clock, seed, VFS,\n"
                     "  processes, services, and hardware (incl. R backing).\n";
    else if (t == "restore")
        std::cout << "restore <id>: rewind the WORLD to a checkpoint (events are kept).\n"
                     "  why/trace then explain the current incarnation only; older ledger\n"
                     "  history stays visible under an explicit history header.\n";
    else if (t == "rewind")
        std::cout << "rewind [n]: restore the nth previous checkpoint (default: latest).\n";
    else if (t == "branch")
        std::cout << "branch <label>: labelled checkpoint (linear history, no tree).\n";
    else if (t == "replay")
        std::cout << "replay [n]: display the last n events (display-only, deterministic).\n";
    else if (t == "connect" || t == "link" || t == "disconnect" || t == "unlink")
        std::cout << "connect <a> <b> | disconnect <a> <b>: link control.\n"
                     "  a new link also adds dependency a -> b. severed links degrade dependents.\n";
    else if (t == "ping")
        std::cout << "ping <node> [observer]: 4-probe statistical ping; FAILED targets give\n"
                     "  no reply with 100% loss; missing routes are UNREACHABLE.\n"
                     "  observers see believed latency.\n";
    else if (t == "packet")
        std::cout << "packet <from> <to>: routed delivery over UP links.\n"
                     "  FAILED endpoints -> DROPPED; no route -> UNREACHABLE; else DELIVERED.\n"
                     "  congestion adds delay and deterministic loss.\n";
    else if (t == "process" || t == "ps")
        std::cout << "ps | process list: simulated process table (no OS interaction).\n"
                     "  process inspect <pid|name> | spawn <name> [type] [host] [parent]\n"
                      "  process kill <pid|name>\n"
                      "    [crash|oom|segfault|runaway|deadlock|sigkill|zombie|block|degrade]\n"
                      "  process pause|resume|restart <pid|name>\n"
                      "  killing a host's last running process decays its health.\n";
    else if (t == "service" || t == "svc")
        std::cout << "service list: services bound to nodes (OVERKNRL service layer).\n"
                     "  service spawn <name> <node> <binary> [config] [ALWAYS|NEVER]\n"
                     "  service start|stop|restart|remove <name> | service inspect <name>\n"
                     "  ALWAYS services auto-start on ticks/repair/reboot; a down service\n"
                     "  raises a node service-down fault; 3+ crashes raise restart-loop.\n"
                     "  Services and processes are separate registries: a process TYPE\n"
                     "  like \"service\" is just a label. `service spawn` registers a\n"
                     "  managed service; the world starts with none.\n";
    else if (t == "cool")
        std::cout << "cool <node>: thermal recovery only (temp down, unthrottle, clear\n"
                     "  thermal faults). storage/fs/config faults survive.\n";
    else if (t == "reboot")
        std::cout << "reboot <node>: revive dead kernels; clears thermal/clock/kernel/power\n"
                     "  faults and restarts processes. disk/fs/config/perm faults persist.\n";
    else if (t == "start")
        std::cout << "start <node>: reboot when the kernel is dead or off, else repair.\n";
    else if (t == "halt" || t == "shutdown" || t == "poweroff")
        std::cout << "halt <node>: orderly shutdown (processes STOPPED, kernel OFF,\n"
                     "  node PAUSED). not a failure: dependents see PAUSED. `start`\n"
                     "  boots it again; `repair` is refused while OFF.\n";
    else if (t == "recover" || t == "fix-fault" || t == "resolve")
        std::cout << "recover <node> <fault>: resolve one stacked fault with kind-specific\n"
                     "  cleanup (free space, remount, unthrottle, module OK, ...). fs-corrupt\n"
                     "  needs two passes (CORRUPTED -> DEGRADED -> MOUNTED). kernel panic\n"
                     "  cannot be recovered (reboot required).\n";
    else if (t == "faults")
        std::cout << "faults [node]: list stacked active faults world-wide or per node.\n";
    else if (t == "world")
        std::cout << "world: subsystem health, active faults, and the recent causal chain.\n";
    else if (t == "exec")
        std::cout << "exec <node> <binary> [args]: simulated execution (exit 0 or a\n"
                     "  structured, event-backed failure: NOENT, permission, read-only fs,\n"
                     "  unavailable fs, dead kernel, allocation failure).\n";
    else if (t == "priv")
        std::cout << "priv <node> [state]: show/set simulated privileges\n"
                     "  (ROOT|USER|LOCKED|RESTRICTED|CORRUPTED). privileged binaries need ROOT.\n";
    else if (t == "iface" || t == "interface")
        std::cout << "iface <node> <peer> <up|down>: toggle a link endpoint (interface).\n";
    else if (t == "autofail")
        std::cout << "autofail [on|off|rate <0-100>|status]: background entropy.\n"
                     "  wear, latency spikes, pressure, crashes strike strained nodes;\n"
                     "  seeded and deterministic. link loss/congestion decays on its own.\n";
    else if (t == "local")
        std::cout << "local [info] | local ports | local interfaces: fake local machine.\n"
                     "  simulated inventory only; load derives from (seed, tick).\n";
    else if (t == "resources")
        std::cout << "resources: node CPU/temp/RAM/storage/connections plus link congestion.\n"
                     "  high CPU raises latency; memory strain decays health.\n";
    else if (t == "hardware" || t == "hw")
        std::cout << "hardware: virtual hardware profile (simulated i5/4GB/64GB) plus\n"
                     "  per-resource V/R backends and the V->R overflow policy.\n"
                     "  V = fully virtual; R = isolated in-RAM backing (never host disk).\n"
                     "  hardware backend <cpu|ram|storage|net> <V|R>\n"
                     "  hardware overflow <keep-r|reclaim|cancel>\n"
                     "  keep-r keeps overflow bytes in R; reclaim migrates R back when\n"
                     "  it fits, else refuses; cancel refuses any V overflow.\n";
    else if (t == "ls" || t == "dir" || t == "cd" || t == "pwd" || t == "cat" || t == "type" ||
             t == "mkdir" || t == "md" || t == "touch" || t == "rm" || t == "del" || t == "cp" ||
             t == "copy" || t == "mv" || t == "move" || t == "rename" || t == "edit" ||
             t == "write" || t == "corrupt" || t == "vfs")
        std::cout << "virtual filesystem (OVERKNRL; host disk is never touched):\n"
                     "  ls [path] | cd <path> | pwd | cat <path> | mkdir <path>\n"
                     "  touch <path> | rm <path> | cp <src> <dst> | mv <src> <dst>\n"
                     "  edit <path> <text...> (create-or-replace) | corrupt <path>\n"
                     "  a file whose name matches a node (e.g. /etc/server.conf) is\n"
                     "  node-owned: writes need that node's rootfs writable, and\n"
                     "  corrupting an /etc/ config corrupts the node's configuration.\n";
    else if (t == "predict" || t == "forecast")
        std::cout << "predict <node>: rule-based next-event forecast from live signals.\n";
    else if (t == "race")
        std::cout << "race <command...>: run a command timed vs the event log.\n";
    else if (t == "benchmark" || t == "bench")
        std::cout << "benchmark [ticks]: break->tick->repair->tick drill with a kept baseline.\n";
    else if (t == "scenario" || t == "scenarios")
        std::cout << "scenario list | scenario run <name>: reproducible failure scripts.\n";
    else if (t == "experiment")
        std::cout << "experiment run <scenario> | experiment compare: bracketed runs with\n"
                     "  before/after checkpoints.\n";
    else if (t == "chaos")
        std::cout << "chaos strike [node]: seeded random fault (CHAOS mode only, deterministic).\n";
    else if (t == "seed")
        std::cout << "seed [value]: show or set the determinism seed (same seed + same\n"
                     "  commands = identical state and events).\n";
    else if (t == "validate")
        std::cout << "validate: check every node/link invariant.\n";
    else if (t == "digest")
        std::cout << "digest: canonical state + ledger hashes (replay/equivalence checks).\n";
    else if (t == "mode")
        std::cout << "mode [name]: enforced modes change behavior:\n"
                     "  normal: full control.  observe: read-only (world frozen).\n"
                     "  debug: verbose footer.  forensic: frozen, navigation allowed.\n"
                     "  chaos: unlocks `chaos strike`.  override: unlocks state/proc edits.\n"
                     "  safe: destructive ops blocked.  simulation: auto-tick per command.\n"
                     "  time_travel: time ops only.  watch: auto-display status after changes.\n"
                     "  all other modes are reserved (accepted, behave as normal).\n";
    else if (t == "status" || t == "list" || t == "inspect" || t == "show" || t == "watch")
        std::cout << t << ": read-only observation (always allowed).\n";
    else if (t == "warnings" || t == "warning" || t == "confirm" || t == "force")
        std::cout
            << "warnings: destructive operations print a WARNING first (see last warning\n"
               "  in interactive output). Levels:\n"
               "  WARN: proceed after the warning (break, inject, reboot, rm/corrupt of\n"
               "    ordinary files, process kill, service stop, link/interface down).\n"
               "  CONFIRM: halt, service remove, remove <node>, rm/corrupt of critical\n"
               "    paths (/boot, /kernel, /lib/modules, /etc/, /dev/, /sbin/init),\n"
               "    module unload/fail. Headless/script use requires --force/--yes;\n"
               "    interactive shells ask Continue? [y/N]. Never silently auto-confirmed.\n"
               "  warnings are presentation only: they never mutate simulation state.\n";
    else if (t == "worlds" || t == "world" || t == "joinw" || t == "save" || t == "reset" ||
             t == "deletew" || t == "create")
        std::cout
            << "worlds: up to 6 persistent slots (w1..w6) as .ord files under the app\n"
               "  data dir (never inside the simulated VFS). A file exists only for an\n"
               "  existing world; nothing is pre-created.\n"
               "  create w | crt w: fresh world in the lowest free slot (fails at 6/6).\n"
               "  joinw <w>: save current, load target (failure keeps current active).\n"
               "  save: persist current world (+uptime session commit). reset [--force]:\n"
               "    fresh simulation, same identity/uptime/achievements. deletew <w>:\n"
               "    permanent slot free (typed DELETE interactive, --force headless).\n"
               "  note [text]: world annotation. worlds: slot list. world: active info.\n"
               "  upt [-at]: session vs cumulative active lifetime (pause-safe,\n"
               "    rewind-proof; reset keeps it, deleting the world deletes it).\n"
               "  restart != reset, switch != merge, achievement state != world state.\n";
    else if (t == "rec" || t == "recovery")
        std::cout
            << "rec <target> [-h]: read-only recovery diagnosis (never repairs anything).\n"
               "  targets: kernel | boot | filesystem | module [name] | service [name] |\n"
               "    network | device [name] | node <name> | <service-name>.\n"
               "  reports the live condition, recovery approaches, investigation steps,\n"
               "  relevant commands, and verification. -h adds subsystems, detailed\n"
               "  dependencies, prerequisites, and risks. Reading guidance unlocks\n"
               "  nothing: only real repairs change the world.\n";
    else if (t == "color" || t == "colour")
        std::cout << "color [on|off|auto]: terminal styling (default auto: on in\n"
                     "  interactive shells, off headless/in scripts/in tests). NO_COLOR=1\n"
                     "  forces plain output. Styling never changes simulation state,\n"
                     "  digests, or saved files; markers (ERROR:/WARNING:/OK:) stay put.\n";
    else if (t == "achievement" || t == "achievements" || t == "achmt")
        std::cout << "achievement | achmt: real-subsystem achievement viewer.\n"
                     "  headless: prints the full list ([X] unlocked, [ ] visible,\n"
                     "  [?] hidden, progress lines for multi-step ones). interactive:\n"
                     "  paged keyboard app (Left/Right navigate, Q/E exit, 10 per page).\n"
                     "  unlocks persist in achievement.ord across worlds, rewind, reset,\n"
                     "  and relaunch; notifications show once and never advance sim time.\n";
    else if (t == "network" || t == "topology" || t == "route")
        std::cout << "network: links plus dependency edges.\n";
    else if (t == "time" || t == "clock" || t == "history" || t == "beliefs" || t == "run" ||
             t == "script" || t == "load" || t == "source" || t == "echo" || t == "clear" ||
             t == "exit" || t == "quit")
        std::cout << t << ": shell utility (see `help`).\n";
    else if (t == "dev" || t == "device" || t == "devices")
        std::cout << "dev [list|inspect <name>]: simulated /dev metadata + gates.\n"
                     "  net0 gates iface/packet/ping, disk0 gates stored writes,\n"
                     "  console gates boot. Deleting/corrupting entries has real effects.\n";
    else if (t == "kernel")
        std::cout << "kernel [status|inspect <node>]: kernel state, boot image/init/\n"
                     "  config presence, modules with image state, last boot phases.\n"
                     "  All derived from engine state + ledger (no parallel logging).\n";
    else if (t == "uname" || t == "dmesg" || t == "lsmod")
        std::cout << t << ": uname [node] (sim identity), dmesg [node] [limit]\n"
                     "  (kernel ring: KERNEL_/BOOT_/MODULE_ ledger events), lsmod [node]\n"
                     "  (modules with image presence).\n";
    else if (t == "modprobe" || t == "module")
        std::cout << "modprobe <node> <mod> | module <node> <load|unload|fail> <mod>:\n"
                     "  admin module control through the engine fault-effect path.\n"
                     "  unload/fail confirm (destructive: net loss drops links).\n";
    else if (t == "sysctl")
        std::cout << "sysctl <node> [key[=value]]: thermal.limit, net.base_latency,\n"
                     "  vm.swap_mb (validated writes); kernel.instability,\n"
                     "  kernel.modules (read-only).\n";
    else if (t == "threads" || t == "thread")
        std::cout << "threads [proc] | thread spawn <proc> <name> | thread inspect <tid>:\n"
                     "  threads live inside processes (task->process->thread), snapshotted\n"
                     "  and digested with everything else. tid = pid*1000+slot.\n";
    else if (t == "sched" || t == "syscalls" || t == "ipc" || t == "signal")
        std::cout << "sched: ready/running/blocked queues in priority order + cpu time.\n"
                     "  syscalls: operator-issued call-site counters (observational).\n"
                     "  ipc [list|send <proc> <msg>|recv <proc>]: bounded live mailboxes.\n"
                     "  signal <TERM|STOP|CONT|KILL|HUP> <proc>: real state transitions.\n";
    else if (t == "ps" || t == "process")
        std::cout << "process list | inspect <pid|name> | spawn <name> [type] [host]\n"
                     "  [parent] | kill <pid|name> [reason] | pause|resume|restart |\n"
                     "  tree (task hierarchy with thread counts). Kill warns.\n";
    else if (t == "uptime" || t == "upt")
        std::cout << "upt [-at]: session vs cumulative active lifetime of the active\n"
                     "  world (persisted in .ord). Pause does not stop it; rewind never\n"
                     "  rolls it back; reset keeps it; deleting the world deletes it.\n";
    else if (t == "sudo")
        std::cout << "sudo <command> [args...]: execute an OVERRIDE command with\n"
                     "  simulated root privileges (host privileges are never granted\n"
                     "  and the host OS is never modified). The session itself is\n"
                     "  unchanged afterwards; persistent root mode is separate.\n"
                     "  -i installs sudo for the account (password once); -v version;\n"
                     "  -a activates root ([root@override]#) after password auth;\n"
                     "  -e exits root mode; -st shows status; sudo <cmd> runs one\n"
                     "  command as root without switching the session.\n"
                     "  root mode ([root@override]#) means already-root: commands run\n"
                     "  directly there (e.g. `get kernel`), no wrapper needed.\n"
                     "  sudo get <kernel|rootfs|kernel,rootfs>: obtain a simulated\n"
                     "    recovery package (never touches the world).\n"
                     "  sudo load <kernel|rootfs|kernel,rootfs>: apply a verified\n"
                     "    package to the active world (transactional).\n"
                     "  get = obtain a package; load = apply it. See `rec kernel`.\n";
    else if (t == "get" || t == "load")
        std::cout << t << " <kernel|rootfs|kernel,rootfs>: privileged recovery\n"
                     "  operation (requires simulated root mode; see `help sudo`).\n"
                     "  In root mode run it directly (`get kernel`); otherwise use\n"
                     "  `sudo get kernel` (one-shot elevation). get obtains a\n"
                     "  package, load applies it. Never touches the host.\n";
    else if (t == "chmod")
        std::cout << "chmod [+/-]<ur|uw|or|ow|rtr|rtw> <path>: simulated ACLs.\n"
                     "  actors: u ser, o wner, rt root (values 2/0/8); caps: r ead (5),\n"
                     "  w rite (7). Only the entry owner (or root) may chmod. Enforced\n"
                     "  by cat/edit/touch/mkdir/rm/cp/mv/corrupt and binary execution.\n"
                     "  Root bypasses inside the simulation (hard safety gates remain).\n";
    else
        std::cout << "no help for '" << topic << "'. Try `help`.\n";
}


} // namespace override

