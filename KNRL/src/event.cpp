#include "override/event.hpp"

#include <unordered_set>

namespace override {

Event& EventLog::emit(uint64_t tick, std::string type, std::string source,
                       std::string target, uint64_t causeId, std::string message,
                       std::map<std::string, std::string> metadata, std::string severity) {
    Event e;
    e.id = nextId_++;
    e.tick = tick;
    e.type = std::move(type);
    e.source = std::move(source);
    e.target = std::move(target);
    e.causeId = causeId;
    e.message = std::move(message);
    e.severity = std::move(severity);
    e.metadata = std::move(metadata);
    events_.push_back(std::move(e));
    return events_.back();
}

const Event* EventLog::find(uint64_t id) const {
    for (const auto& e : events_)
        if (e.id == id) return &e;
    return nullptr;
}

std::vector<const Event*> EventLog::forTarget(const std::string& target, int limit) const {
    std::vector<const Event*> out;
    for (const auto& e : events_)
        if (e.target == target) out.push_back(&e);
    if (limit >= 0 && (int)out.size() > limit)
        out.erase(out.begin(), out.end() - limit);
    return out;
}

std::vector<const Event*> EventLog::causeChain(uint64_t id) const {
    // Cycle-safe: ledger ids always increase with cause, but a visited set
    // guarantees termination even for a corrupted ledger.
    std::vector<const Event*> chain;
    std::unordered_set<uint64_t> seen;
    const Event* cur = find(id);
    while (cur && seen.insert(cur->id).second) {
        chain.push_back(cur);
        if (cur->causeId == 0) break;
        cur = find(cur->causeId);
    }
    return chain;
}

std::string eventScope(const Event& e) {
    const std::string& t = e.type;
    auto starts = [&](const char* p) { return t.rfind(p, 0) == 0; };
    if (starts("LINK_") || starts("PACKET_") || starts("ROUTE_")) return "LINK";
    if (starts("PROC_")) return "PROCESS";
    if (starts("SERVICE_") || starts("SVC_")) return "SERVICE";
    if (starts("VFS_") || starts("FS_")) return "VFS";
    if (starts("USER_")) return "SYSTEM";
    // Control-plane / reporting outcomes (explicit set; everything else that
    // the engine emits about world state defaults to NODE below).
    static const char* sys[] = {"WORLD_INIT",
                                "CHECKPOINT",
                                "RESTORE",
                                "SEED",
                                "AUTOFAULTS",
                                "AUTO_TICK",
                                "REPLAY",
                                "BENCHMARK",
                                "RACE",
                                "MODE_SWITCH",
                                "CLOCK_PAUSED",
                                "CLOCK_RESUMED",
                                "BELIEF_PLANTED",
                                "BELIEFS_CLEARED",
                                "CMD_OK",
                                "CMD_NOENT",
                                "CMD_PERM_DENIED",
                                "CMD_READONLY_FS",
                                "CMD_FS_UNAVAILABLE",
                                "CMD_ALLOC_FAILED",
                                "CMD_KERNEL_DOWN",
                                "CMD_HOST_DOWN",
                                "CMD_HOST_PAUSED",
                                "CREATE_REJECTED",
                                "OVERRIDE_REJECTED",
                                "INJECT_REJECTED",
                                "RECOVER_REJECTED",
                                "PROC_REJECTED",
                                "SERVICE_REJECTED",
                                "ENGINE_INVARIANT",
                                "DEVICE_UNAVAILABLE",
                                "PACKAGE_REJECTED",
                                nullptr};
    for (const char** s = sys; *s != nullptr; ++s) {
        if (t == *s) return "SYSTEM";
    }
    return "NODE";
}

} // namespace override
