#include "override/shell.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>

#include "override/parser.hpp"
#include "override/color.hpp"
#include "override/fault.hpp"
#include "override/progress.hpp"
#include "override/scenario.hpp"
#include "override/worlds.hpp"

#include <iomanip>
#if defined(_WIN32)
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif
#include <thread>

namespace override {


namespace {
// Permission kinds for the mode matrix (see Shell::Op in shell.hpp).

// Adapter: engine progress counts -> one in-place terminal line. Silent for
// small work (see wantProgressBar); the caller finishes the line. The bar
// painter is presentation-injected (plain ASCII unless colors are on).
ProgressCb shellProgress(ProgressOut& bar) {
    bar.setPainter(color::paintBar);
    return [&](int done, int total) { bar.update(done, total); };
}

// Enforced modes: NORMAL, OBSERVE, DEBUG, FORENSIC, CHAOS, OVERRIDE, SAFE,
// SIMULATION, TIME_TRAVEL, WATCH. Everything else is reserved (behaves as NORMAL).
bool isReservedMode(const std::string& m) {
    static const std::vector<std::string> enforced = {
        "NORMAL", "OBSERVE", "DEBUG", "FORENSIC", "CHAOS", "OVERRIDE", "SAFE", "SIMULATION",
        "TIME_TRAVEL", "WATCH"};
    return std::find(enforced.begin(), enforced.end(), m) == enforced.end();
}

std::string join(const std::vector<std::string>& v, size_t from) {
    std::string o;
    for (size_t i = from; i < v.size(); ++i) {
        if (i > from) o += " ";
        o += v[i];
    }
    return o;
}

// Legacy inject names predating the fault catalog (kept working).
bool isLegacyFault(const std::string& f) {
    static const std::vector<std::string> legacy = {
        "latency", "packet_loss", "packet-loss", "loss", "corruption", "corrupt",
        "pressure", "memory", "oom", "cpu", "crash", "kill", "fail", "pause"};
    return std::find(legacy.begin(), legacy.end(), f) != legacy.end();
}

// ---- destructive-operation warnings (presentation only; never mutates) ----

struct Risk {
    enum class Level { None, Warn, Confirm };
    Level level = Level::None;
    std::string message;
};

// Explicit --force/--yes/-y anywhere among fixed-arity args (never inside
// free-text commands like edit/echo, which take no confirmation).
struct ForceArgs {
    bool force = false;
    std::vector<std::string> args;
};

ForceArgs stripForce(const std::vector<std::string>& a) {
    ForceArgs f;
    for (const auto& x : a) {
        std::string l = toLower(x);
        if (l == "--force" || l == "--yes" || l == "-y") f.force = true;
        else f.args.push_back(x);
    }
    return f;
}

// Paths whose damage breaks boot/devices/config (CONFIRM); other damage WARNs.
bool isCriticalPath(const std::string& abs) {
    if (abs == "/sbin/init" || abs == "/bin/init") return true;
    for (const char* p : {"/boot", "/kernel", "/lib/modules", "/etc/", "/dev/"}) {
        std::string pre(p);
        if (abs == pre.substr(0, pre.size() - (pre.back() == '/' ? 1 : 0))) return true;
        if (abs.rfind(pre, 0) == 0) return true;
    }
    return false;
}

// Dependents that would feel a node going down (for cascade warnings).
std::string dependentList(const System& sys, const std::string& node) {
    std::string out;
    for (const auto& n : sys.nodeNames()) {
        if (n == node) continue;
        const auto& deps = sys.get(n).dependencies;
        if (std::find(deps.begin(), deps.end(), node) != deps.end()) {
            if (!out.empty()) out += ", ";
            out += n;
        }
    }
    return out;
}

// verb: canonical operation ("halt", "reboot", "rm", "service-remove", ...).
Risk assessRisk(const std::string& verb, const std::vector<std::string>& args,
                const System& sys) {
    Risk r;
    auto cascade = [&](const std::string& node) {
        std::string deps = dependentList(sys, node);
        if (!deps.empty())
            r.message += " It may cascade to dependent node(s): " + deps + ".";
    };
    if (verb == "halt") {
        r.level = Risk::Level::Confirm;
        r.message = "halting `" + (args.empty() ? "?" : args[0]) +
                    "` powers its kernel OFF (node PAUSED).";
        if (!args.empty()) cascade(args[0]);
    } else if (verb == "reboot") {
        r.level = Risk::Level::Warn;
        r.message = "rebooting `" + (args.empty() ? "?" : args[0]) +
                    "` clears volatile kernel/thermal/clock state (disk/fs/config kept).";
        if (!args.empty()) cascade(args[0]);
    } else if (verb == "break") {
        r.level = Risk::Level::Warn;
        r.message = "this may destabilize node `" + (args.empty() ? "?" : args[0]) +
                    "` and trigger cascading failures.";
        if (!args.empty()) cascade(args[0]);
    } else if (verb == "inject") {
        r.level = Risk::Level::Warn;
        r.message = "injecting a fault may destabilize the target and cascade to dependents.";
    } else if (verb == "service-stop") {
        r.level = Risk::Level::Warn;
        r.message = "stopping `" + (args.empty() ? "?" : args[0]) +
                    "` may degrade its host (service-down fault) and dependents.";
    } else if (verb == "service-remove") {
        r.level = Risk::Level::Confirm;
        r.message = "removing service `" + (args.empty() ? "?" : args[0]) +
                    "` is destructive inside the simulated world (registry entry deleted).";
    } else if (verb == "remove-node") {
        r.level = Risk::Level::Confirm;
        r.message = "removing node `" + (args.empty() ? "?" : args[0]) +
                    "` drops its links, purges hosted processes/services, and may cascade.";
        if (!args.empty()) cascade(args[0]);
    } else if (verb == "rm" || verb == "corrupt") {
        std::string path = args.empty() ? "?" : args[0];
        std::string abs = Vfs::normalize("/", path);
        bool critical = !abs.empty() && isCriticalPath(abs);
        r.level = critical ? Risk::Level::Confirm : Risk::Level::Warn;
        if (verb == "rm") {
            r.message = critical
                            ? "deleting `" + abs +
                                  "` may break boot/devices/config (e.g. /boot/kernel "
                                  "makes nodes unable to boot)."
                            : "deleting `" + abs +
                                  "` is destructive inside the simulated world.";
        } else {
            r.message = critical
                            ? "corrupting `" + abs +
                                  "` may break boot/devices/config with no magical recovery."
                            : "corrupting `" + abs +
                                  "` is destructive inside the simulated world.";
        }
    } else if (verb == "proc-kill" || verb == "signal") {
        r.level = Risk::Level::Warn;
        r.message = "killing `" + (args.empty() ? "?" : args[0]) +
                    "` may starve its host and trigger supervision cascades.";
    } else if (verb == "disconnect" || verb == "iface-down") {
        r.level = Risk::Level::Warn;
        r.message = "taking this link/interface down may degrade dependents and trigger "
                    "cascading failures.";
    } else if (verb == "module-unload" || verb == "module-fail") {
        r.level = Risk::Level::Confirm;
        std::string what = args.size() >= 2 ? args[0] + " " + args[1] : "?";
        r.message = "module " + what +
                    " changes real simulated subsystem state (e.g. unloading `net` "
                    "disables networking) and may cascade.";
    } else if (verb == "chaos") {
        r.level = Risk::Level::Warn;
        r.message = "chaos strike injects an uncontrolled fault (seeded, but surprising).";
    } else if (verb == "chmod") {
        r.level = Risk::Level::Warn;
        r.message = "changing permissions can lock operations out (including your own).";
    } else if (verb == "reset") {
        r.level = Risk::Level::Confirm;
        r.message = "resetting world `" + (args.empty() ? "?" : args[0]) +
                    "` returns its simulation to fresh (identity, achievements, and "
                    "all-time uptime are kept).";
    }
    return r;
}
} // namespace

void Shell::print(const std::string& s) const {
    lastOut_ = s;
    if (!color::enabled()) {
        std::cout << s;
        if (!s.empty() && s.back() != '\n') std::cout << "\n";
        std::cout << std::flush;
        return;
    }
    // Painted line-by-line; raw text (and newlines) preserved exactly.
    size_t start = 0;
    while (start <= s.size()) {
        size_t e = s.find('\n', start);
        bool last = (e == std::string::npos);
        std::string line = last ? s.substr(start) : s.substr(start, e - start);
        std::cout << color::paintLine(line);
        if (!last) std::cout << "\n";
        if (last) break;
        start = e + 1;
    }
    if (!s.empty() && s.back() != '\n') std::cout << "\n";
    std::cout << std::flush;
}

// Central permission check. Returns "" when allowed, else a reason.
std::string Shell::permits(Op op, const std::string& verb) {
    const std::string& m = sys_.mode();
    if (isReservedMode(m)) return ""; // reserved modes behave as NORMAL
    if (op == Op::MUTATE) {
        if (m == "OBSERVE") return "OBSERVE mode: mutation '" + verb + "' blocked (read-only).";
        if (m == "FORENSIC")
            return "FORENSIC mode: frozen timeline; mutations blocked (use trace/why/replay).";
        if (m == "TIME_TRAVEL")
            return "TIME_TRAVEL mode: only time operations allowed (checkpoint/restore/rewind).";
        if (m == "SAFE") {
            static const std::vector<std::string> destructive = {
                "break", "inject", "deceive", "disconnect", "remove", "chaos"};
            if (std::find(destructive.begin(), destructive.end(), verb) != destructive.end())
                return "SAFE mode: '" + verb + "' blocked (destructive).";
        }
    } else if (op == Op::ADVANCE) {
        if (m == "OBSERVE") return "OBSERVE mode: clock is frozen (read-only).";
        if (m == "FORENSIC") return "FORENSIC mode: clock is frozen (investigate the past).";
    } else { // RESTORE
        if (m == "OBSERVE") return "OBSERVE mode: restore/rewind/branch blocked (read-only).";
    }
    return "";
}

bool Shell::guard(Op op, const std::string& verb) {
    std::string why = permits(op, verb);
    if (!why.empty()) {
        print(why);
        return false;
    }
    return true;
}

bool Shell::riskGate(const std::string& verb, const std::vector<std::string>& args,
                     bool force) {
    Risk risk = assessRisk(verb, args, sys_);
    if (risk.level == Risk::Level::None) return true;
    lastWarning_ = risk.message;
    if (risk.level == Risk::Level::Warn) {
        print("WARNING: " + risk.message);
        return true;
    }
    // CONFIRM level: explicit --force, or an interactive Continue? [y/N].
    // Headless/script/test paths without --force are refused, never silently
    // auto-confirmed. Pure presentation: no simulation state touched here.
    if (force) {
        print("WARNING: " + risk.message + " (--force: proceeding)");
        return true;
    }
    print("WARNING: " + risk.message);
    if (interactive_) {
        std::cout << "Continue? [y/N] " << std::flush;
        bool yes = false;
        if (confirmHandler_) {
            yes = confirmHandler_("Continue? [y/N] ");
        } else {
            std::string line;
            if (std::getline(std::cin, line)) {
                std::string l = toLower(trim(line));
                yes = (l == "y" || l == "yes");
            }
        }
        if (!yes) {
            print("cancelled (state unchanged)");
            return false;
        }
        return true;
    }
    print("error: this operation requires confirmation (re-run with --force)");
    return false;
}

void Shell::printCausedBy(uint64_t rootId, const std::string& primary) {
    for (const auto& e : sys_.events().all()) {
        if (e.causeId == rootId && e.target != primary)
            std::cout << e.target << ": " << e.message << "\n";
    }
}

std::string Shell::svcUnknownHint(const std::string& name) const {
    // Services (ServiceManager) and processes (ProcessManager) are
    // intentionally separate registries: a process TYPE like "service" is
    // just a category label, not a managed service. Only this hint lives
    // here; the engine keeps returning plain "unknown service: <name>".
    if (sys_.processes().find(name) != nullptr)
        return " (note: a process named '" + name +
               "' exists, but services and processes are separate registries"
               " - see `help service`)";
    return "";
}

bool Shell::execLine(const std::string& line, bool recordHistory) {
    std::string t = trim(line);
    if (t.empty() || t.rfind("#", 0) == 0) return true;
    try {
        if (recordHistory) sys_.recordCommand(t);
        ParsedCommand pc = parse(t);
        if (pc.name.empty()) return true;
        bool cont = dispatch(pc);
        if (!cont) return false;
        if (sys_.mode() == "SIMULATION") {
            static const std::vector<std::string> noAuto = {"tick", "step",     "ff",   "pause",
                                                            "resume", "exit",  "quit", "mode",
                                                            "seed",   "clear", "help", "?"};
            if (std::find(noAuto.begin(), noAuto.end(), pc.name) == noAuto.end())
                sys_.autoTick();
        }
        if (sys_.mode() == "WATCH") {
            // WATCH mode: continuously display world state after world-changing
            // commands (SIMULATION stays the auto-advance mode).
            static const std::vector<std::string> watched = {
                "break", "kill", "fail", "repair", "fix", "heal", "override", "set",
                "inject", "deceive", "create", "spawn", "add", "remove", "delete", "destroy",
                "terminate", "connect", "link", "disconnect", "unlink", "depend", "tick",
                "step", "ff", "restore", "rewind", "checkpoint", "snapshot", "snap", "branch",
                "process", "ps", "chaos", "autofail", "race", "benchmark", "cool", "reboot",
                "start", "recover", "exec", "priv", "iface", "mkdir", "touch", "rm", "cp",
                "mv", "edit", "write", "corrupt", "restart", "service", "svc", "halt", "shutdown",
                "poweroff"};
            if (std::find(watched.begin(), watched.end(), pc.name) != watched.end())
                print(sys_.status());
        }
        if (sys_.mode() == "DEBUG") {
            const auto& evs = sys_.events().all();
            std::cout << "[debug] tick=" << sys_.clock().tickCount() << " events=" << evs.size();
            if (!evs.empty()) std::cout << " last=#" << evs.back().id << " " << evs.back().type;
            std::cout << "\n";
        }
        // Achievement evaluation runs on real post-command state (events +
        // world + observed reads). Presentation only: no ticks, no mutation.
        drainAchievements();
        return true;
    } catch (const std::exception& e) {
        // Last-resort guard: a malformed command must never kill the shell.
        print(std::string("error: ") + e.what());
        return true;
    } catch (...) {
        print("error: unknown failure (state unchanged)");
        return true;
    }
}

int Shell::runScript(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        print("cannot open script: " + path);
        return 1;
    }
    std::string line;
    while (std::getline(f, line)) {
        std::cout << prompt() << line << "\n";
        if (!execLine(line)) break;
    }
    return 0;
}

