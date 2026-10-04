// Achievement engine: definitions, state, conditions, unlock logic,
// progress, and the transient notification queue.
#include "override/achievement.hpp"

#include <algorithm>
#include <sstream>

#include "override/system.hpp"

namespace override {

namespace {
const AchievementDef kDefs[] = {
    {"first_boot", "FIRST BOOT", "Boot a node for the first time.", false, 1},
    {"hello_world", "HELLO, WORLD", "Start the first simulated process.", false, 1},
    {"operator", "OPERATOR", "Perform a meaningful simulation intervention.", false, 1},
    {"explorer", "EXPLORER", "Inspect several major subsystems.", false, 5},
    {"validated", "VALIDATED", "Successfully pass world validation.", false, 1},
    {"kernel_panic", "KERNEL PANIC", "Trigger a kernel panic.", false, 1},
    {"cold_start", "COLD START", "Boot from OFF.", false, 1},
    {"clean_boot", "CLEAN BOOT", "Complete a boot without failure.", false, 1},
    {"not_today", "NOT TODAY", "Recover from a failed boot.", false, 1},
    {"you_had_one_job", "YOU HAD ONE JOB",
     "Make a node unbootable by destroying a critical kernel dependency.", true, 1},
    {"module_missing", "MODULE MISSING", "Cause a required module to become unavailable.",
     false, 1},
    {"kernel_surgery", "KERNEL SURGERY", "Recover from a kernel failure.", true, 1},
    {"dynamic", "DYNAMIC", "Load and unload a simulated module.", false, 1},
    {"panic_loop", "PANIC LOOP", "Trigger multiple kernel panics.", true, 3},
    {"back_from_dead", "BACK FROM THE DEAD", "Recover a node from terminal failure.", false,
     1},
    {"fs_explorer", "FILE SYSTEM EXPLORER", "Visit multiple virtual filesystem areas.", false,
     6},
    {"touch_grass", "TOUCH GRASS", "Create a file.", false, 1},
    {"organized", "ORGANIZED", "Create/manipulate a directory structure.", false, 1},
    {"copycat", "COPYCAT", "Copy a file.", false, 1},
    {"move_it", "MOVE IT", "Move a file.", false, 1},
    {"oops", "OOPS", "Delete something and later restore it.", true, 1},
    {"corruption", "CORRUPTION", "Corrupt a simulated file.", false, 1},
    {"root_energy", "ROOT USER ENERGY",
     "Modify protected configuration through valid simulated permissions.", false, 1},
    {"read_only", "READ ONLY", "Attempt a forbidden filesystem mutation.", true, 1},
    {"restored", "RESTORED", "Recover filesystem state using checkpoint/restore/rewind.",
     false, 1},
    {"process_engineer", "PROCESS ENGINEER", "Create and inspect a process.", false, 1},
    {"threads", "THREADS", "Use a process with multiple simulated threads.", false, 1},
    {"zombie_apocalypse", "ZOMBIE APOCALYPSE", "Create 5 zombie processes.", false, 5},
    {"orphaned", "ORPHANED", "Create an orphan process.", true, 1},
    {"terminated", "TERMINATED", "Successfully terminate a process.", false, 1},
    {"signal_fire", "SIGNAL FIRE", "Use simulated signals.", false, 1},
    {"pid_1", "PID 1", "Interact with simulated init.", true, 1},
    {"service_starter", "SERVICE STARTER", "Start a simulated service.", false, 1},
    {"service_stopper", "SERVICE STOPPER", "Stop a simulated service.", false, 1},
    {"supervisor", "SUPERVISOR", "Trigger and recover a service restart.", false, 1},
    {"service_cascade", "SERVICE CASCADE", "Cause a service dependency cascade.", false, 1},
    {"bad_binary", "BAD BINARY", "Attempt to start a service with an invalid executable.",
     false, 1},
    {"connected", "CONNECTED", "Successfully ping across a healthy link.", false, 1},
    {"disconnected", "DISCONNECTED", "Bring a link/interface down.", false, 1},
    {"back_online", "BACK ONLINE", "Restore the link and connectivity.", false, 1},
    {"packet_loss", "PACKET LOSS", "Cause meaningful packet loss.", false, 1},
    {"latency", "LATENCY", "Cause significant network latency.", false, 1},
    {"network_cascade", "NETWORK CASCADE", "Cause networking to affect another subsystem.",
     true, 1},
    {"too_hot", "TOO HOT TO HANDLE", "Cause severe thermal pressure.", false, 1},
    {"memory_pressure", "MEMORY PRESSURE", "Cause meaningful memory pressure.", false, 3},
    {"storage_crisis", "STORAGE CRISIS", "Cause significant storage pressure.", false, 1},
    {"resourceful", "RESOURCEFUL", "Recover from serious resource pressure without rebooting.",
     true, 1},
    {"time_traveler", "TIME TRAVELER", "Restore or rewind to a checkpoint.", false, 1},
    {"butterfly", "BUTTERFLY EFFECT",
     "Cause one intervention to generate a multi-event cascade.", true, 1},
    {"cause_effect", "CAUSE AND EFFECT", "Use why/trace to inspect a real causal chain.",
     false, 1},
};
constexpr size_t kDefCount = sizeof(kDefs) / sizeof(kDefs[0]);

const Event* findEvent(const System& sys, uint64_t id) { return sys.events().find(id); }

int countType(const System& sys, const std::string& type) {
    int n = 0;
    for (const auto& e : sys.events().all())
        if (e.type == type) ++n;
    return n;
}

bool hasType(const System& sys, const std::string& type) { return countType(sys, type) > 0; }

bool hasMeta(const System& sys, const std::string& type, const std::string& key,
             const std::string& value) {
    for (const auto& e : sys.events().all()) {
        if (e.type != type) continue;
        auto it = e.metadata.find(key);
        if (it != e.metadata.end() && it->second == value) return true;
    }
    return false;
}

bool msgHas(const System& sys, const std::string& type, const std::string& frag) {
    for (const auto& e : sys.events().all())
        if (e.type == type && e.message.find(frag) != std::string::npos) return true;
    return false;
}
} // namespace

const AchievementDef* AchievementEngine::defs(size_t& n) {
    n = kDefCount;
    return kDefs;
}

AchievementEngine::AchievementEngine() {
    for (size_t i = 0; i < kDefCount; ++i) states_[kDefs[i].id] = AchievementState{};
}

size_t AchievementEngine::count() const { return kDefCount; }

size_t AchievementEngine::unlockedCount() const {
    size_t n = 0;
    for (const auto& [id, st] : states_)
        if (st.unlocked) ++n;
    return n;
}

const AchievementDef& AchievementEngine::def(size_t i) const { return kDefs[i]; }

const AchievementState& AchievementEngine::state(const std::string& id) const {
    static const AchievementState kEmpty{};
    auto it = states_.find(id);
    return it == states_.end() ? kEmpty : it->second;
}

int AchievementEngine::progressOf(const std::string& id) const { return state(id).progress; }

bool AchievementEngine::unlocked(const std::string& id) const { return state(id).unlocked; }

int AchievementEngine::progressFor(const std::string& id, const System& sys,
                                   const ShellObserved& obs) const {
    const auto& evs = sys.events().all();
    (void)evs;
    if (id == "first_boot") return hasType(sys, "KERNEL_UP") ? 1 : 0;
    if (id == "hello_world") return hasType(sys, "PROC_SPAWNED") ? 1 : 0;
    if (id == "operator")
        return (hasType(sys, "USER_INJECT") || hasType(sys, "USER_BREAK") ||
                hasType(sys, "USER_OVERRIDE") || hasType(sys, "USER_CHAOS"))
                   ? 1
                   : 0;
    if (id == "explorer") return std::min<int>((int)obs.subsystems.size(), 5);
    if (id == "validated") return obs.validated ? 1 : 0;
    if (id == "kernel_panic") return hasType(sys, "KERNEL_PANIC") ? 1 : 0;
    if (id == "cold_start") return hasMeta(sys, "KERNEL_BOOT", "from", "OFF") ? 1 : 0;
    if (id == "clean_boot") {
        // A KERNEL_UP whose causal root produced no BOOT_FAILED sibling.
        for (const auto& e : sys.events().all()) {
            if (e.type != "KERNEL_UP") continue;
            bool failed = false;
            for (const auto& c : sys.events().all())
                if (c.type == "BOOT_FAILED" && c.causeId == e.causeId) failed = true;
            if (!failed) return 1;
        }
        return 0;
    }
    if (id == "not_today") {
        // A node recorded BOOT_FAILED and later reached KERNEL_UP.
        for (const auto& e : sys.events().all()) {
            if (e.type != "KERNEL_UP") continue;
            for (const auto& c : sys.events().all())
                if (c.type == "BOOT_FAILED" && c.target == e.target && c.id < e.id) return 1;
        }
        return 0;
    }
    if (id == "you_had_one_job") return msgHas(sys, "BOOT_FAILED", "/boot/kernel") ? 1 : 0;
    if (id == "module_missing") return msgHas(sys, "BOOT_FAILED", "/lib/modules") ? 1 : 0;
    if (id == "kernel_surgery") {
        // A kernel panicked and the same node runs a kernel again now.
        for (const auto& e : sys.events().all()) {
            if (e.type != "KERNEL_PANIC") continue;
            if (sys.hasNode(e.target) && sys.get(e.target).kernel == KernelState::RUNNING)
                return 1;
        }
        return 0;
    }
    if (id == "dynamic")
        return (hasType(sys, "MODULE_LOAD") && hasType(sys, "MODULE_UNLOAD")) ? 1 : 0;
    if (id == "panic_loop") return std::min(countType(sys, "KERNEL_PANIC"), 3);
    if (id == "back_from_dead") {
        // A FAILED -> ONLINE transition completed (from/to metadata).
        for (const auto& e : sys.events().all()) {
            auto fit = e.metadata.find("from"), tit = e.metadata.find("to");
            if (fit != e.metadata.end() && tit != e.metadata.end() && fit->second == "FAILED" &&
                tit->second == "ONLINE")
                return 1;
        }
        return 0;
    }
    if (id == "fs_explorer") return std::min<int>((int)obs.fsAreas.size(), 6);
    if (id == "touch_grass") return hasType(sys, "VFS_TOUCH") ? 1 : 0;
    if (id == "organized") return hasType(sys, "VFS_MKDIR") ? 1 : 0;
    if (id == "copycat") return hasType(sys, "VFS_COPY") ? 1 : 0;
    if (id == "move_it") return hasType(sys, "VFS_MOVE") ? 1 : 0;
    if (id == "oops") {
        // A removal followed later by a world restore.
        for (const auto& e : sys.events().all()) {
            if (e.type != "RESTORE") continue;
            for (const auto& c : sys.events().all())
                if (c.type == "VFS_REMOVE" && c.id < e.id) return 1;
        }
        return 0;
    }
    if (id == "corruption") return hasType(sys, "VFS_CORRUPT") ? 1 : 0;
    if (id == "root_energy") return hasType(sys, "SYSCTL_SET") ? 1 : 0;
    if (id == "read_only")
        return (hasType(sys, "VFS_DENIED") || hasType(sys, "CMD_READONLY_FS") ||
                hasType(sys, "CMD_FS_UNAVAILABLE"))
                   ? 1
                   : 0;
    if (id == "restored") return hasType(sys, "RESTORE") ? 1 : 0;
    if (id == "process_engineer")
        return (hasType(sys, "PROC_SPAWNED") && obs.procInspected) ? 1 : 0;
    if (id == "threads") return hasType(sys, "PROC_THREAD_SPAWNED") ? 1 : 0;
    if (id == "zombie_apocalypse") return std::min(countType(sys, "PROC_ZOMBIE"), 5);
    if (id == "orphaned") return msgHas(sys, "PROC_ZOMBIE", "parent reaped") ? 1 : 0;
    if (id == "terminated")
        return (hasType(sys, "PROC_OOM_KILL") || hasType(sys, "PROC_KILLED")) ? 1 : 0;
    if (id == "signal_fire") return hasType(sys, "USER_SIGNAL") ? 1 : 0;
    if (id == "pid_1") return hasMeta(sys, "CMD_OK", "binary", "init") ? 1 : 0;
    if (id == "service_starter") return hasType(sys, "SERVICE_STARTED") ? 1 : 0;
    if (id == "service_stopper") return hasType(sys, "SERVICE_STOPPED") ? 1 : 0;
    if (id == "supervisor") {
        // Same service crashed, then started again later.
        for (const auto& e : sys.events().all()) {
            if (e.type != "SERVICE_STARTED") continue;
            auto it = e.metadata.find("service");
            if (it == e.metadata.end()) continue;
            for (const auto& c : sys.events().all()) {
                if (c.type != "SERVICE_CRASHED" || c.id >= e.id) continue;
                auto jt = c.metadata.find("service");
                if (jt != c.metadata.end() && jt->second == it->second) return 1;
            }
        }
        return 0;
    }
    if (id == "service_cascade") return hasMeta(sys, "FAULT_RAISED", "fault", "service-down")
                                          ? 1
                                          : 0;
    if (id == "bad_binary") return msgHas(sys, "SERVICE_START_FAILED", "no such binary")
                                        ? 1
                                        : 0;
    if (id == "connected") return (obs.pingOk || obs.packetOk) ? 1 : 0;
    if (id == "disconnected")
        return (hasType(sys, "LINK_DOWN") || hasType(sys, "IFACE_DOWN")) ? 1 : 0;
    if (id == "back_online") {
        bool down = hasType(sys, "LINK_DOWN") || hasType(sys, "IFACE_DOWN");
        bool up = hasType(sys, "LINK_UP") || hasType(sys, "IFACE_UP");
        return (down && up) ? 1 : 0;
    }
    if (id == "packet_loss") return hasType(sys, "PACKET_LOSS") ? 1 : 0;
    if (id == "latency")
        return (hasType(sys, "LATENCY_SPIKE") || hasType(sys, "REQUEST_SLOW")) ? 1 : 0;
    if (id == "network_cascade") return msgHas(sys, "CLIENT_DEGRADED", "link to dependency")
                                            ? 1
                                            : 0;
    if (id == "too_hot") {
        for (const auto& name : sys.nodeNames())
            if (sys.get(name).tempC >= sys.get(name).criticalC) return 1;
        return 0;
    }
    if (id == "memory_pressure") return std::min(countType(sys, "MEMORY_PRESSURE"), 3);
    if (id == "storage_crisis") return hasMeta(sys, "FAULT_RAISED", "fault", "disk-full")
                                            ? 1
                                            : 0;
    if (id == "resourceful") {
        // A pressure fault resolved with no reboot between raise and resolve.
        for (const auto& e : sys.events().all()) {
            if (e.type != "FAULT_RESOLVED") continue;
            auto fit = e.metadata.find("fault");
            if (fit == e.metadata.end()) continue;
            const std::string& fn = fit->second;
            if (fn != "overheat" && fn != "mem-leak" && fn != "disk-full") continue;
            uint64_t raised = 0;
            for (const auto& c : sys.events().all()) {
                if (c.type != "FAULT_RAISED" || c.id >= e.id) continue;
                auto mt = c.metadata.find("fault");
                if (mt != c.metadata.end() && mt->second == fn && c.target == e.target)
                    raised = c.id;
            }
            if (raised == 0) continue;
            bool rebooted = false;
            for (const auto& c : sys.events().all())
                if (c.type == "USER_REBOOT" && c.id > raised && c.id < e.id) rebooted = true;
            if (!rebooted) return 1;
        }
        return 0;
    }
    if (id == "time_traveler") return hasType(sys, "RESTORE") ? 1 : 0;
    if (id == "butterfly") {
        std::map<uint64_t, int> fanout;
        for (const auto& e : sys.events().all()) {
            if (e.causeId == 0) continue;
            if (++fanout[e.causeId] >= 5) return 1;
        }
        return 0;
    }
    if (id == "cause_effect") {
        if (obs.subsystems.count("causality") == 0) return 0;
        for (const auto& e : sys.events().all())
            if (e.causeId != 0) return 1;
        return 0;
    }
    return 0;
}

std::vector<std::string> AchievementEngine::evaluate(const System& sys,
                                                     const ShellObserved& obs) {
    std::vector<std::string> fresh;
    for (size_t i = 0; i < kDefCount; ++i) {
        const AchievementDef& d = kDefs[i];
        AchievementState& st = states_[d.id];
        int p = progressFor(d.id, sys, obs);
        if (p > st.progress) st.progress = std::min(p, d.target);
        if (!st.unlocked && st.progress >= d.target) {
            st.unlocked = true;
            st.firstTick = (int64_t)sys.clock().tickCount();
            // Each unlock is queued exactly once; already-unlocked never refire.
            if (std::find(queue_.begin(), queue_.end(), d.id) == queue_.end())
                queue_.push_back(d.id);
            fresh.push_back(d.id);
        }
    }
    watermark_ = sys.events().size();
    (void)watermark_;
    return fresh;
}

std::vector<std::string> AchievementEngine::drainNotifications() {
    std::vector<std::string> out = queue_;
    queue_.clear();
    return out;
}

void AchievementEngine::serializeDoc(ordc::OrdDoc& doc) const {
    doc = ordc::OrdDoc{};
    doc.type = "achievements";
    doc.id = "global";
    for (size_t i = 0; i < kDefCount; ++i) {
        const AchievementDef& d = kDefs[i];
        const AchievementState& st = states_.at(d.id);
        std::string sec = std::string("achievement.") + d.id;
        doc.set(sec, "unlocked", st.unlocked ? "true" : "false");
        doc.set(sec, "progress", std::to_string(st.progress));
        doc.set(sec, "target", std::to_string(d.target));
        doc.set(sec, "first_tick", std::to_string(st.firstTick));
    }
}

bool AchievementEngine::deserializeDoc(const ordc::OrdDoc& doc, std::string& err) {
    if (doc.type != "achievements") {
        err = "not an achievements document (type = " + doc.type + ")";
        return false;
    }
    ordc::OrdError oerr;
    oerr.file = "achievement.ord";
    std::map<std::string, AchievementState> loaded;
    for (size_t i = 0; i < kDefCount; ++i) {
        const AchievementDef& d = kDefs[i];
        std::string sec = std::string("achievement.") + d.id;
        bool unlocked = false;
        int progress = 0, target = 0;
        int64_t first = -1;
        if (!ordc::getBool(doc, sec, "unlocked", unlocked, oerr)) {
            err = oerr.str();
            return false;
        }
        if (!ordc::getInt(doc, sec, "progress", progress, oerr)) {
            err = oerr.str();
            return false;
        }
        if (!ordc::getInt(doc, sec, "target", target, oerr)) {
            err = oerr.str();
            return false;
        }
        if (target != d.target) {
            err = oerr.file + ":0: [" + sec + "] target mismatch (file " +
                  std::to_string(target) + " != engine " + std::to_string(d.target) + ")";
            return false;
        }
        if (!ordc::getInt64(doc, sec, "first_tick", first, oerr)) {
            err = oerr.str();
            return false;
        }
        // Clamp defensively: progress never exceeds target, locked implies
        // zero-or-partial progress, unlocked implies full progress.
        AchievementState st;
        st.progress = std::clamp(progress, 0, d.target);
        st.unlocked = unlocked && (st.progress >= d.target);
        if (st.unlocked != unlocked) {
            err = oerr.file + ":0: [" + sec + "] inconsistent unlock state";
            return false;
        }
        st.firstTick = st.unlocked ? first : -1;
        loaded[d.id] = st;
    }
    states_ = loaded;
    queue_.clear(); // notifications are transient: never persisted
    return true;
}

std::string AchievementEngine::renderPage(int page, int perPage) const {
    if (perPage < 1) perPage = 1;
    int pages = achievementPageCount(kDefCount, perPage);
    if (page < 0) page = 0;
    if (page >= pages) page = pages - 1;
    // Inner width is 60 between the '|' walls; longer text is truncated so
    // the box never overflows a narrow terminal.
    auto cell = [](const std::string& s, size_t w) {
        if (s.size() > w) return s.substr(0, w);
        return s + std::string(w - s.size(), ' ');
    };
    std::ostringstream o;
    o << "+------------------------------------------------------------+\n";
    o << "|                     ACHIEVEMENTS                           |\n";
    {
        std::ostringstream mid;
        mid << unlockedCount() << " / " << kDefCount;
        o << "|" << cell(mid.str(), 60) << "|\n";
    }
    o << "+------------------------------------------------------------+\n";
    int start = page * perPage, end = std::min<int>(start + perPage, (int)kDefCount);
    for (int i = start; i < end; ++i) {
        const AchievementDef& d = kDefs[i];
        const AchievementState& st = states_.at(d.id);
        std::string mark = st.unlocked ? "[X]" : (d.hidden ? "[?]" : "[ ]");
        std::string title = (st.unlocked || !d.hidden) ? d.title : "HIDDEN";
        std::string desc = (st.unlocked || !d.hidden) ? d.desc : "Hidden achievement.";
        o << "|" << cell("", 60) << "|\n";
        o << "| " << mark << " " << cell(title, 55) << "|\n";
        o << "|     " << cell(desc, 55) << "|\n";
        if (d.target > 1 && !st.unlocked) {
            std::ostringstream pr;
            pr << "Progress: " << st.progress << " / " << d.target;
            o << "|     " << cell(pr.str(), 55) << "|\n";
        }
    }
    o << "|" << cell("", 60) << "|\n";
    o << "+------------------------------------------------------------+\n";
    {
        std::ostringstream foot;
        foot << "Page " << (page + 1) << " / " << pages;
        std::string f = foot.str(), r = "Left/Right      Q/E: Exit";
        size_t gap = (f.size() + r.size() + 4 <= 62) ? 58 - f.size() - r.size() : 1;
        o << "| " << f << std::string(gap, ' ') << r << " |\n";
    }
    o << "+------------------------------------------------------------+\n";
    return o.str();
}

std::string AchievementEngine::renderNotification(const std::string& id) const {
    const AchievementDef* found = nullptr;
    for (size_t i = 0; i < kDefCount; ++i)
        if (kDefs[i].id == id) found = &kDefs[i];
    std::string title = found ? found->title : id;
    std::string desc = found ? found->desc : "";
    auto center = [](const std::string& s, size_t w) {
        if (s.size() >= w) return s.substr(0, w);
        size_t pad = (w - s.size()) / 2;
        return std::string(pad, ' ') + s + std::string(w - pad - s.size(), ' ');
    };
    std::ostringstream o;
    o << "+--------------------------------------+\n";
    o << "|        ACHIEVEMENT UNLOCKED          |\n";
    o << "|" << center("", 38) << "|\n";
    o << "|" << center(title, 38) << "|\n";
    o << "|" << center("", 38) << "|\n";
    o << "|" << center(desc, 38) << "|\n";
    o << "+--------------------------------------+\n";
    return o.str();
}

int achievementPageCount(size_t total, int perPage) {
    if (perPage < 1) perPage = 1;
    return (int)((total + (size_t)perPage - 1) / (size_t)perPage);
}

NavAction viewerKey(AchievementNav& nav, int key) {
    // Arrow keys arrive as escape sequences (POSIX "\x1b[D"/"[C") or prefix
    // codes (Windows 224/75 left, 224/77 right); plain q/e/Q/E exit.
    if (key == 'q' || key == 'Q' || key == 'e' || key == 'E') return NavAction::Exit;
    if (key == 0) return NavAction::None; // sequence prefix: wait for more
    if (key == 'D' || key == 75) {        // Left
        if (nav.page > 0) --nav.page;
        return NavAction::Prev;
    }
    if (key == 'C' || key == 77) { // Right
        if (nav.page < nav.pages - 1) ++nav.page;
        return NavAction::Next;
    }
    return NavAction::None;
}

} // namespace override
