// System (simulated process operations). Split from system.cpp; behavior unchanged.
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

void System::emitProcNotes(const std::vector<ProcNote>& notes, uint64_t cause) {
    for (const auto& n : notes) emit(cause, n.type, n.source, n.target, n.message);
}

// ---- simulated processes ----

OpResult System::procSpawn(const std::string& name, const std::string& type,
                           const std::string& host, int parentPid) {
    std::string h = host.empty() ? "server" : host;
    if (!hasNode(h)) return OpResult::failure("unknown host node: " + h);
    if (parentPid != 0 && !pm_.find(std::to_string(parentPid)))
        return OpResult::failure("unknown parent pid: " + std::to_string(parentPid));
    uint64_t root = emit(0, "USER_PROC_SPAWN", "user", h, "process spawn " + name + " on " + h);
    std::vector<ProcNote> notes;
    std::string err;
    int pid = pm_.spawn(name, type, h, parentPid, notes, err);
    if (pid == 0) {
        emit(root, "PROC_REJECTED", "engine", h, err);
        return OpResult::failure(err, root);
    }
    noteSyscall("clone");
    emitProcNotes(notes, root);
    return OpResult::success(root, "spawned " + name + " (pid " + std::to_string(pid) + ") on " + h);
}

OpResult System::procKill(const std::string& pidOrName, const std::string& reason) {
    std::string r = reason;
    for (auto& c : r) c = (char)std::tolower((unsigned char)c);
    if (r != "crash" && r != "oom" && r != "segfault" && r != "runaway" && r != "deadlock" &&
        r != "sigkill" && r != "zombie" && r != "block" && r != "degrade")
        return OpResult::failure("unknown failure reason '" + reason +
                                 "' (crash|oom|segfault|runaway|deadlock|sigkill|zombie|block|"
                                 "degrade)");
    std::vector<ProcNote> notes;
    std::string err;
    // Pre-check for the starvation warning below.
    const SimProcess* before = pm_.find(pidOrName);
    std::string host = before ? before->host : "";
    uint64_t root = emit(0, "USER_PROC_KILL", "user", host.empty() ? "system" : host,
                         "process kill " + pidOrName + " (" + r + ")");
    if (!pm_.failAs(pidOrName, r, notes, err)) {
        emit(root, "PROC_REJECTED", "engine", host.empty() ? "system" : host, err);
        return OpResult::failure(err, root);
    }
    noteSyscall("kill");
    if (r == "oom" && !host.empty() && hasNode(host)) get(host).oomKills += 1;
    emitProcNotes(notes, root);
    std::string info = notes.empty() ? "killed" : notes.back().message;
    if (!host.empty() && hasNode(host) && pm_.runningOn(host) == 0 && pm_.totalOn(host) > 0) {
        emit(root, "NODE_STRAINED", "engine", host,
             host + " has no running processes; health will decay");
        info += " (WARNING: " + host + " has no running processes left)";
    }
    return OpResult::success(root, info);
}

OpResult System::procPause(const std::string& pidOrName) {
    std::vector<ProcNote> notes;
    std::string err;
    const SimProcess* before = pm_.find(pidOrName);
    std::string host = before ? before->host : "system";
    uint64_t root =
        emit(0, "USER_PROC_PAUSE", "user", host, "process pause " + pidOrName);
    if (!pm_.pause(pidOrName, notes, err)) {
        emit(root, "PROC_REJECTED", "engine", host, err);
        return OpResult::failure(err, root);
    }
    emitProcNotes(notes, root);
    return OpResult::success(root, notes.back().message);
}

OpResult System::procResume(const std::string& pidOrName) {
    std::vector<ProcNote> notes;
    std::string err;
    const SimProcess* before = pm_.find(pidOrName);
    std::string host = before ? before->host : "system";
    uint64_t root =
        emit(0, "USER_PROC_RESUME", "user", host, "process resume " + pidOrName);
    if (!pm_.resume(pidOrName, notes, err)) {
        emit(root, "PROC_REJECTED", "engine", host, err);
        return OpResult::failure(err, root);
    }
    emitProcNotes(notes, root);
    return OpResult::success(root, notes.back().message);
}

OpResult System::procRestart(const std::string& pidOrName) {
    std::vector<ProcNote> notes;
    std::string err;
    const SimProcess* before = pm_.find(pidOrName);
    std::string host = before ? before->host : "system";
    uint64_t root =
        emit(0, "USER_PROC_RESTART", "user", host, "process restart " + pidOrName);
    if (!pm_.restart(pidOrName, notes, err)) {
        emit(root, "PROC_REJECTED", "engine", host, err);
        return OpResult::failure(err, root);
    }
    emitProcNotes(notes, root);
    return OpResult::success(root, notes.back().message);
}

