#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace override {

// A single causal event in the simulation.
struct Event {
    uint64_t id = 0;
    uint64_t tick = 0;
    std::string type;     // e.g. USER_BREAK, SERVER_FAILED, CLIENT_DEGRADED
    std::string source;   // who caused it ("user", "server", "engine"...)
    std::string target;   // which node it concerns
    uint64_t causeId = 0; // id of causing event (0 = root)
    std::string message;
    std::string severity = "INFO"; // INFO|WARNING|DEGRADED|CRITICAL|FAILED|PANIC|HALTED
    std::map<std::string, std::string> metadata;
};

class EventLog {
public:
    Event& emit(uint64_t tick, std::string type, std::string source,
                std::string target, uint64_t causeId, std::string message,
                std::map<std::string, std::string> metadata = {},
                std::string severity = "INFO");

    const std::vector<Event>& all() const { return events_; }
    const Event* find(uint64_t id) const;
    std::vector<const Event*> forTarget(const std::string& target, int limit = -1) const;
    // Follow cause links from `id` back to the root. Returns chain leaf->...->root.
    std::vector<const Event*> causeChain(uint64_t id) const;
    void clear() { events_.clear(); nextId_ = 1; }
    size_t size() const { return events_.size(); }

private:
    std::vector<Event> events_;
    uint64_t nextId_ = 1;
};

// Event scope: what an event concerns. Node-scoped events render as
// `node->node` because source and target are both the affected node --
// never a self-link (self-links are rejected by the topology). Link events
// name real endpoints ("a <-> b"). Interface state (IFACE_*) is node-scoped:
// it targets the node that owns the interface. Deterministic, derived from
// the type vocabulary.
std::string eventScope(const Event& e);

} // namespace override