int Shell::runCommands(const std::string& batch) {
    std::string cur;
    for (char c : batch) {
        if (c == ';' || c == '\n') {
            if (!trim(cur).empty() && !execLine(cur)) return 0;
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!trim(cur).empty()) execLine(cur);
    return 0;
}

int Shell::runInteractive() {
    setInteractive(true);
    std::cout << "OVERRIDE v0.1.0 -- CONTROL THE SYSTEM. BREAK THE SYSTEM. UNDERSTAND THE SYSTEM.\n"
                 "Type `help` for commands. `exit` to quit.\n";
    std::string line;
    while (!exited_) {
        std::cout << prompt() << std::flush;
        if (!std::getline(std::cin, line)) {
            std::cout << "\n";
            break;
        }
        if (!execLine(line)) break;
    }
    return 0;
}

namespace {
// Human duration: "12m 08s", "3h 42m 19s", "2d 5h 17m 08s", "0s".
std::string fmtDur(int64_t total) {
    if (total < 0) total = 0;
    int64_t d = total / 86400, h = (total % 86400) / 3600, m = (total % 3600) / 60,
            s = total % 60;
    std::ostringstream o;
    bool any = false;
    if (d > 0) {
        o << d << "d ";
        any = true;
    }
    if (h > 0 || any) {
        o << h << "h ";
        any = true;
    }
    if (m > 0 || any) {
        o << m << "m ";
        any = true;
    }
    if (any) {
        o << std::setw(2) << std::setfill('0') << s << "s";
    } else {
        o << s << "s";
    }
    return o.str();
}
} // namespace

bool Shell::requireWorlds() {
    if (hasWorlds()) return true;
    print("error: world persistence not configured in this session");
    return false;
}

bool Shell::pristineSession() const {
    System fresh; // deterministic init: same seed, same ops, same state
    return fresh.worldDigest() == sys_.worldDigest() &&
           fresh.events().size() == sys_.events().size();
}

bool Shell::saveActive(std::string& err) {
    if (!hasWorlds()) {
        err = "world persistence not configured in this session";
        return false;
    }
    if (sys_.worldId().empty()) {
        std::string slot = worlds_->lowestFreeSlot();
        if (slot.empty()) {
            err = "maximum world limit reached (6/6)";
            return false;
        }
        sys_.setWorldId(slot);
        if (sys_.worldName() == "unnamed") sys_.setWorldName(slot);
        if (sys_.worldCreatedAt() == 0) sys_.setWorldCreatedAt(worlds_->now());
        worlds_->setActiveId(slot);
    }
    return worlds_->saveWorld(sys_, err);
}

bool Shell::createWorld() {
    if (!requireWorlds()) return true;
    // Never silently discard work: backed worlds save first; dirty unbacked
    // sessions must `save` explicitly before being replaced.
    if (!sys_.worldId().empty()) {
        std::string err;
        if (!saveActive(err)) {
            print("error: cannot save current world: " + err);
            return true;
        }
    } else if (!pristineSession()) {
        print("error: active session has unsaved changes (`save` first to keep it as a world)");
        return true;
    }
    std::string slot = worlds_->lowestFreeSlot();
    if (slot.empty()) {
        print("error: maximum world limit reached (6/6)");
        return true;
    }
    // Transactional: build into a scratch world; the live world is replaced
    // only after init + validation + save all succeed. No half-created files:
    // saveWorld writes tmp + renames only on success.
    System fresh;
    std::cout << "Creating world " << slot << "\n";
    ProgressOut bar(1);
    std::string perr;
    if (!fresh.initWorldPipeline(shellProgress(bar), perr)) {
        bar.finish();
        print("error: world initialization failed: " + perr);
        return true;
    }
    bar.finish();
    fresh.setWorldId(slot);
    fresh.setWorldName(slot);
    fresh.setWorldCreatedAt(worlds_->now());
    fresh.setWorldCreatedTick(0);
    fresh.checkpoint("init");
    sys_ = std::move(fresh);
    std::string serr;
    if (!saveActive(serr)) {
        print("error: world " + slot + " ready in memory but not saved: " + serr);
        return true;
    }
    worlds_->setActiveId(slot);
    sys_.beginUptimeSession(worlds_->now());
    print("World " + slot + " ready.");
    return true;
}

bool Shell::joinWorld(const std::string& id) {
    if (!requireWorlds()) return true;
    std::string low = toLower(id);
    if (!WorldManager::validSlotId(low)) {
        print("error: invalid world id '" + id + "' (w1..w6)");
        return true;
    }
    if (!worlds_->worldExists(low)) {
        print("error: world " + low + " does not exist (no " + low + ".ord)");
        return true;
    }
    if (low == worlds_->activeId()) {
        print("Active world: " + low + " (already joined)");
        return true;
    }
    if (!sys_.worldId().empty()) {
        std::cout << "Saving " << sys_.worldId() << "...\n";
        std::string err;
        if (!saveActive(err)) {
            print("error: cannot save current world (" + err + "); staying put");
            return true;
        }
        std::cout << "World " << sys_.worldId() << " saved.\n";
    } else if (!pristineSession()) {
        print("error: active session has unsaved changes (`save` first to keep it as a world)");
        return true;
    }
    System incoming;
    std::string err;
    std::cout << "Loading " << low << "...\n";
    if (!worlds_->loadWorld(incoming, low, err)) {
        // Current world untouched (load builds into scratch): stay put.
        print("error: " + err);
        return true;
    }
    sys_ = std::move(incoming);
    std::cout << "World " << low << " restored.\n";
    sys_.beginUptimeSession(worlds_->now());
    print("Active world: " + low);
    return true;
}

bool Shell::resetWorld(bool force) {
    if (!requireWorlds()) return true;
    if (sys_.worldId().empty()) {
        print("error: no active world to reset (create w)");
        return true;
    }
    if (!riskGate("reset", {sys_.worldId()}, force)) return true;
    // Reset keeps identity, name, note, creation time, and all-time uptime;
    // only simulation state returns to fresh. Elapsed session time is folded
    // into the total first so uptime is never silently erased.
    int64_t now = worlds_->now();
    sys_.commitUptimeSession(now);
    System fresh;
    ProgressOut bar(1);
    std::string perr;
    if (!fresh.initWorldPipeline(shellProgress(bar), perr)) {
        bar.finish();
        print("error: reset initialization failed: " + perr);
        return true;
    }
    bar.finish();
    fresh.setWorldId(sys_.worldId());
    fresh.setWorldName(sys_.worldName());
    fresh.setWorldNote(sys_.worldNote());
    fresh.setWorldCreatedAt(sys_.worldCreatedAt());
    fresh.setWorldCreatedTick(sys_.worldCreatedTick());
    fresh.setUptimeTotalSecs(sys_.allTimeUptimeSecs(now)); // session folded above
    fresh.checkpoint("init");
    sys_ = std::move(fresh);
    std::string err;
    if (!saveActive(err)) {
        print("error: world reset but not saved: " + err);
        return true;
    }
    sys_.beginUptimeSession(worlds_->now());
    print("World " + sys_.worldId() + " reset to a clean simulation state.");
    return true;
}

bool Shell::deleteWorld(const std::string& id, bool force) {
    if (!requireWorlds()) return true;
    std::string low = toLower(id);
    if (!WorldManager::validSlotId(low)) {
        print("error: invalid world id '" + id + "' (w1..w6)");
        return true;
    }
    if (!worlds_->worldExists(low)) {
        print("error: world " + low + " does not exist");
        return true;
    }
    // Deleting a world is permanent: typed DELETE in interactive shells,
    // --force headless. Never silent. Achievement file untouched by design.
    bool confirmed = force;
    if (!confirmed && interactive_) {
        std::cout << "This permanently removes world " + low + ".\nType DELETE " + low +
                         " to confirm: "
                  << std::flush;
        if (confirmHandler_) {
            confirmed = confirmHandler_("Type DELETE " + low + " to confirm: ");
        } else {
            std::string line;
            if (std::getline(std::cin, line)) confirmed = (trim(line) == "DELETE " + low);
        }
    }
    if (!confirmed) {
        if (!interactive_)
            print("error: deleting a world requires confirmation (re-run with --force)");
        else
            print("cancelled (state unchanged)");
        return true;
    }
    bool wasActive = (worlds_->activeId() == low || sys_.worldId() == low);
    std::string err;
    if (!worlds_->deleteWorld(low, err)) {
        print("error: " + err);
        return true;
    }
    print("World " + low + " deleted.");
    if (wasActive) {
        // Fall back to the lowest remaining world, else a fresh session.
        auto rest = worlds_->discover();
        if (!rest.empty()) {
            joinWorld(rest.front());
        } else {
            System fresh;
            sys_ = std::move(fresh); // pristine unbacked session, no timer yet
            print("No worlds remain: fresh session (create w).");
        }
    }
    return true;
}

void Shell::showWorlds() const {
    if (!hasWorlds()) {
        print("error: world persistence not configured in this session");
        return;
    }
    std::ostringstream o;
    o << "worlds (" << worlds_->appDir() << "):\n";
    bool any = false;
    for (int i = 1; i <= WorldManager::kMaxWorlds; ++i) {
        std::string id = "w" + std::to_string(i);
        if (!worlds_->worldExists(id)) continue;
        any = true;
        std::string name;
        uint64_t total = 0;
        std::string err;
        std::string info;
        if (worlds_->worldInfo(id, name, total, err)) {
            info = name + "  all-time " + fmtDur((int64_t)total);
        } else {
            info = "(unreadable: " + err + ")";
        }
        o << "  " << id << (worlds_->activeId() == id ? " *ACTIVE*" : "") << "  " << info
          << "\n";
    }
    if (!any) o << "  (none yet -- create w)\n";
    print(o.str());
}

void Shell::showWorld() const {
    if (!hasWorlds()) {
        print("error: world persistence not configured in this session");
        return;
    }
    if (sys_.worldId().empty()) {
        print("No active world (unbacked session -- create w).");
        return;
    }
    int64_t now = worlds_->now();
    std::ostringstream o;
    o << "World: " << sys_.worldId() << " (" << sys_.worldName() << ")\n";
    if (!sys_.worldNote().empty()) o << "Note: " << sys_.worldNote() << "\n";
    o << "Session uptime: " << fmtDur(sys_.sessionUptimeSecs(now)) << "\n";
    o << "All-time uptime: " << fmtDur((int64_t)sys_.allTimeUptimeSecs(now)) << "\n";
    o << "Checkpoints: " << sys_.history().snapshots().size()
      << "  Events: " << sys_.events().size() << "\n";
    print(o.str());
}

void Shell::uptCmd(const std::vector<std::string>& args) const {
    // upt | uptime [-at] [-h]: session vs cumulative active lifetime.
    // Pause does NOT stop uptime: it measures world active presence, not
    // simulation tick advancement (documented in `upt -h`).
    bool allTime = false;
    for (const auto& x : args) {
        std::string l = toLower(x);
        if (l == "-at" || l == "--all-time" || l == "all") allTime = true;
        else if (l == "-h" || l == "--help" || l == "help") {
            print("upt | uptime [-at]: world uptime.\n"
                  "  upt:      current session uptime (since create w / joinw).\n"
                  "  upt -at:  cumulative active lifetime across sessions (persisted).\n"
                  "  uptime measures world active presence: pause does not stop it,\n"
                  "  and checkpoint/restore/rewind never roll it back. reset keeps it;\n"
                  "  deleting the world deletes its history with it.");
            return;
        } else {
            print("usage: upt [-at]  (upt -h for details)");
            return;
        }
    }
    if (!hasWorlds()) {
        print("error: world persistence not configured in this session");
        return;
    }
    if (sys_.worldId().empty()) {
        print("error: no active world");
        return;
    }
    int64_t now = worlds_->now();
    std::ostringstream o;
    o << "World: " << sys_.worldId() << "\n";
    if (allTime) {
        o << "All-time uptime: " << fmtDur((int64_t)sys_.allTimeUptimeSecs(now));
    } else {
        o << "Uptime: " << fmtDur(sys_.sessionUptimeSecs(now));
    }
    print(o.str());
}

bool Shell::persistOnExit(std::string& msg) {
    if (!hasWorlds()) {
        msg = "";
        return true;
    }
    // Backed worlds save (committing the session). Dirty unbacked sessions
    // claim the lowest free slot so work is never silently lost; pristine
    // sessions persist nothing (no placeholder files, ever).
    if (!sys_.worldId().empty() || !pristineSession()) {
        std::string err;
        if (!saveActive(err)) {
            msg = "exit: could not persist world (" + err + ")";
            return false;
        }
        persistAchievements();
        msg = "exit: world " + sys_.worldId() + " saved.";
        return true;
    }
    msg = "";
    return true;
}

void Shell::noteFsArea(const std::string& path) {
    // Map a virtual path to its substrate area (boot, kernel, bin, ...).
    std::string abs = Vfs::normalize(sys_.cwd(), path);
    if (abs.empty() || abs == "/") return;
    auto top = abs.substr(1);
    auto slash = top.find('/');
    if (slash != std::string::npos) top.resize(slash);
    if (top == "bin" || top == "sbin") observed_.fsAreas.insert("bin");
    else if (top == "home" || top == "root") observed_.fsAreas.insert("home");
    else if (top == "boot" || top == "kernel" || top == "lib" || top == "dev" ||
             top == "proc" || top == "sys" || top == "run" || top == "etc" || top == "var" ||
             top == "tmp" || top == "usr")
        observed_.fsAreas.insert(top);
}

void Shell::showUnlock(const std::string& id) {
    std::string box = achievements_.renderNotification(id);
    lastNotification_ = box; // raw text kept for tests; colors never leak in
    std::cout << color::paintAchievements(box) << std::flush; // stdout, never print()
    if (!interactive_) return;                                // headless: no sleeps, no spam beyond the box
    // ~3s wall-clock display, then erase the box and let the prompt loop
    // redraw. Presentation only: sim time never advances (no ticks here).
    std::this_thread::sleep_for(std::chrono::seconds(3));
    int lines = 1;
    for (char c : box)
        if (c == '\n') ++lines;
    std::cout << "\x1b[" << lines << "A\x1b[J" << std::flush;
}

void Shell::drainAchievements() {
    auto fresh = achievements_.evaluate(sys_, observed_);
    if (fresh.empty()) return;
    persistAchievements();
    for (const auto& id : achievements_.drainNotifications()) showUnlock(id);
}

void Shell::persistAchievements() {
    if (!hasWorlds()) return; // System-mode sessions keep meta in memory only
    std::string err;
    worlds_->saveAchievements(achievements_, err); // best effort; worlds save covers exit
}

std::string Shell::prompt() const {
    // Simulated identities only: root shows [root@override]#, everyone
    // else [<account>@override]$ . Updates immediately with operator state.
    if (sys_.opRoot()) return "[root@override]# ";
    std::string user = sys_.opUser().empty() ? "user" : sys_.opUser();
    return "[" + user + "@override]$ ";
}

Shell::PasswordSource Shell::passwordSource(bool hasReader, bool interactiveFlow) {
    if (hasReader) return PasswordSource::Injected;
    if (interactiveFlow) return PasswordSource::Console;
    return PasswordSource::FailClosed;
}

std::string Shell::readPassword(const std::string& promptText, bool interactive) {
    switch (passwordSource(passwordReader_ != nullptr, interactive)) {
        case PasswordSource::Injected: return passwordReader_(promptText);
        case PasswordSource::FailClosed: return ""; // headless: no silent auth
        case PasswordSource::Console: break;
    }
#if defined(_WIN32)
    std::cout << promptText << std::flush;
    std::string out;
    int failures = 0;
    for (;;) {
        int ch = _getch();
        if (ch == '\r' || ch == '\n') break;
        if (ch < 0) {
            // No console to read from (e.g. fully headless): fail closed
            // instead of spinning.
            if (++failures > 3) {
                std::cout << "\n" << std::flush;
                return "";
            }
            continue;
        }
        failures = 0;
        if ((ch == '\b' || ch == 127) && !out.empty()) {
            out.pop_back();
            std::cout << "\b \b" << std::flush;
        } else if (ch >= 32 && ch < 127) {
            out += (char)ch;
            std::cout << '*' << std::flush;
        }
    }
    std::cout << "\n" << std::flush;
    return out;
#else
    termios saved{}, raw{};
    bool haveTty = (tcgetattr(STDIN_FILENO, &saved) == 0);
    std::cout << promptText << std::flush;
    std::string out;
    if (haveTty) {
        raw = saved;
        raw.c_lflag &= (unsigned)(~(ICANON | ECHO));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    for (;;) {
        int ch = getchar();
        if (ch == '\n' || ch == '\r' || ch == EOF) break;
        if ((ch == 127 || ch == '\b') && !out.empty()) {
            out.pop_back();
            std::cout << "\b \b" << std::flush;
        } else if (ch >= 32 && ch < 127) {
            out += (char)ch;
            std::cout << '*' << std::flush;
        }
    }
    if (haveTty) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    std::cout << "\n" << std::flush;
    return out;
#endif
}

bool Shell::ensureAccount(bool interactive, std::string& msg) {
    // No world manager (System-mode tests): identity stays as constructed.
    if (!hasWorlds()) {
        msg = "";
        return true;
    }
    std::string err;
    if (worlds_->loadAccount(worlds_->account(), err)) {
        // Returning operator: privilege never resumes across launches.
        sys_.setOpUser(worlds_->account().username);
        sys_.setOpRoot(false);
        msg = "";
        return true;
    }
    bool missing = (err.find("no account configured") != std::string::npos);
    if (!interactive) {
        // Headless/scripted runs without an account proceed as simulated
        // root (pre-account behavior) with a notice; nothing is created.
        msg = "notice: " + err + " (proceeding as simulated root; register interactively)";
        return true;
    }
    if (!missing) {
        std::cout << "warning: " << err << " (re-registering account)\n";
    }
    // First-run registration, exactly as specified. Username echoed;
    // password masked with asterisks, never printed or stored in clear.
    std::string username;
    for (;;) {
        std::cout << "Create a override username: " << std::flush;
        if (!std::getline(std::cin, username)) {
            msg = "account creation aborted";
            return false;
        }
        username = trim(username);
        std::string uerr;
        if (username.empty()) {
            std::cout << "username must not be empty.\n";
            continue;
        }
        if (!validAccountName(username)) {
            std::cout << "invalid username (use [a-z][a-z0-9_-]*, max 32).\n";
            continue;
        }
        break;
    }
    std::string password = readPassword("Create a override password: ", interactive);
    std::string cerr;
    AccountStore acc = AccountStore::create(username, password, cerr);
    if (!acc.configured()) {
        msg = "error: " + cerr;
        return false;
    }
    if (!worlds_->saveAccount(acc, cerr)) {
        msg = "error: cannot persist account (" + cerr + ")";
        return false;
    }
    // The session must see the new account immediately (sudo etc.), not
    // only after a restart: the file and the live store update together.
    worlds_->account() = acc;
    sys_.setOpUser(username);
    sys_.setOpRoot(false);
    msg = "";
    return true;
}

bool Shell::runAchievementViewer() {
    size_t total = 0;
    (void)achievements_.defs(total);
    if (!interactive_) {
        // Headless/script/test: full list, no clearing, no key handling.
        std::ostringstream o;
        int pages = achievementPageCount(total, 10);
        for (int p = 0; p < pages; ++p) o << achievements_.renderPage(p, 10);
        print(color::paintAchievements(o.str()));
        return true;
    }
    std::cout << "\x1b[2J\x1b[H" << std::flush; // enter achievement mode
    AchievementNav nav{0, achievementPageCount(total, 10)};
    for (;;) {
        std::cout << color::paintAchievements(achievements_.renderPage(nav.page, 10))
                  << std::flush;
        int code = -1;
#if defined(_WIN32)
        int ch = _getch();
        if (ch == 0 || ch == 224) {
            int ext = _getch();
            code = ext; // 75 left, 77 right (prefix context disambiguates)
        } else {
            code = ch;
        }
#else
        // POSIX raw mode: arrows arrive as ESC [ D / C.
        termios saved{}, raw{};
        bool haveTty = (tcgetattr(STDIN_FILENO, &saved) == 0);
        if (haveTty) {
            raw = saved;
            raw.c_lflag &= (unsigned)(~(ICANON | ECHO));
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
        int ch = getchar();
        if (ch == 27) {
            int b1 = getchar(), b2 = getchar();
            code = (b1 == '[') ? b2 : -1; // D left, C right
        } else {
            code = ch;
        }
        if (haveTty) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
#endif
        NavAction act = viewerKey(nav, code);
        if (act == NavAction::Exit) break;
        std::cout << "\x1b[2J\x1b[H" << std::flush;
    }
    return true;
}

void Shell::runGetLoad(const std::string& sub, const std::vector<std::string>& args,
                       const std::string& usagePrefix) {
    // Shared get/load implementation (single copy for `sudo get|load` and
    // bare root-mode `get|load`). get fetches into the package store (never
    // the world, never the ledger); load applies verified content to the
    // active world transactionally. No SUDO_* audit lines here by design.
    if (args.size() != 1) {
        print("usage: " + usagePrefix + sub + " <kernel|rootfs|kernel,rootfs>");
        return;
    }
    std::vector<std::string> targets;
    {
        std::string cur;
        for (char ch : args[0]) {
            if (ch == ',') {
                if (!cur.empty()) targets.push_back(toLower(cur));
                cur.clear();
            } else {
                cur += ch;
            }
        }
        if (!cur.empty()) targets.push_back(toLower(cur));
    }
    for (const auto& t : targets) {
        if (t != "kernel" && t != "rootfs") {
            print("error: unknown recovery target '" + t + "' (kernel|rootfs)");
            return;
        }
    }
    if (targets.empty()) {
        print("usage: " + usagePrefix + sub + " <kernel|rootfs|kernel,rootfs>");
        return;
    }
    PackageManager& pm = worlds_->packages();
    if (sub == "get") {
        // Pre-check validity so fresh fetches print progress headers
        // while already-available packages report exactly that.
        auto validNow = [&](const std::string& t) {
            std::string dummy;
            if (t == "kernel") {
                std::string bytes;
                return pm.readKernel(bytes, dummy);
            }
            RootfsContent rc;
            return pm.readRootfs(rc, dummy);
        };
        bool anyFetch = false;
        for (const auto& t : targets) {
            bool avail = (t == "kernel") ? pm.kernelAvailable() : pm.rootfsAvailable();
            if (!avail || !validNow(t)) {
                anyFetch = true;
                break;
            }
        }
        if (anyFetch && targets.size() > 1)
            std::cout << "Fetching simulated recovery packages...\n";
        for (const auto& t : targets) {
            bool created = false;
            std::string msg, err;
            bool ok;
            uint64_t now = (uint64_t)worlds_->now();
            if (t == "kernel") {
                if (anyFetch && targets.size() == 1)
                    std::cout << "Fetching simulated kernel package...\n";
                ok = pm.ensureKernel(created, msg, err, now);
            } else {
                if (anyFetch && targets.size() == 1)
                    std::cout << "Fetching simulated rootfs package...\n";
                ProgressOut bar;
                ok = pm.ensureRootfs(sys_.nodeNames(), created, msg, err, now,
                                     shellProgress(bar));
                bar.finish();
            }
            if (!ok) print("error: " + err);
            else print(msg);
        }
        return;
    }
    // load: presence first (missing => hint, zero footprint), then validate
    // everything (invalid => zero footprint), then apply transactionally in
    // order. Apply-time quota failures unwind via the engine journal.
    for (const auto& t : targets) {
        bool avail = (t == "kernel") ? pm.kernelAvailable() : pm.rootfsAvailable();
        if (!avail) {
            print("error: " + t + " recovery package is not available\nhint: run `sudo get " +
                  t + "` first");
            return;
        }
    }
    std::string kbytes;
    RootfsContent rcontent;
    bool needK = false, needR = false;
    for (const auto& t : targets) {
        if (t == "kernel") needK = true;
        else needR = true;
    }
    std::string verr;
    if (needK && !pm.readKernel(kbytes, verr)) {
        print("error: recovery package validation failed\nreason: " + verr);
        return;
    }
    if (needR && !pm.readRootfs(rcontent, verr)) {
        print("error: recovery package validation failed\nreason: " + verr);
        return;
    }
    for (const auto& t : targets) {
        if (t == "kernel") {
            std::cout << "Loading kernel package...\n";
            OpResult r = sys_.loadKernelImage(kbytes);
            if (!r.ok) print("error: " + r.error);
            else print(r.info);
        } else {
            std::cout << "Loading rootfs package...\n";
            ProgressOut bar;
            OpResult r = sys_.loadRootfsBaseline(rcontent, shellProgress(bar));
            bar.finish();
            if (!r.ok) print("error: " + r.error);
            else print(r.info);
        }
    }
}

bool Shell::dispatch(const ParsedCommand& cmd) {
    const std::string& c = cmd.name;
    const auto& a = cmd.args;

    if (c == "exit" || c == "quit") {
        exited_ = true;
        return false;
    }
    if (c == "clear") {
        std::cout << "\x1b[2J\x1b[H" << std::flush;
        return true;
    }
    if (c == "help" || c == "?") {
        help(a.empty() ? "" : a[0]);
        return true;
    }
    if (c == "status") {
        print(sys_.status());
        return true;
    }
    if (c == "list") {
        std::ostringstream o;
        o << "nodes:\n";
        for (auto& n : sys_.nodeNames()) {
            const Node& node = sys_.get(n);
            o << "  " << node.name << " [" << node.type << "] " << toString(node.state)
              << " health=" << node.health << " lat=" << node.latencyMs << "ms\n";
        }
        o << "links: " << sys_.network().links().size() << "\n";
        print(o.str());
        return true;
    }
    if (c == "inspect" || c == "show" || c == "watch") {
        if (a.empty()) {
            if (c == "watch") {
                print(sys_.status());
                return true;
            }
            print("usage: inspect <node>");
            return true;
        }
        std::string target = toLower(a[0]);
        if (target == "system" || target == "all") {
            std::ostringstream o;
            o << "system " << sys_.clock().now() << " mode=" << sys_.mode()
              << " seed=" << sys_.seed() << "\n";
            o << sys_.status();
            observed_.subsystems.insert("nodes");
            print(o.str());
            return true;
        }
        observed_.subsystems.insert("nodes");
        print(sys_.inspect(target));
        return true;
    }
    if (c == "break" || c == "kill" || c == "fail") {
        if (!guard(Op::MUTATE, "break")) return true;
        ForceArgs fa = stripForce(a);
        if (fa.args.empty()) {
            print("usage: break <node>");
            return true;
        }
        if (!riskGate("break", fa.args, fa.force)) return true;
        std::string target = toLower(fa.args[0]);
        OpResult r = sys_.breakNode(target);
        if (!r.ok) {
            print("error: " + r.error + (r.eventId ? " (#" + std::to_string(r.eventId) + ")" : ""));
            return true;
        }
        print(r.info + " (#" + std::to_string(r.eventId) + ")");
        printCausedBy(r.eventId, target);
        return true;
    }
    if (c == "repair" || c == "fix" || c == "heal") {
        if (!guard(Op::MUTATE, "repair")) return true;
        if (a.empty()) {
            print("usage: repair <node>");
            return true;
        }
        std::string target = toLower(a[0]);
        OpResult r = sys_.repairNode(target);
        if (!r.ok) {
            print("error: " + r.error);
            return true;
        }
        print(r.info + " (#" + std::to_string(r.eventId) + ")");
        printCausedBy(r.eventId, target);
        return true;
    }
    if (c == "override" || c == "set") {
        if (!guard(Op::MUTATE, "override")) return true;
        if (a.size() < 3) {
            print("usage: override <node> <property> <value>");
            return true;
        }
        std::string target = toLower(a[0]);
        std::string prop = a[1];
        std::string pl = toLower(prop);
        if ((pl == "state" || pl == "proc") && sys_.mode() != "OVERRIDE" &&
            !isReservedMode(sys_.mode())) {
            print("error: overriding '" + prop + "' requires OVERRIDE mode (`mode override`).");
            return true;
        }
        std::string val = join(a, 2);
        OpResult r = sys_.overrideProp(target, prop, val);
        if (!r.ok) {
            print("error: " + r.error);
            return true;
        }
        print(r.info);
        printCausedBy(r.eventId, target);
        return true;
    }
    if (c == "inject") {
        if (!guard(Op::MUTATE, "inject")) return true;
        ForceArgs fa = stripForce(a);
        const std::vector<std::string>& ia = fa.args;
        if (ia.size() < 2) {
            print("usage: inject <node> <fault> [value]  (or: inject <fault> <node> [value])");
            return true;
        }
        if (!riskGate("inject", ia, fa.force)) return true;
        // Accept both `inject <node> <fault>` (legacy) and `inject <fault> <node>`
        // (chaos-scenario style). Node-first wins when ambiguous.
        std::string target, fault, val;
        std::string x0 = toLower(ia[0]), x1 = toLower(ia[1]);
        bool x0node = sys_.hasNode(x0), x1node = sys_.hasNode(x1);
        bool x0fault = faultDef(x0) != nullptr || isLegacyFault(x0);
        bool x1fault = faultDef(x1) != nullptr || isLegacyFault(x1);
        if (x0node && x1fault) {
            target = x0;
            fault = ia[1];
            val = ia.size() > 2 ? join(ia, 2) : "";
        } else if (x0fault && x1node) {
            fault = ia[0];
            target = x1;
            val = ia.size() > 2 ? join(ia, 2) : "";
        } else if (x0node) {
            target = x0;
            fault = ia[1];
            val = ia.size() > 2 ? join(ia, 2) : "";
        } else {
            // Neither side resolves: report the most likely problem first.
            if (!x1node && !x1fault) {
                print("error: unknown node: " + ia[1] + " (and '" + ia[0] +
                      "' is not a known fault)");
                return true;
            }
            target = x0;
            fault = ia[1];
            val = ia.size() > 2 ? join(ia, 2) : "";
        }
        OpResult r = sys_.injectFault(target, fault, val);
        if (!r.ok) {
            print("error: " + r.error);
            return true;
        }
        print(r.info);
        printCausedBy(r.eventId, target);
        return true;
    }
    if (c == "cool") {
        if (!guard(Op::MUTATE, "cool")) return true;
        if (a.empty()) {
            print("usage: cool <node>  (thermal recovery only)");
            return true;
        }
        std::string target = toLower(a[0]);
        OpResult r = sys_.coolNode(target);
        if (!r.ok) print("error: " + r.error);
        else {
            print(r.info);
            printCausedBy(r.eventId, target);
        }
        return true;
    }
    if (c == "reboot") {
        if (!guard(Op::MUTATE, "reboot")) return true;
        ForceArgs fa = stripForce(a);
        if (fa.args.empty()) {
            print("usage: reboot <node> [--force]  (clears kernel/thermal/clock; keeps disk/fs/config)");
            return true;
        }
        if (!riskGate("reboot", fa.args, fa.force)) return true;
        std::string target = toLower(fa.args[0]);
        ProgressOut bar;
        OpResult r = sys_.rebootNode(target, shellProgress(bar));
        bar.finish();
        if (!r.ok) print("error: " + r.error);
        else {
            print(r.info);
            printCausedBy(r.eventId, target);
        }
        return true;
    }
    if (c == "halt" || c == "shutdown" || c == "poweroff") {
        if (!guard(Op::MUTATE, "halt")) return true;
        ForceArgs fa = stripForce(a);
        if (fa.args.empty()) {
            print("usage: halt <node> [--force]  (orderly shutdown: kernel OFF, node PAUSED)");
            return true;
        }
        if (!riskGate("halt", fa.args, fa.force)) return true;
        std::string target = toLower(fa.args[0]);
        OpResult r = sys_.haltNode(target);
        if (!r.ok) print("error: " + r.error);
        else {
            print(r.info);
            printCausedBy(r.eventId, target);
        }
        return true;
    }
    if (c == "start") {
        if (!guard(Op::MUTATE, "start")) return true;
        ForceArgs fa = stripForce(a);
        if (fa.args.empty()) {
            print("usage: start <node>  (reboot if kernel dead, else repair)");
            return true;
        }
        std::string target = toLower(fa.args[0]);
        // A start that must (re)boot a dead/off kernel carries reboot's risk.
        if (sys_.hasNode(target)) {
            const Node& sn = sys_.get(target);
            if (sn.kernel == KernelState::PANICKED || sn.kernel == KernelState::HALTED ||
                sn.kernel == KernelState::OFF) {
                if (!riskGate("reboot", fa.args, fa.force)) return true;
            }
        }
        ProgressOut bar;
        OpResult r = sys_.startNode(target, shellProgress(bar));
        bar.finish();
        if (!r.ok) print("error: " + r.error);
        else {
            print(r.info);
            printCausedBy(r.eventId, target);
        }
        return true;
    }
    if (c == "recover" || c == "fix-fault" || c == "resolve") {
        if (!guard(Op::MUTATE, "recover")) return true;
        if (a.size() < 2) {
            print("usage: recover <node> <fault>  (see `help inject` for fault names)");
            return true;
        }
        OpResult r = sys_.resolveFault(toLower(a[0]), toLower(a[1]));
        if (!r.ok) print("error: " + r.error);
        else {
            print(r.info);
            printCausedBy(r.eventId, toLower(a[0]));
        }
        return true;
    }
    if (c == "exec") {
        if (!guard(Op::MUTATE, "exec")) return true;
        if (a.size() < 2) {
            print("usage: exec <node> <binary> [args...]  (simulated execution)");
            return true;
        }
        std::string target = toLower(a[0]);
        std::string bin = toLower(a[1]);
        std::string args = a.size() > 2 ? join(a, 2) : "";
        OpResult r = sys_.runBinary(target, bin, args);
        if (!r.ok) print("error: " + r.error);
        else print(r.info);
        return true;
    }
    if (c == "priv") {
        if (a.empty()) {
            print("usage: priv <node> [ROOT|USER|LOCKED|RESTRICTED|CORRUPTED]");
            return true;
        }
        std::string target = toLower(a[0]);
        if (a.size() == 1) {
            if (!sys_.hasNode(target)) {
                print("error: unknown node: " + target);
                return true;
            }
            print(target + " privileges: " + sys_.effectiveProp("", target, "priv"));
            return true;
        }
        if (!guard(Op::MUTATE, "priv")) return true;
        OpResult r = sys_.setPriv(target, a[1]);
        if (!r.ok) print("error: " + r.error);
        else print(r.info);
        return true;
    }
    if (c == "get" || c == "load") {
        // Privileged recovery operations in root mode: the same
        // implementation as `sudo get|load` (rewritten below and
        // re-dispatched, exactly like the `crt` alias). Non-root sessions
        // are refused here; `sudo <cmd>` elevates one-shot instead.
        if (!sys_.opRoot()) {
            print("error: root privileges required");
            return true;
        }
        ParsedCommand sudo2;
        sudo2.name = "sudo";
        sudo2.args = std::vector<std::string>{c};
        sudo2.args.insert(sudo2.args.end(), a.begin(), a.end());
        sudo2.raw = "sudo " + join(a, 0);
        return dispatch(sudo2);
    }
    if (c == "sudo") {
        // Simulated sudo: session operator identity only, never host admin.
        if (!hasWorlds() || !worlds_->account().configured()) {
            print("error: no OVERRIDE account configured");
            return true;
        }
        AccountStore& acc = worlds_->account();
        std::string sub = a.empty() ? "" : toLower(a[0]);
        auto sudoEvent = [&](const std::string& type, const std::string& message,
                             const std::string& sev) {
            uint64_t root = sys_.emit(0, "USER_SUDO", "user", "system",
                                      "sudo " + (sub.empty() ? "" : sub));
            sys_.emit(root, type, "user", "system", message, {}, sev);
        };
        if (sub == "-v" || sub == "--version" || sub == "version") {
            print("OVERRIDE sudo 0.1.0 (simulated; host untouched)");
            return true;
        }
        if (sub == "-st" || sub == "status") {
            std::ostringstream o;
            o << "sudo status\nInstalled: " << (acc.sudoInstalled ? "yes" : "no") << "\n";
            o << "Active: " << (sys_.opRoot() ? "yes" : "no") << "\n";
            o << "User: " << acc.username << "\n";
            o << "Root: " << (sys_.opRoot() ? "active" : "inactive") << "\n";
            print(o.str());
            return true;
        }
        if (sub == "-i" || sub == "install") {
            if (!guard(Op::MUTATE, "sudo")) return true;
            if (acc.sudoInstalled) {
                print("sudo already installed for " + acc.username + " (simulated)");
                return true;
            }
                    std::string pw = readPassword("Password: ", interactive_);
            if (!acc.verifyPassword(pw)) {
                sudoEvent("SUDO_DENIED", "sudo install denied: authentication failure",
                          "WARNING");
                print("error: authentication failure");
                return true;
            }
            acc.sudoInstalled = true;
            std::string err;
            if (!worlds_->saveAccount(acc, err)) {
                acc.sudoInstalled = false;
                print("error: cannot persist sudo state (" + err + ")");
                return true;
            }
            sudoEvent("SUDO_OK", "sudo installed for " + acc.username, "INFO");
            print("sudo installed for " + acc.username + " (simulated; host untouched)");
            return true;
        }
        if (sub == "-a" || sub == "activate") {
            if (!guard(Op::MUTATE, "sudo")) return true;
            if (!acc.sudoInstalled) {
                print("error: sudo not installed (sudo -i first)");
                return true;
            }
            if (sys_.opRoot()) {
                print("already in root mode ([root@override]#)");
                return true;
            }
                    std::string pw = readPassword("Password: ", interactive_);
            if (!acc.verifyPassword(pw)) {
                sudoEvent("SUDO_DENIED", "sudo -a denied: authentication failure", "WARNING");
                print("error: authentication failure");
                return true;
            }
            sys_.setOpRoot(true);
            sudoEvent("SUDO_OK", "root mode activated by " + acc.username, "INFO");
            print("root mode active ([root@override]#)");
            return true;
        }
        if (sub == "-e" || sub == "exit" || sub == "deactivate") {
            if (!sys_.opRoot()) {
                print("error: not in root mode");
                return true;
            }
            sys_.setOpRoot(false);
            sudoEvent("SUDO_OK", "root mode exited by " + acc.username, "INFO");
            print("root mode exited ([" + acc.username + "@override]$)");
            return true;
        }
        if (sub == "get" || sub == "load") {
            // Privileged recovery ops through the standard one-shot sudo
            // mechanism (same as every other `sudo <cmd>`): authenticate
            // unless already root, execute, restore the session. get never
            // touches world/ledger; load applies verified content with its
            // own PACKAGE events, so no SUDO_OK audit line is emitted here.
            if (!guard(Op::MUTATE, "sudo")) return true;
            bool wasRoot = sys_.opRoot();
            if (!wasRoot) {
                if (!acc.sudoInstalled) {
                    print("error: sudo not installed (sudo -i first)");
                    return true;
                }
                std::string pw = readPassword("Password: ", interactive_);
                if (!acc.verifyPassword(pw)) {
                    sudoEvent("SUDO_DENIED", "sudo command denied: authentication failure",
                              "WARNING");
                    print("error: authentication failure");
                    return true;
                }
                sys_.setOpRoot(true);
            }
            runGetLoad(sub, std::vector<std::string>(a.begin() + 1, a.end()), "sudo ");
            if (!wasRoot) sys_.setOpRoot(false);
            return true;
        }

        if (!sub.empty() && sub[0] == '-') {
            print("error: unknown sudo option '" + a[0] +
                  "' (sudo -i|-v|-a|-e|-st | sudo <cmd>)");
            return true;
        }
        if (a.empty()) {
            print("usage: sudo -i | sudo -v | sudo -a | sudo -e | sudo -st | sudo <cmd>");
            return true;
        }
        // sudo <cmd>: one command with root permissions, session unchanged.
        if (!guard(Op::MUTATE, "sudo")) return true;
        bool wasRoot = sys_.opRoot();
        if (!wasRoot) {
            if (!acc.sudoInstalled) {
                print("error: sudo not installed (sudo -i first)");
                return true;
            }
                    std::string pw = readPassword("Password: ", interactive_);
            if (!acc.verifyPassword(pw)) {
                sudoEvent("SUDO_DENIED", "sudo command denied: authentication failure",
                          "WARNING");
                print("error: authentication failure");
                return true;
            }
            sys_.setOpRoot(true);
        }
        ParsedCommand sub2;
        sub2.name = toLower(a[0]);
        sub2.args = std::vector<std::string>(a.begin() + 1, a.end());
        sub2.raw = join(a, 0);
        sudoEvent("SUDO_OK",
                  "root single-command by " + acc.username + ": " + sub2.raw, "INFO");
        bool cont = dispatch(sub2);
        if (!wasRoot) sys_.setOpRoot(false);
        return cont;
    }
    if (c == "chmod") {
        if (a.size() < 2) {
            print("usage: chmod [+/-]<ur|uw|or|ow|rtr|rtw> <path>");
            return true;
        }
        if (!guard(Op::MUTATE, "chmod")) return true;
        std::string mod = a[0];
        bool add = true;
        if (!mod.empty() && (mod[0] == '+' || mod[0] == '-')) {
            add = (mod[0] == '+');
            mod = mod.substr(1);
        } else {
            print("usage: chmod [+/-]<ur|uw|or|ow|rtr|rtw> <path>");
            return true;
        }
        if (!riskGate("chmod", {mod, a[1]}, false)) return true;
        OpResult r = sys_.vfsChmod(a[1], add, toLower(mod));
        if (!r.ok) print("error: " + r.error);
        else print(r.info);
        return true;
    }
    if (c == "iface" || c == "interface") {
        if (!guard(Op::MUTATE, "iface")) return true;
        if (a.size() < 3) {
            print("usage: iface <node> <peer> <up|down>");
            return true;
        }
        std::string st = toLower(a[2]);
        if (st != "up" && st != "down") {
            print("error: state must be up|down, got '" + a[2] + "'");
            return true;
        }
        if (st == "down" && !riskGate("iface-down", {toLower(a[0]), toLower(a[1])}, false))
            return true;
        OpResult r = sys_.ifaceSet(toLower(a[0]), toLower(a[1]), st == "up");
        if (!r.ok) print("error: " + r.error);
        else {
            print(r.info + ".");
            printCausedBy(r.eventId, toLower(a[0]));
        }
        return true;
    }
    if (c == "world") {
        observed_.subsystems.insert("system");
        print(sys_.worldHealth());
        return true;
    }
    if (c == "faults") {
        observed_.subsystems.insert("nodes");
        if (a.empty()) {
            std::ostringstream o;
            bool any = false;
            for (auto& n : sys_.nodeNames()) {
                const Node& node = sys_.get(n);
                for (const auto& [fname, f] : node.faults) {
                    o << n << "  " << fname << " [" << toString(f.kind) << "/"
                      << toString(f.severity) << "] since t=" << f.sinceTick << "\n";
                    any = true;
                }
            }
            print(any ? o.str() : "(no active faults)");
            return true;
        }
        std::string target = toLower(a[0]);
        if (!sys_.hasNode(target)) {
            print("error: unknown node: " + target);
            return true;
        }
        const Node& node = sys_.get(target);
        if (node.faults.empty()) {
            print(target + ": no active faults");
            return true;
        }
        std::ostringstream o;
        for (const auto& [fname, f] : node.faults) {
            o << fname << " [" << toString(f.kind) << "/" << toString(f.severity)
              << "] since t=" << f.sinceTick;
            if (!f.detail.empty()) o << " (" << f.detail << ")";
            o << "\n";
        }
        print(o.str());
        return true;
    }
    if (c == "deceive" || c == "lie" || c == "spoof") {
        if (!guard(Op::MUTATE, "deceive")) return true;
        if (a.size() < 3) {
            print("usage: deceive <observer> <target.property> <value>");
            return true;
        }
        std::string observer = toLower(a[0]);
        std::string tp = a[1];
        std::string val = join(a, 2);
        auto dot = tp.find('.');
        if (dot == std::string::npos) {
            print("usage: deceive <observer> <target.property> <value>");
            return true;
        }
        std::string target = toLower(tp.substr(0, dot));
        std::string prop = tp.substr(dot + 1);
        OpResult r = sys_.deceive(observer, target, prop, val);
        if (!r.ok) {
            print("error: " + r.error);
            return true;
        }
        std::cout << r.info << " (truth: " << sys_.effectiveProp("", target, prop) << ")\n";
        return true;
    }
    if (c == "undeceive") {
        if (!guard(Op::MUTATE, "deceive")) return true;
        if (a.empty()) {
            print("usage: undeceive <observer> [target]");
            return true;
        }
        OpResult r = sys_.clearBeliefs(toLower(a[0]), a.size() > 1 ? toLower(a[1]) : "");
        if (!r.ok) print("error: " + r.error);
        else print(r.info);
        return true;
    }
    if (c == "beliefs") {
        if (a.empty()) {
            bool any = false;
            for (auto& n : sys_.nodeNames()) {
                const Node& node = sys_.get(n);
                for (const auto& [t, props] : node.beliefs)
                    for (const auto& [k, v] : props) {
                        std::cout << n << " believes " << t << "." << k << "=" << v << "\n";
                        any = true;
                    }
            }
            if (!any) print("(no beliefs planted)");
        } else {
            std::string obs = toLower(a[0]);
            if (!sys_.hasNode(obs)) {
                print("error: unknown node: " + obs);
                return true;
            }
            const Node& o = sys_.get(obs);
            if (o.beliefs.empty()) {
                print(obs + " holds no beliefs.");
                return true;
            }
            for (const auto& [t, props] : o.beliefs)
                for (const auto& [k, v] : props) std::cout << t << "." << k << "=" << v << "\n";
        }
        return true;
    }
    if (c == "ps" || c == "process") {
        std::string sub = a.empty() ? "list" : toLower(a[0]);
        if (c == "ps" && a.empty()) sub = "list";
        if (sub == "list" || sub == "ps") {
            observed_.subsystems.insert("processes");
            print(sys_.psList());
            return true;
        }
        if (sub == "inspect" || sub == "show") {
            if (a.size() < 2) {
                print("usage: process inspect <pid|name>");
                return true;
            }
            observed_.subsystems.insert("processes");
            observed_.procInspected = true;
            print(sys_.procInspect(a[1]));
            return true;
        }
        if (sub == "spawn" || sub == "create" || sub == "fork") {
            if (!guard(Op::MUTATE, "process")) return true;
            if (a.size() < 2) {
                print("usage: process spawn <name> [type] [host] [parent-pid]");
                return true;
            }
            std::string type = a.size() > 2 ? toLower(a[2]) : "worker";
            std::string host = a.size() > 3 ? toLower(a[3]) : "";
            int parent = 0;
            if (a.size() > 4 && !parseIntStrict(a[4], parent)) {
                print("error: invalid parent pid '" + a[4] + "'");
                return true;
            }
            OpResult r = sys_.procSpawn(toLower(a[1]), type, host, parent);
            if (!r.ok) print("error: " + r.error);
            else print(r.info);
            return true;
        }
        if (sub == "kill" || sub == "terminate" || sub == "stop") {
            if (!guard(Op::MUTATE, "process")) return true;
            if (a.size() < 2) {
                print("usage: process kill <pid|name>\n"
                      "  [crash|oom|segfault|runaway|deadlock|sigkill|zombie|block|degrade]");
                return true;
            }
            if (!riskGate("proc-kill", {toLower(a[1])}, false)) return true;
            std::string reason = a.size() > 2 ? toLower(a[2]) : "crash";
            OpResult r = sys_.procKill(toLower(a[1]), reason);
            if (!r.ok) print("error: " + r.error);
            else print(r.info);
            return true;
        }
        if (sub == "pause" || sub == "suspend") {
            if (!guard(Op::MUTATE, "process")) return true;
            if (a.size() < 2) {
                print("usage: process pause <pid|name>");
                return true;
            }
            OpResult r = sys_.procPause(toLower(a[1]));
            if (!r.ok) print("error: " + r.error);
            else print(r.info);
            return true;
        }
        if (sub == "resume" || sub == "continue") {
            if (!guard(Op::MUTATE, "process")) return true;
            if (a.size() < 2) {
                print("usage: process resume <pid|name>");
                return true;
            }
            OpResult r = sys_.procResume(toLower(a[1]));
            if (!r.ok) print("error: " + r.error);
            else print(r.info);
            return true;
        }
        if (sub == "restart" || sub == "reboot") {
            if (!guard(Op::MUTATE, "process")) return true;
            if (a.size() < 2) {
                print("usage: process restart <pid|name>");
                return true;
            }
            OpResult r = sys_.procRestart(toLower(a[1]));
            if (!r.ok) print("error: " + r.error);
            else print(r.info);
            return true;
        }
        if (sub == "tree" || sub == "tasks") {
            observed_.subsystems.insert("processes");
            print(sys_.processTree());
            return true;
        }
        print("usage: process list | inspect <pid|name> | spawn <name> [type] [host] "
              "| kill <pid|name> [crash|oom|segfault|runaway|deadlock|sigkill|zombie|block|"
              "degrade] | pause|resume|restart <pid|name> | tree");
        return true;
    }
    if (c == "threads" || c == "thread" || c == "sched" || c == "syscalls" || c == "ipc" ||
        c == "signal") {
        if (c == "threads") {
            observed_.subsystems.insert("processes");
            print(sys_.threadList(a.empty() ? "" : toLower(a[0])));
            return true;
        }
        if (c == "thread") {
            std::string sub = a.empty() ? "" : toLower(a[0]);
            if (sub == "spawn" && a.size() >= 3) {
                if (!guard(Op::MUTATE, "thread")) return true;
                OpResult r = sys_.threadSpawn(toLower(a[1]), toLower(a[2]));
                if (!r.ok) print("error: " + r.error);
                else print(r.info);
                return true;
            }
            if ((sub == "inspect" || sub == "show") && a.size() >= 2) {
                int tid = 0;
                if (!parseIntStrict(a[1], tid)) {
                    print("error: invalid tid '" + a[1] + "'");
                    return true;
                }
                print(sys_.threadInspect(tid));
                return true;
            }
            print("usage: thread spawn <proc> <name> | thread inspect <tid>  (or: threads [proc])");
            return true;
        }
        if (c == "sched") {
            print(sys_.schedView());
            return true;
        }
        if (c == "syscalls") {
            print(sys_.syscallView());
            return true;
        }
        if (c == "ipc") {
            std::string sub = a.empty() ? "list" : toLower(a[0]);
            if (sub == "list" || sub == "ls") {
                print(sys_.ipcList(a.size() > 1 ? toLower(a[1]) : ""));
                return true;
            }
            if (sub == "send") {
                if (!guard(Op::MUTATE, "ipc")) return true;
                if (a.size() < 3) {
                    print("usage: ipc send <proc> <message...>");
                    return true;
                }
                OpResult r = sys_.ipcSend(toLower(a[1]), join(a, 2));
                if (!r.ok) print("error: " + r.error);
                else print(r.info);
                return true;
            }
            if (sub == "recv" || sub == "receive") {
                if (!guard(Op::MUTATE, "ipc")) return true;
                if (a.size() < 2) {
                    print("usage: ipc recv <proc>");
                    return true;
                }
                OpResult r = sys_.ipcRecv(toLower(a[1]));
                if (!r.ok) print("error: " + r.error);
                else print("received: " + r.info);
                return true;
            }
            print("usage: ipc [list [proc]|send <proc> <message...>|recv <proc>]");
            return true;
        }
        // signal <SIG> <proc>: TERM/STOP park, CONT resumes, KILL kills, HUP restarts.
        if (a.size() < 2) {
            print("usage: signal <TERM|STOP|CONT|KILL|HUP> <pid|name>");
            return true;
        }
        if (!guard(Op::MUTATE, "signal")) return true;
        if (!riskGate("signal", {toLower(a[1])}, false)) return true;
        OpResult r = sys_.procSignal(a[0], toLower(a[1]));
        if (!r.ok) print("error: " + r.error);
        else print(r.info);
        return true;
    }
    if (c == "service" || c == "svc") {
        std::string sub = a.empty() ? "list" : toLower(a[0]);
        if (sub == "list" || sub == "ls") {
            observed_.subsystems.insert("services");
            print(sys_.serviceList());
            return true;
        }
        if (sub == "inspect" || sub == "show") {
            if (a.size() < 2) {
                print("usage: service inspect <name>");
                return true;
            }
            std::string target = toLower(a[1]);
            observed_.subsystems.insert("services");
            observed_.serviceInspected = true;
            std::string out = sys_.serviceInspect(target);
            if (out.rfind("unknown service:", 0) == 0) out += svcUnknownHint(target);
            print(out);
            return true;
        }
        if (sub == "spawn" || sub == "create" || sub == "add") {
            if (!guard(Op::MUTATE, "service")) return true;
            // service spawn <name> <node> <binary> [config] [policy]
            if (a.size() < 4) {
                print("usage: service spawn <name> <node> <binary> [config] [ALWAYS|NEVER]");
                return true;
            }
            std::string config = a.size() > 4 ? a[4] : "";
            std::string policy = a.size() > 5 ? a[5] : "ALWAYS";
            // DWIM: a lone 4th word that names a policy is the policy, not a
            // config path (so `spawn demo server /bin/sh ALWAYS` just works).
            std::string cup = config;
            for (auto& ch : cup) ch = (char)std::toupper((unsigned char)ch);
            if (a.size() == 5 && (cup == "ALWAYS" || cup == "NEVER")) {
                policy = a[4];
                config.clear();
            }
            OpResult r =
                sys_.serviceSpawn(toLower(a[1]), toLower(a[2]), toLower(a[3]), config, policy);
            if (!r.ok) print("error: " + r.error);
            else print(r.info);
            return true;
        }
        if (sub == "start") {
            if (!guard(Op::MUTATE, "service")) return true;
            if (a.size() < 2) {
                print("usage: service start <name>");
                return true;
            }
            std::string target = toLower(a[1]);
            OpResult r = sys_.serviceStart(target);
            if (!r.ok) {
                std::string e = r.error;
                if (e.rfind("unknown service:", 0) == 0) e += svcUnknownHint(target);
                print("error: " + e);
            } else print(r.info);
            return true;
        }
        if (sub == "stop") {
            if (!guard(Op::MUTATE, "service")) return true;
            if (a.size() < 2) {
                print("usage: service stop <name>");
                return true;
            }
            std::string target = toLower(a[1]);
            if (!riskGate("service-stop", {target}, false)) return true;
            OpResult r = sys_.serviceStop(target);
            if (!r.ok) {
                std::string e = r.error;
                if (e.rfind("unknown service:", 0) == 0) e += svcUnknownHint(target);
                print("error: " + e);
            } else print(r.info);
            return true;
        }
        if (sub == "restart" || sub == "reboot") {
            if (!guard(Op::MUTATE, "service")) return true;
            if (a.size() < 2) {
                print("usage: service restart <name>");
                return true;
            }
            std::string target = toLower(a[1]);
            OpResult r = sys_.serviceRestart(target);
            if (!r.ok) {
                std::string e = r.error;
                if (e.rfind("unknown service:", 0) == 0) e += svcUnknownHint(target);
                print("error: " + e);
            } else print(r.info);
            return true;
        }
        if (sub == "remove" || sub == "delete" || sub == "rm") {
            if (!guard(Op::MUTATE, "service")) return true;
            if (a.size() < 2) {
                print("usage: service remove <name> [--force]");
                return true;
            }
            ForceArgs fa = stripForce(std::vector<std::string>(a.begin() + 1, a.end()));
            if (fa.args.empty()) {
                print("usage: service remove <name> [--force]");
                return true;
            }
            std::string target = toLower(fa.args[0]);
            if (!riskGate("service-remove", {target}, fa.force)) return true;
            OpResult r = sys_.serviceRemove(target);
            if (!r.ok) {
                std::string e = r.error;
                if (e.rfind("unknown service:", 0) == 0) e += svcUnknownHint(target);
                print("error: " + e);
            } else print(r.info);
            return true;
        }
        print("usage: service list | inspect <name> | spawn <name> <node> <binary> [config] "
              "[policy] | start|stop|restart|remove <name>");
        return true;
    }
    if (c == "trace") {
        if (a.empty()) {
            print("usage: trace <node>");
            return true;
        }
        observed_.subsystems.insert("causality");
        print(sys_.trace(toLower(a[0])));
        return true;
    }
    if (c == "why") {
        if (a.empty()) {
            print("usage: why <node>");
            return true;
        }
        observed_.subsystems.insert("causality");
        print(sys_.why(toLower(a[0])));
        return true;
    }
    if (c == "network" || c == "topology" || c == "route") {
        observed_.subsystems.insert("network");
        print(sys_.topology());
        return true;
    }
    if (c == "connect" || c == "link") {
        if (!guard(Op::MUTATE, "connect")) return true;
        if (a.size() < 2) {
            print("usage: connect <a> <b>");
            return true;
        }
        OpResult r = sys_.connect(toLower(a[0]), toLower(a[1]));
        if (!r.ok) print("error: " + r.error);
        else {
            print(r.info + ".");
            printCausedBy(r.eventId, toLower(a[0]));
        }
        return true;
    }
    if (c == "disconnect" || c == "unlink") {
        if (!guard(Op::MUTATE, "disconnect")) return true;
        if (a.size() < 2) {
            print("usage: disconnect <a> <b>");
            return true;
        }
        if (!riskGate("disconnect", {toLower(a[0]), toLower(a[1])}, false)) return true;
        OpResult r = sys_.disconnect(toLower(a[0]), toLower(a[1]));
        if (!r.ok) print("error: " + r.error);
        else {
            print(r.info + ".");
            printCausedBy(r.eventId, toLower(a[0]));
        }
        return true;
    }
    if (c == "depend") {
        if (!guard(Op::MUTATE, "depend")) return true;
        if (a.size() < 2) {
            print("usage: depend <node> <dep>");
            return true;
        }
        OpResult r = sys_.addDependency(toLower(a[0]), toLower(a[1]));
        if (!r.ok) print("error: " + r.error);
        else {
            print(r.info + ".");
            printCausedBy(r.eventId, toLower(a[0]));
        }
        return true;
    }
    if (c == "ping") {
        if (a.empty()) {
            print("usage: ping <node> [observer]");
            return true;
        }
        std::string obs = a.size() > 1 ? toLower(a[1]) : "";
        std::string out = sys_.pingProbes(toLower(a[0]), obs);
        observed_.subsystems.insert("network");
        if (out.find("packet loss: 0%") != std::string::npos) observed_.pingOk = true;
        print(out);
        return true;
    }
    if (c == "packet") {
        if (a.size() < 2) {
            print("usage: packet <from> <to>");
            return true;
        }
        std::string from = toLower(a[0]), to = toLower(a[1]);
        std::string out = System::formatPacket(from, to, sys_.sendPacket(from, to));
        observed_.subsystems.insert("network");
        if (out.find("DELIVERED") != std::string::npos) observed_.packetOk = true;
        print(out);
        return true;
    }
    if (c == "time" || c == "clock") {
        std::cout << sys_.clock().now() << " events=" << sys_.events().size() << "\n";
        return true;
    }
    if (c == "tick" || c == "step" || c == "ff") {
        if (!guard(Op::ADVANCE, "tick")) return true;
        if (sys_.clock().paused()) {
            print("clock paused. Use `resume` first.");
            return true;
        }
        int n = 1;
        if (!a.empty() && !parseIntStrict(a[0], n)) {
            print("error: invalid tick count '" + a[0] + "' (expected integer >= 1)");
            return true;
        }
        if (n < 1) {
            print("error: tick count must be >= 1, got '" + (a.empty() ? "" : a[0]) + "'");
            return true;
        }
        // Long runs show one in-place bar; short runs print normally.
        // Paused clocks advance zero ticks, so no bar is ever faked.
        ProgressOut bar(10);
        int done = sys_.tick(n, 0, shellProgress(bar));
        bar.finish();
        std::cout << "advanced " << done << " tick(s) -> " << sys_.clock().now() << "\n";
        return true;
    }
    if (c == "pause") {
        if (!guard(Op::ADVANCE, "pause")) return true;
        sys_.clock().pause();
        sys_.emit(0, "CLOCK_PAUSED", "user", "system", "clock paused");
        print("paused at " + sys_.clock().now());
        return true;
    }
    if (c == "resume") {
        if (!guard(Op::ADVANCE, "resume")) return true;
        sys_.clock().resume();
        sys_.emit(0, "CLOCK_RESUMED", "user", "system", "clock resumed");
        print("resumed at " + sys_.clock().now());
        return true;
    }
    if (c == "history") {
        observed_.subsystems.insert("ledger");
        const auto& h = sys_.history().commands();
        for (size_t i = 0; i < h.size(); ++i) std::cout << "  " << (i + 1) << "  " << h[i] << "\n";
        if (h.empty()) print("(no commands yet)");
        return true;
    }
    if (c == "events" || c == "log") {
        std::string target;
        int limit = 20;
        if (!a.empty()) {
            int num = 0;
            if (parseIntStrict(a[0], num)) {
                limit = num;
            } else {
                target = toLower(a[0]);
                if (a.size() > 1) {
                    if (!parseIntStrict(a[1], limit)) {
                        print("error: invalid limit '" + a[1] + "' (expected integer >= 1)");
                        return true;
                    }
                }
            }
        }
        if (limit < 1) {
            print("error: limit must be >= 1");
            return true;
        }
        observed_.subsystems.insert("ledger");
        const auto& all = sys_.events().all();
        int shown = 0;
        for (auto it = all.rbegin(); it != all.rend() && shown < limit; ++it) {
            if (!target.empty() && it->target != target) continue;
            std::ostringstream line;
            line << "[t=" << it->tick << " #" << it->id << "] [" << eventScope(*it) << "] "
                 << it->type << " " << it->source << "->" << it->target << ": " << it->message;
            if (it->causeId) line << " (caused by #" << it->causeId << ")";
            std::cout << color::paintLine(line.str()) << "\n";
            ++shown;
        }
        if (shown == 0) print("(no events)");
        return true;
    }
    if (c == "checkpoint" || c == "snapshot" || c == "snap") {
        uint64_t id = sys_.checkpoint(join(a, 0));
        std::cout << "checkpoint #" << id << " at " << sys_.clock().now() << "\n";
        return true;
    }
    if (c == "restore") {
        if (!guard(Op::RESTORE, "restore")) return true;
        if (a.empty()) {
            print("usage: restore <checkpoint-id>");
            return true;
        }
        uint64_t id = 0;
        if (!parseUintStrict(a[0], id)) {
            print("error: invalid checkpoint id '" + a[0] + "' (expected integer)");
            return true;
        }
        ProgressOut bar;
        bool ok = sys_.restore(id, nullptr, shellProgress(bar));
        bar.finish();
        if (ok) print("restored checkpoint #" + a[0] + " (events kept)");
        else print("error: no such checkpoint: " + a[0]);
        return true;
    }
    if (c == "rewind") {
        if (!guard(Op::RESTORE, "rewind")) return true;
        int n = 0;
        if (!a.empty() && !parseIntStrict(a[0], n)) {
            print("error: invalid rewind count '" + a[0] + "' (expected integer >= 0)");
            return true;
        }
        if (n < 0) {
            print("error: rewind count must be >= 0");
            return true;
        }
        ProgressOut bar;
        bool ok = sys_.rewindSteps(n, nullptr, shellProgress(bar));
        bar.finish();
        if (ok) print("rewound to " + sys_.clock().now() + " (events kept)");
        else print("nothing to rewind to.");
        return true;
    }
    // ---- persistent worlds (w1..w6 .ord slots; see `help worlds`) ----
    if (c == "joinw" || c == "join") {
        if (a.empty()) {
            print("usage: joinw <world>  (w1..w6)");
            return true;
        }
        return joinWorld(a[0]);
    }
    if (c == "save") {
        if (!hasWorlds()) {
            print("error: world persistence not configured in this session");
            return true;
        }
        std::string err;
        if (!saveActive(err)) print("error: " + err);
        else {
            persistAchievements();
            print("World " + sys_.worldId() + " saved.");
        }
        return true;
    }
    if (c == "reset") {
        if (!guard(Op::MUTATE, "reset")) return true;
        ForceArgs fa = stripForce(a);
        return resetWorld(fa.force);
    }
    if (c == "deletew" || c == "delete-world") {
        if (!guard(Op::MUTATE, "deletew")) return true;
        ForceArgs fa = stripForce(a);
        if (fa.args.empty()) {
            print("usage: deletew <world> [--force]  (permanent; frees the slot)");
            return true;
        }
        return deleteWorld(fa.args[0], fa.force);
    }
    if (c == "note") {
        if (!hasWorlds()) {
            print("error: world persistence not configured in this session");
            return true;
        }
        if (a.empty()) {
            print(sys_.worldNote().empty() ? "(no note)" : sys_.worldNote());
            return true;
        }
        if (!guard(Op::MUTATE, "note")) return true;
        std::string text = join(a, 0);
        sys_.setWorldNote(text);
        sys_.emit(0, "USER_NOTE", "user", "world", "note: " + text);
        print("note set.");
        return true;
    }
    if (c == "worlds") {
        showWorlds();
        return true;
    }
    if (c == "world") {
        showWorld();
        return true;
    }
    if (c == "upt" || c == "uptime") {
        uptCmd(a);
        return true;
    }
    if (c == "color" || c == "colour") {
        if (a.empty()) {
            print(std::string("colors ") + (color::enabled() ? "on." : "off.") +
                  " (auto: on in interactive shells; NO_COLOR=1 forces off)");
            return true;
        }
        std::string m = toLower(a[0]);
        if (m == "on") color::setMode(color::Mode::On);
        else if (m == "off") color::setMode(color::Mode::Off);
        else if (m == "auto") color::setMode(color::Mode::Auto);
        else {
            print("usage: color [on|off|auto]");
            return true;
        }
        print(std::string("color ") + m + ".");
        return true;
    }
    if (c == "achievement" || c == "achievements" || c == "achmt") {
        // Same viewer under both names. Headless prints the full list;
        // interactive opens the paged keyboard application.
        return runAchievementViewer();
    }
    if (c == "rec" || c == "recovery") {
        // Read-only diagnosis: never mutates simulation/achievement/worlds.
        bool detailed = false;
        std::vector<std::string> rest;
        for (const auto& x : a) {
            std::string l = toLower(x);
            if (l == "-h" || l == "--help" || l == "help") detailed = true;
            else rest.push_back(x);
        }
        if (rest.empty()) {
            print("usage: rec <kernel|boot|filesystem|module [name]|service [name]|\n"
                  "  network|device [name]|node <name>|<service>> [-h]");
            return true;
        }
        print(sys_.recoveryReport(rest[0], rest.size() > 1 ? rest[1] : "", detailed));
        return true;
    }
    if (c == "branch") {
        if (!guard(Op::RESTORE, "branch")) return true;
        if (a.empty()) {
            print("usage: branch <label>");
            return true;
        }
        uint64_t id = sys_.checkpoint("branch:" + join(a, 0));
        std::cout << "branched at checkpoint #" << id << " (linear history)\n";
        return true;
    }
    if (c == "replay") {
        observed_.subsystems.insert("ledger");
        int limit = 10;
        if (!a.empty() && !parseIntStrict(a[0], limit)) {
            print("error: invalid replay count '" + a[0] + "' (expected integer >= 1)");
            return true;
        }
        if (limit < 1) {
            print("error: replay count must be >= 1");
            return true;
        }
        const auto& all = sys_.events().all();
        size_t start = all.size() > (size_t)limit ? all.size() - (size_t)limit : 0;
        std::cout << "replaying last " << (all.size() - start) << " events:\n";
        for (size_t i = start; i < all.size(); ++i) {
            const auto& e = all[i];
            std::ostringstream line;
            line << "  [t=" << e.tick << " #" << e.id << "] " << e.type << ": " << e.message;
            std::cout << color::paintLine(line.str()) << "\n";
        }
        sys_.emit(0, "REPLAY", "user", "system",
                  "replayed " + std::to_string(all.size() - start) + " events");
        return true;
    }
    if (c == "predict" || c == "forecast") {
        if (a.empty()) {
            print("usage: predict <node>");
            return true;
        }
        print(sys_.predict(toLower(a[0])));
        return true;
    }
    if (c == "race") {
        if (a.empty()) {
            print("usage: race <command...>  (e.g. race repair server)");
            return true;
        }
        auto t0 = std::chrono::steady_clock::now();
        std::string sub = join(a, 0);
        std::cout << "racing: " << sub << "\n";
        execLine(sub, false);
        auto t1 = std::chrono::steady_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        sys_.emit(0, "RACE", "user", "system",
                  "race '" + sub + "' finished in " + std::to_string(us) + "us");
        std::cout << "race finished in " << us << "us at " << sys_.clock().now() << "\n";
        return true;
    }
    if (c == "benchmark" || c == "bench") {
        if (!guard(Op::MUTATE, "benchmark")) return true;
        int ticks = 10;
        if (!a.empty() && !parseIntStrict(a[0], ticks)) {
            print("error: invalid tick count '" + a[0] + "' (expected integer >= 1)");
            return true;
        }
        if (ticks < 1) {
            print("error: tick count must be >= 1");
            return true;
        }
        uint64_t snap = sys_.checkpoint("bench-before");
        auto t0 = std::chrono::steady_clock::now();
        if (!riskGate("break", {"server"}, false)) return true;
        sys_.breakNode("server");
        if (sys_.clock().paused()) {
            print("clock paused. Use `resume` first.");
            return true;
        }
        // Long benchmark runs share one bar across both tick phases.
        ProgressOut bbar(10);
        int total = 2 * ticks;
        sys_.tick(ticks, 0, [&](int d, int t) {
            (void)t;
            bbar.update(d, total);
        });
        sys_.repairNode("server");
        sys_.tick(ticks, 0, [&](int d, int t) {
            (void)t;
            bbar.update(ticks + d, total);
        });
        bbar.finish();
        auto t1 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::cout << "benchmark: break->tick(" << ticks << ")->repair->tick(" << ticks
                  << ") in " << ms << "ms wallclock, state:\n";
        print(sys_.status());
        std::cout << "(baseline checkpoint #" << snap << " kept for comparison)\n";
        sys_.emit(0, "BENCHMARK", "engine", "system",
                  "benchmark ticks=" + std::to_string(ticks) + " wall=" + std::to_string(ms) +
                      "ms");
        return true;
    }
    if (c == "scenario" || c == "scenarios") {
        auto all = builtinScenarios();
        if (a.empty() || toLower(a[0]) == "list") {
            std::cout << "scenarios:\n";
            for (auto& s : all) std::cout << "  " << s.name << " -- " << s.description << "\n";
            return true;
        }
        std::string sub = toLower(a[0]);
        if ((sub == "run" || sub == "load") && a.size() > 1) {
            const Scenario* s = findScenario(all, toLower(a[1]));
            if (!s) {
                print("error: unknown scenario: " + a[1]);
                return true;
            }
            std::cout << "SCENARIO: " << s->name << "\n";
            for (auto& st : s->steps) {
                std::cout << prompt() << st.line << "\n";
                if (!execLine(st.line)) break;
            }
            std::cout << "scenario '" << s->name << "' done.\n";
            return true;
        }
        print("usage: scenario list | scenario run <name>");
        return true;
    }
    if (c == "experiment") {
        auto all = builtinScenarios();
        if (a.size() >= 2 && (toLower(a[0]) == "run" || toLower(a[0]) == "create")) {
            const Scenario* s = findScenario(all, toLower(a[1]));
            if (!s) {
                print("error: unknown scenario: " + a[1]);
                return true;
            }
            uint64_t before = sys_.checkpoint("exp-before:" + s->name);
            for (auto& st : s->steps) {
                std::cout << prompt() << st.line << "\n";
                if (!execLine(st.line)) break;
            }
            uint64_t after = sys_.checkpoint("exp-after:" + s->name);
            std::cout << "experiment '" << s->name << "': before=#" << before
                      << " after=#" << after << " events=" << sys_.events().size() << "\n";
            return true;
        }
        if (!a.empty() && toLower(a[0]) == "compare") {
            const auto& snaps = sys_.history().snapshots();
            std::cout << "checkpoints (" << snaps.size() << "):\n";
            for (auto& s : snaps)
                std::cout << "  #" << s.id << " '" << s.label << "' t=" << s.tick << " nodes="
                          << s.nodes.size() << "\n";
            return true;
        }
        print("usage: experiment run <scenario> | experiment compare");
        return true;
    }
    if (c == "mode") {
        if (a.empty()) {
            std::cout << "mode: " << sys_.mode();
            if (isReservedMode(sys_.mode())) std::cout << " (reserved: behaves as normal)";
            std::cout << "\n";
            return true;
        }
        std::string m = a[0];
        for (auto& ch : m) ch = (char)std::toupper((unsigned char)ch);
        static const std::vector<std::string> known = {
            "NORMAL",     "OBSERVE", "DEBUG",    "FORENSIC",  "CHAOS",    "OVERRIDE",
            "SAFE",       "SIMULATION", "REPLAY", "TIME_TRAVEL", "NETWORK", "PROCESS",
            "MEMORY",     "SECURITY",  "EXPERIMENT", "BENCHMARK", "WATCH", "LEARNING",
            "SCENARIO",   "SCRIPT", "HEADLESS"};
        if (std::find(known.begin(), known.end(), m) == known.end()) {
            std::string s = suggest(a[0], known, 3);
            std::string msg = "error: unknown mode: " + a[0];
            if (!s.empty()) {
                std::string low = s;
                for (auto& ch : low) ch = (char)std::tolower((unsigned char)ch);
                msg += "\ndid you mean: " + low + "?";
            }
            print(msg);
            return true;
        }
        sys_.setMode(m);
        sys_.emit(0, "MODE_SWITCH", "user", "system", "mode -> " + m);
        std::cout << "mode -> " << m << "\n";
        if (m == "OBSERVE") print("(read-only: world frozen)");
        if (m == "FORENSIC") print("(frozen: investigate with trace/why/events/replay)");
        if (m == "SAFE") print("(destructive ops blocked)");
        if (m == "CHAOS") print("(chaos strike unlocked)");
        if (m == "OVERRIDE") print("(direct state/proc edits unlocked)");
        if (m == "SIMULATION") print("(auto-tick after each command)");
        if (m == "TIME_TRAVEL") print("(time operations only)");
        if (m == "WATCH") print("(status auto-displayed after world changes)");
        if (isReservedMode(m)) print("(reserved: no behavior change, acts as normal)");
        return true;
    }
    if (c == "crt") {
        // Alias of create (node spawn), except bare `crt w` = world creation.
        ParsedCommand c2 = cmd;
        c2.name = "create";
        return dispatch(c2);
    }
    if (c == "create" || c == "spawn" || c == "add") {
        if (!guard(Op::MUTATE, "create")) return true;
        // Bare `create w` allocates a persistent world slot (see `help worlds`).
        if (c == "create" && a.size() == 1 && toLower(a[0]) == "w") return createWorld();
        if (a.empty()) {
            print("usage: create <name> [type]");
            return true;
        }
        std::string name = toLower(a[0]);
        // Path-like names that cannot be node names become virtual files:
        // `create server.conf` == `touch server.conf`. Real nodes unchanged.
        if (a.size() == 1 && !isValidNodeName(name) &&
            (name.find('/') != std::string::npos || name.find('.') != std::string::npos)) {
            OpResult r = sys_.vfsTouch(a[0]);
            if (!r.ok) print("error: " + r.error);
            else print(r.info);
            return true;
        }
        std::string type = a.size() > 1 ? toLower(a[1]) : "service";
        OpResult r = sys_.addNode(name, type);
        if (!r.ok) print("error: " + r.error);
        else print(r.info);
        return true;
    }
    if (c == "remove" || c == "delete" || c == "destroy" || c == "terminate") {
        if (!guard(Op::MUTATE, "remove")) return true;
        ForceArgs fa = stripForce(a);
        if (fa.args.empty()) {
            print("usage: remove <name> [--force]");
            return true;
        }
        if (!riskGate("remove-node", fa.args, fa.force)) return true;
        OpResult r = sys_.removeNode(toLower(fa.args[0]));
        if (!r.ok) print("error: " + r.error);
        else print(r.info);
        return true;
    }
    if (c == "seed") {
        if (a.empty()) {
            std::cout << "seed: " << sys_.seed() << "\n";
            return true;
        }
        if (!guard(Op::MUTATE, "seed")) return true;
        uint64_t s = 0;
        if (!parseUintStrict(a[0], s)) {
            print("error: invalid seed '" + a[0] + "' (expected unsigned integer)");
            return true;
        }
        sys_.setSeed(s);
        print("seed set to " + a[0]);
        return true;
    }
    if (c == "chaos") {
        if (a.empty() || toLower(a[0]) != "strike") {
            print("usage: chaos strike [node]  (CHAOS mode only)");
            return true;
        }
        if (!guard(Op::MUTATE, "chaos")) return true;
        if (sys_.mode() != "CHAOS") {
            print("error: chaos strike requires CHAOS mode (`mode chaos`).");
            return true;
        }
        OpResult r = sys_.chaosStrike(a.size() > 1 ? toLower(a[1]) : "");
        if (!r.ok) print("error: " + r.error);
        else {
            print(r.info);
            printCausedBy(r.eventId, "");
        }
        return true;
    }
    if (c == "validate") {
        std::string err;
        if (sys_.validateAll(&err)) {
            observed_.validated = true;
            std::cout << "all invariants hold (" << sys_.nodeNames().size() << " nodes, "
                      << sys_.network().links().size() << " links)\n";
        } else {
            print("error: invariant violated: " + err);
        }
        return true;
    }
    if (c == "digest") {
        observed_.subsystems.insert("system");
        std::cout << "world: " << sys_.worldDigest() << "\nledger: " << sys_.digest() << "\n";
        return true;
    }
    if (c == "autofail" || c == "entropy") {
        std::string sub = a.empty() ? "status" : toLower(a[0]);
        if (sub == "status") {
            std::cout << "autonomous failures: " << (sys_.autoFaults() ? "on" : "off")
                      << " (rate " << sys_.autoRate() << "%/node/tick, seed " << sys_.seed()
                      << ")\n";
            return true;
        }
        if (!guard(Op::MUTATE, "autofail")) return true;
        if (sub == "on" || sub == "enable") {
            sys_.setAutoFaults(true);
            print("autonomous failures enabled.");
            return true;
        }
        if (sub == "off" || sub == "disable") {
            sys_.setAutoFaults(false);
            print("autonomous failures disabled.");
            return true;
        }
        if (sub == "rate") {
            if (a.size() < 2) {
                print("usage: autofail rate <0-100>");
                return true;
            }
            int rate = 0;
            if (!parseIntStrict(a[1], rate)) {
                print("error: invalid rate '" + a[1] + "' (expected integer 0-100)");
                return true;
            }
            OpResult r = sys_.setAutoRate(rate);
            if (!r.ok) print("error: " + r.error);
            else print(r.info);
            return true;
        }
        print("usage: autofail [on|off|rate <0-100>|status]");
        return true;
    }
    if (c == "local" || c == "host") {
        std::string sub = a.empty() ? "info" : toLower(a[0]);
        if (sub == "info" || sub == "status") print(sys_.localInfo());
        else if (sub == "ports") print(sys_.localPorts());
        else if (sub == "interfaces" || sub == "ifaces" || sub == "net")
            print(sys_.localInterfaces());
        else print("usage: local [info|ports|interfaces]");
        return true;
    }
    if (c == "resources" || c == "res" || c == "top") {
        observed_.subsystems.insert("resources");
        print(sys_.resources());
        return true;
    }
    if (c == "hardware" || c == "hw") {
        // Virtual hardware profile + V/R backends + overflow policy.
        // Fully simulated; never queries host hardware.
        observed_.subsystems.insert("hardware");
        std::string sub = a.empty() ? "status" : toLower(a[0]);
        if (sub == "status" || sub == "info" || sub == "show") {
            print(sys_.hardwareDescribe());
            return true;
        }
        if (sub == "backend") {
            if (a.size() < 3) {
                print("usage: hardware backend <cpu|ram|storage|net> <V|R>");
                return true;
            }
            if (!guard(Op::MUTATE, "hardware")) return true;
            OpResult r = sys_.hardwareBackend(toLower(a[1]), a[2]);
            if (!r.ok) print("error: " + r.error);
            else print(r.info);
            return true;
        }
        if (sub == "overflow" || sub == "policy") {
            if (a.size() < 2) {
                print("usage: hardware overflow <keep-r|reclaim|cancel>");
                return true;
            }
            if (!guard(Op::MUTATE, "hardware")) return true;
            OpResult r = sys_.hardwareOverflow(a[1]);
            if (!r.ok) print("error: " + r.error);
            else print(r.info);
            return true;
        }
        print("usage: hardware [status|backend <res> <V|R>|overflow <policy>]");
        return true;
    }
    if (c == "dev" || c == "device" || c == "devices") {
        // Simulated device metadata (/dev) + what each device gates.
        observed_.subsystems.insert("devices");
        std::string sub = a.empty() ? "list" : toLower(a[0]);
        if (sub == "list" || sub == "ls") {
            print(sys_.deviceList());
            return true;
        }
        if (sub == "inspect" || sub == "show" || sub == "info") {
            if (a.size() < 2) {
                print("usage: dev inspect <name>  (e.g. dev inspect net0)");
                return true;
            }
            print(sys_.deviceInspect(toLower(a[1])));
            return true;
        }
        if (sub == "net0" || sub == "disk0" || sub == "console" || sub == "cpu0" ||
            sub == "null" || sub == "zero" || sub == "tty0" || sub == "random") {
            print(sys_.deviceInspect(sub));
            return true;
        }
        print("usage: dev [list|inspect <name>]");
        return true;
    }
    if (c == "kernel") {
        // Simulated kernel views; all derived from engine state + ledger.
        observed_.subsystems.insert("kernel");
        std::string sub = a.empty() ? "status" : toLower(a[0]);
        if (sub == "status" || sub == "st") {
            print(sys_.kernelStatus());
            return true;
        }
        if (sub == "inspect" || sub == "show" || sub == "info") {
            if (a.size() < 2) {
                print("usage: kernel inspect <node>");
                return true;
            }
            print(sys_.kernelInspect(toLower(a[1])));
            return true;
        }
        print("usage: kernel [status|inspect <node>]");
        return true;
    }
    if (c == "uname") {
        observed_.subsystems.insert("kernel");
        print(sys_.uname(a.empty() ? "" : toLower(a[0])));
        return true;
    }
    if (c == "dmesg") {
        std::string target;
        int limit = 30;
        if (!a.empty()) {
            int num = 0;
            if (parseIntStrict(a[0], num)) {
                limit = num;
            } else {
                target = toLower(a[0]);
                if (a.size() > 1 && !parseIntStrict(a[1], limit)) {
                    print("error: invalid limit '" + a[1] + "' (expected integer >= 1)");
                    return true;
                }
            }
        }
        if (limit < 1) {
            print("error: limit must be >= 1");
            return true;
        }
        observed_.subsystems.insert("kernel");
        print(sys_.dmesg(target, limit));
        return true;
    }
    if (c == "lsmod") {
        observed_.subsystems.insert("kernel");
        print(sys_.lsmod(a.empty() ? "" : toLower(a[0])));
        return true;
    }
    if (c == "modprobe") {
        // Load path reuses the engine module machinery (no second implementation).
        if (!guard(Op::MUTATE, "modprobe")) return true;
        observed_.subsystems.insert("kernel");
        if (a.size() < 2) {
            print("usage: modprobe <node> <module>");
            return true;
        }
        OpResult r = sys_.injectFault(toLower(a[0]), "module-load", toLower(a[1]));
        if (!r.ok) print("error: " + r.error);
        else {
            print(r.info);
            printCausedBy(r.eventId, toLower(a[0]));
        }
        return true;
    }
    if (c == "module") {
        // Admin module control via the same fault-effect path as inject.
        if (!guard(Op::MUTATE, "module")) return true;
        observed_.subsystems.insert("kernel");
        ForceArgs mfa = stripForce(a);
        if (mfa.args.size() < 3) {
            print("usage: module <node> <load|unload|fail> <module> [--force]");
            return true;
        }
        std::string op = toLower(mfa.args[1]);
        std::string fault;
        if (op == "load") fault = "module-load";
        else if (op == "unload") fault = "module-unload";
        else if (op == "fail") fault = "module-fail";
        else {
            print("error: unknown module op '" + mfa.args[1] + "' (load|unload|fail)");
            return true;
        }
        std::string mnode = toLower(mfa.args[0]), mmod = toLower(mfa.args[2]);
        if ((op == "unload" || op == "fail") &&
            !riskGate(op == "unload" ? "module-unload" : "module-fail", {mnode, mmod},
                      mfa.force))
            return true;
        OpResult r = sys_.injectFault(mnode, fault, mmod);
        if (!r.ok) print("error: " + r.error);
        else {
            print(r.info);
            printCausedBy(r.eventId, toLower(a[0]));
        }
        return true;
    }
    if (c == "sysctl") {
        observed_.subsystems.insert("kernel");
        if (a.empty()) {
            print("usage: sysctl <node> [key[=value]]  (keys: kernel.instability, "
                  "thermal.limit, net.base_latency, vm.swap_mb, kernel.modules)");
            return true;
        }
        std::string target = toLower(a[0]);
        if (a.size() == 1) {
            for (const char* k :
                 {"kernel.instability", "thermal.limit", "net.base_latency", "vm.swap_mb",
                  "kernel.modules"}) {
                bool ok = false;
                std::string v = sys_.sysctlGet(target, k, ok);
                if (!ok) {
                    print("error: " + v);
                    return true;
                }
                std::cout << k << " = " << v << "\n";
            }
            return true;
        }
        std::string key = toLower(a[1]);
        std::string value;
        auto eq = key.find('=');
        if (eq != std::string::npos) {
            value = key.substr(eq + 1);
            key = key.substr(0, eq);
        } else if (a.size() > 2) {
            value = join(a, 2);
        }
        if (value.empty()) {
            bool ok = false;
            std::string v = sys_.sysctlGet(target, key, ok);
            if (!ok) print("error: " + v);
            else print(key + " = " + v);
            return true;
        }
        if (!guard(Op::MUTATE, "sysctl")) return true;
        OpResult r = sys_.sysctlSet(target, key, value);
        if (!r.ok) print("error: " + r.error);
        else print(r.info);
        return true;
    }
    if (c == "run" || c == "script" || c == "load" || c == "source") {
        if (a.empty()) {
            print("usage: run <file>");
            return true;
        }
        runScript(join(a, 0));
        return true;
    }
    if (c == "echo") {
        print(join(a, 0));
        return true;
    }
    // ---- OVERKNRL virtual filesystem (simulated tree; host fs never touched)
    if (c == "ls" || c == "dir") {
        observed_.subsystems.insert("vfs");
        noteFsArea(a.empty() ? sys_.cwd() : a[0]);
        print(sys_.vfsList(a.empty() ? "" : a[0]));
        return true;
    }
    if (c == "pwd") {
        observed_.subsystems.insert("vfs");
        print(sys_.cwd());
        return true;
    }
    if (c == "cd") {
        if (a.empty()) {
            print("usage: cd <path>  (virtual filesystem)");
            return true;
        }
        observed_.subsystems.insert("vfs");
        noteFsArea(a[0]);
        OpResult r = sys_.setCwd(a[0]);
        if (!r.ok) print("error: " + r.error);
        else print(r.info);
        return true;
    }
    if (c == "cat" || c == "type") {
        if (a.empty()) {
            print("usage: cat <path>  (virtual filesystem)");
            return true;
        }
        observed_.subsystems.insert("vfs");
        noteFsArea(a[0]);
        print(sys_.vfsCat(a[0]));
        return true;
    }
    if (c == "mkdir" || c == "md") {
        if (!guard(Op::MUTATE, "mkdir")) return true;
        if (a.empty()) {
            print("usage: mkdir <path>  (virtual filesystem)");
            return true;
        }
        observed_.subsystems.insert("vfs");
        noteFsArea(a[0]);
        OpResult r = sys_.vfsMkdir(a[0]);
        if (!r.ok) print("error: " + r.error);
        else print(r.info);
        return true;
    }
    if (c == "touch") {
        if (!guard(Op::MUTATE, "touch")) return true;
        if (a.empty()) {
            print("usage: touch <path>  (virtual filesystem)");
            return true;
        }
        observed_.subsystems.insert("vfs");
        noteFsArea(a[0]);
        OpResult r = sys_.vfsTouch(a[0]);
        if (!r.ok) print("error: " + r.error);
        else print(r.info);
        return true;
    }
    if (c == "rm" || c == "del") {
        if (!guard(Op::MUTATE, "rm")) return true;
        ForceArgs fa = stripForce(a);
        if (fa.args.empty()) {
            print("usage: rm <path> [--force]  (virtual filesystem; files + empty dirs)");
            return true;
        }
        observed_.subsystems.insert("vfs");
        noteFsArea(fa.args[0]);
        if (!riskGate("rm", fa.args, fa.force)) return true;
        OpResult r = sys_.vfsRemove(fa.args[0]);
        if (!r.ok) print("error: " + r.error);
        else print(r.info);
        return true;
    }
    if (c == "cp" || c == "copy") {
        if (!guard(Op::MUTATE, "cp")) return true;
        if (a.size() < 2) {
            print("usage: cp <src> <dst>  (virtual filesystem)");
            return true;
        }
        observed_.subsystems.insert("vfs");
        noteFsArea(a[0]);
        OpResult r = sys_.vfsCopy(a[0], a[1]);
        if (!r.ok) print("error: " + r.error);
        else print(r.info);
        return true;
    }
    if (c == "mv" || c == "move" || c == "rename") {
        if (!guard(Op::MUTATE, "mv")) return true;
        if (a.size() < 2) {
            print("usage: mv <src> <dst>  (virtual filesystem)");
            return true;
        }
        observed_.subsystems.insert("vfs");
        noteFsArea(a[0]);
        OpResult r = sys_.vfsMove(a[0], a[1]);
        if (!r.ok) print("error: " + r.error);
        else print(r.info);
        return true;
    }
    if (c == "edit" || c == "write") {
        if (!guard(Op::MUTATE, "edit")) return true;
        if (a.size() < 2) {
            print("usage: edit <path> <text...>  (virtual filesystem; creates or replaces)");
            return true;
        }
        observed_.subsystems.insert("vfs");
        noteFsArea(a[0]);
        OpResult r = sys_.vfsWrite(a[0], join(a, 1));
        if (!r.ok) print("error: " + r.error);
        else print(r.info);
        return true;
    }
    if (c == "corrupt") {
        if (!guard(Op::MUTATE, "corrupt")) return true;
        ForceArgs fa = stripForce(a);
        if (fa.args.empty()) {
            print("usage: corrupt <path> [--force]  (virtual file; /etc/<node>.conf corrupts config)");
            return true;
        }
        observed_.subsystems.insert("vfs");
        noteFsArea(fa.args[0]);
        if (!riskGate("corrupt", fa.args, fa.force)) return true;
        OpResult r = sys_.vfsCorrupt(fa.args[0]);
        if (!r.ok) print("error: " + r.error);
        else print(r.info);
        return true;
    }
    if (c == "restart") {
        // Bare `restart <node>` = node reboot (process restarts stay namespaced).
        if (!guard(Op::MUTATE, "reboot")) return true;
        ForceArgs fa = stripForce(a);
        if (fa.args.empty()) {
            print("usage: restart <node>  (alias of reboot; `process restart` for procs)");
            return true;
        }
        if (!riskGate("reboot", fa.args, fa.force)) return true;
        std::string target = toLower(fa.args[0]);
        ProgressOut bar;
        OpResult r = sys_.rebootNode(target, shellProgress(bar));
        bar.finish();
        if (!r.ok) print("error: " + r.error);
        else {
            print(r.info);
            printCausedBy(r.eventId, target);
        }
        return true;
    }
    if (c == "whoami") {
        print("root@override (simulated sandbox - no host access)");
        return true;
    }

    std::string msg = "unknown command: " + cmd.name;
    std::string s = suggest(cmd.name, overshellCommands());
    if (!s.empty()) msg += "\ndid you mean: " + s + "?";
    else msg += " (try `help`)";
    print(msg);
    return true;
}

} // namespace override