OpResult System::procSignal(const std::string& sig, const std::string& pidOrName) {
    // Signal vocabulary (both SIGTERM and TERM spellings accepted).
    std::string s = sig;
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    if (s.rfind("SIG", 0) == 0) s = s.substr(3);
    std::vector<ProcNote> notes;
    std::string err;
    const SimProcess* before = pm_.find(pidOrName);
    std::string host = before ? before->host : "system";
    uint64_t root =
        emit(0, "USER_SIGNAL", "user", host, "signal SIG" + s + " -> " + pidOrName);
    auto rejected = [&](const std::string& why) {
        emit(root, "PROC_REJECTED", "engine", host, why);
        return OpResult::failure(why, root);
    };
    bool ok = false;
    if (s == "TERM" || s == "STOP") {
        noteSyscall("kill");
        ok = pm_.pause(pidOrName, notes, err);
    } else if (s == "CONT") {
        noteSyscall("kill");
        ok = pm_.resume(pidOrName, notes, err);
    } else if (s == "KILL") {
        noteSyscall("kill");
        ok = pm_.failAs(pidOrName, "sigkill", notes, err);
    } else if (s == "HUP") {
        noteSyscall("kill");
        ok = pm_.restart(pidOrName, notes, err);
    } else {
        return rejected("unknown signal '" + sig + "' (TERM|STOP|CONT|KILL|HUP)");
    }
    if (!ok) return rejected(err);
    emitProcNotes(notes, root);
    emit(root, "SIGNAL_DELIVERED", before ? before->name : pidOrName, host,
         "SIG" + s + " delivered to " + pidOrName, {{"signal", s}});
    return OpResult::success(root, notes.back().message);
}

OpResult System::threadSpawn(const std::string& pidOrName, const std::string& tname) {
    std::string low = tname;
    for (auto& c : low) c = (char)std::tolower((unsigned char)c);
    const SimProcess* before = pm_.find(pidOrName);
    std::string host = before ? before->host : "system";
    uint64_t root = emit(0, "USER_THREAD_SPAWN", "user", host,
                         "thread spawn " + low + " in " + pidOrName);
    std::vector<ProcNote> notes;
    std::string err;
    noteSyscall("clone");
    int tid = pm_.threadSpawn(pidOrName, low, notes, err);
    if (tid == 0) {
        emit(root, "PROC_REJECTED", "engine", host, err);
        return OpResult::failure(err, root);
    }
    emitProcNotes(notes, root);
    return OpResult::success(root, notes.back().message);
}

OpResult System::ipcSend(const std::string& proc, const std::string& message) {
    SimProcess* p = pm_.find(proc);
    if (!p) return OpResult::failure("unknown process: " + proc);
    if (message.size() > 256) return OpResult::failure("message too long (max 256B)");
    uint64_t root = emit(0, "USER_IPC_SEND", "user", p->host, "ipc send " + p->name);
    if (p->state == ProcState::TERMINATED || p->state == ProcState::KILLED) {
        emit(root, "IPC_REJECTED", "engine", p->host,
             "ipc to " + p->name + " refused: process " + toString(p->state));
        return OpResult::failure("process " + toString(p->state) + ": " + p->name, root);
    }
    if (p->mailbox.size() >= ProcessManager::kMaxMailbox) {
        emit(root, "IPC_REJECTED", "engine", p->host,
             "ipc to " + p->name + " refused: mailbox full");
        return OpResult::failure("mailbox full: " + p->name, root);
    }
    noteSyscall("msgsnd");
    p->mailbox.push_back(message);
    emit(root, "IPC_SEND", p->name, p->host,
         "ipc: message queued for " + p->name + " (" +
             std::to_string(p->mailbox.size()) + " queued)",
         {{"to", p->name}});
    return OpResult::success(root, "message queued for " + p->name);
}

OpResult System::ipcRecv(const std::string& proc) {
    SimProcess* p = pm_.find(proc);
    if (!p) return OpResult::failure("unknown process: " + proc);
    uint64_t root = emit(0, "USER_IPC_RECV", "user", p->host, "ipc recv " + p->name);
    if (p->mailbox.empty()) {
        emit(root, "IPC_REJECTED", "engine", p->host,
             "ipc recv " + p->name + " refused: mailbox empty");
        return OpResult::failure("mailbox empty: " + p->name, root);
    }
    noteSyscall("msgrcv");
    std::string msg = p->mailbox.front();
    p->mailbox.erase(p->mailbox.begin());
    emit(root, "IPC_RECV", p->name, p->host,
         "ipc: " + p->name + " received 1 message (" +
             std::to_string(p->mailbox.size()) + " left)",
         {{"from", p->name}});
    return OpResult::success(root, msg);
}

std::string System::ipcList(const std::string& proc) const {
    std::ostringstream o;
    o << "ipc mailboxes:\n";
    for (const auto& [pid, p] : pm_.all()) {
        if (!proc.empty() && std::to_string(pid) != proc && p.name != proc) continue;
        o << "  " << p.name << "(" << pid << "): " << p.mailbox.size() << " queued\n";
    }
    return o.str();
}


} // namespace override

