// System (network: links, routes, packets, ping, topology). Split from system.cpp; behavior unchanged.
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

OpResult System::connect(const std::string& a, const std::string& b) {
    if (!hasNode(a)) return OpResult::failure("unknown node: " + a);
    if (!hasNode(b)) return OpResult::failure("unknown node: " + b);
    if (a == b) return OpResult::failure("cannot connect a node to itself");
    uint64_t root = emit(0, "USER_CONNECT", "user", a, "connect " + a + " " + b);
    if (net_.hasLink(a, b)) {
        const Link* l = net_.find(a, b);
        if (l && l->up) {
            emit(root, "LINK_ACTIVE", "network", a, a + " <-> " + b + " already up");
            return OpResult::failure(a + " <-> " + b + " is already connected", root);
        }
        net_.setUp(a, b, true);
        emit(root, "LINK_UP", "network", a, a + " <-> " + b + " up");
    } else {
        net_.ensureLink(a, b, 10);
        // A new link also adopts a dependency edge a -> b.
        Node& na = get(a);
        if (std::find(na.dependencies.begin(), na.dependencies.end(), b) == na.dependencies.end())
            na.dependencies.push_back(b);
        emit(root, "LINK_CREATED", "network", a, a + " <-> " + b + " created");
    }
    propagate(root);
    return OpResult::success(root, a + " <-> " + b + " connected");
}

OpResult System::disconnect(const std::string& a, const std::string& b) {
    uint64_t root = emit(0, "USER_DISCONNECT", "user", a, "disconnect " + a + " " + b);
    const Link* l = net_.find(a, b);
    if (!l) {
        emit(root, "LINK_NOT_FOUND", "network", a, "no link " + a + " <-> " + b);
        return OpResult::failure("no link " + a + " <-> " + b, root);
    }
    if (!l->up) {
        emit(root, "LINK_ALREADY_DOWN", "network", a, a + " <-> " + b + " already down");
        return OpResult::failure(a + " <-> " + b + " is already down", root);
    }
    net_.setUp(a, b, false);
    emit(root, "LINK_DOWN", "network", a, a + " <-> " + b + " down");
    propagate(root);
    return OpResult::success(root, a + " <-> " + b + " disconnected");
}

bool System::routable(const Node& n) {
    return n.state == NodeState::ONLINE || n.state == NodeState::DEGRADED ||
           n.state == NodeState::WARNING || n.state == NodeState::CRITICAL;
}

std::vector<std::string> System::findRoute(const std::string& from,
                                           const std::string& to) const {
    // BFS over UP links, deterministic neighbour order, avoiding unhealthy
    // transit nodes. Returns the node path inclusive, or empty if unreachable.
    std::map<std::string, std::string> prev;
    std::queue<std::string> q;
    std::set<std::string> seen{from};
    q.push(from);
    while (!q.empty()) {
        std::string cur = q.front();
        q.pop();
        if (cur == to) break;
        auto links = net_.linksFor(cur);
        std::sort(links.begin(), links.end(), [](const Link& a, const Link& b) {
            std::string na = a.a, nb = b.a;
            return na < nb;
        });
        // linksFor order above is arbitrary; sort by neighbour name instead.
        std::vector<std::string> nbrs;
        for (const auto& l : links) {
            if (!l.up) continue;
            nbrs.push_back(l.a == cur ? l.b : l.a);
        }
        std::sort(nbrs.begin(), nbrs.end());
        for (const auto& nb : nbrs) {
            if (seen.count(nb) || !hasNode(nb)) continue;
            if (nb != to && !routable(get(nb))) continue; // no transit via down nodes
            seen.insert(nb);
            prev[nb] = cur;
            q.push(nb);
        }
    }
    if (!seen.count(to)) return {};
    std::vector<std::string> path{to};
    while (path.back() != from) {
        auto it = prev.find(path.back());
        if (it == prev.end()) return {};
        path.push_back(it->second);
    }
    std::reverse(path.begin(), path.end());
    return path;
}

PacketResult System::sendPacket(const std::string& from, const std::string& to) const {
    PacketResult r;
    if (!hasNode(from)) {
        r.detail = "unknown source: " + from;
        return r;
    }
    if (!hasNode(to)) {
        r.detail = "unknown destination: " + to;
        return r;
    }
    const Node& src = get(from);
    const Node& dst = get(to);
    if (from == to) {
        r.path = {from};
        if (!routable(src) && src.state != NodeState::RESTARTING) {
            r.outcome = PacketResult::Outcome::DROPPED;
            r.detail = "node " + toString(src.state) + " (loopback)";
            return r;
        }
        r.outcome = PacketResult::Outcome::DELIVERED;
        r.detail = "loopback";
        return r;
    }
    // Only serving states can send or receive; anything else is DROPPED,
    // never silently treated as healthy.
    auto serving = [](NodeState s) {
        return s == NodeState::ONLINE || s == NodeState::DEGRADED || s == NodeState::WARNING ||
               s == NodeState::CRITICAL;
    };
    if (!serving(src.state)) {
        r.outcome = PacketResult::Outcome::DROPPED;
        r.detail = "source " + from + " is " + toString(src.state);
        return r;
    }
    if (!serving(dst.state)) {
        r.outcome = PacketResult::Outcome::DROPPED;
        r.detail = "destination " + to + " is " + toString(dst.state);
        return r;
    }
    std::string devWhy;
    if (!deviceReady("net0", devWhy)) {
        r.outcome = PacketResult::Outcome::DROPPED;
        r.detail = "device /dev/net0 unavailable (" + devWhy + ")";
        return r;
    }
    // Connection exhaustion: a full table cannot accept new packets.
    if (dst.connections >= dst.maxConnections) {
        r.outcome = PacketResult::Outcome::DROPPED;
        r.detail = "destination " + to + " connection table exhausted (" +
                   std::to_string(dst.connections) + "/" + std::to_string(dst.maxConnections) +
                   ")";
        return r;
    }
    auto path = findRoute(from, to);
    if (path.empty()) {
        r.detail = "no route from " + from + " to " + to;
        return r;
    }
    noteSyscall("sendmsg");
    r.path = path;
    int lat = 0;
    double maxLoss = 0.0;
    double maxLoad = 0.0;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const Link* l = net_.find(path[i], path[i + 1]);
        if (!l || !l->up) {
            r.path.clear();
            r.outcome = PacketResult::Outcome::UNREACHABLE;
            r.detail = "route broke mid-path";
            return r;
        }
        // Congestion: loaded links add delay (load/2 ms each).
        lat += l->latencyMs + (int)(l->loadPct / 2.0);
        maxLoss = std::max(maxLoss, l->lossPct);
        maxLoad = std::max(maxLoad, l->loadPct);
    }
    // Congested links also drop: +1% effective loss per 4% load over 60%.
    double effLoss = maxLoss;
    if (maxLoad > 60.0) effLoss = std::min(100.0, effLoss + (maxLoad - 60.0) / 4.0);
    // Deterministic drop roll: pure function of (seed, tick, endpoints).
    // Same world state => same verdict, no RNG consumed.
    uint64_t roll = hash64(seed_ ^ (clock_.tickCount() * 0x9E3779B97F4A7C15ull) ^
                           fnv1a(from + ">" + to)) %
                    100;
    if ((double)roll < effLoss && effLoss < 100.0) {
        r.outcome = PacketResult::Outcome::DROPPED;
        r.detail = "lost in transit (congestion/loss)";
        r.latencyMs = lat;
        return r;
    }
    if (effLoss >= 100.0 || maxLoss >= 100.0) {
        r.outcome = PacketResult::Outcome::DROPPED;
        r.detail = "100% packet loss on path";
        r.latencyMs = lat;
        return r;
    }
    r.outcome = PacketResult::Outcome::DELIVERED;
    // CPU overload slows packet processing at both ends.
    r.latencyMs = lat + src.load / 5 + dst.load / 5;
    if (maxLoad > 60.0) {
        std::ostringstream d;
        d << "congested path (worst link load " << (int)maxLoad << "%)";
        if (maxLoss > 0.0) d << ", lossy (" << maxLoss << "%)";
        r.detail = d.str();
    } else if (maxLoss > 0.0) {
        std::ostringstream d;
        d << "lossy path (" << maxLoss << "% worst link, delivered this time)";
        r.detail = d.str();
    } else {
        r.detail = "ok";
    }
    return r;
}

std::string System::formatPacket(const std::string& from, const std::string& to,
                                 const PacketResult& r) {
    std::ostringstream o;
    o << "packet " << from << " -> " << to << ": ";
    switch (r.outcome) {
        case PacketResult::Outcome::DELIVERED:
            o << "DELIVERED latency=" << r.latencyMs << "ms hops=" << (r.path.size() - 1);
            if (!r.path.empty()) {
                o << " via ";
                for (size_t i = 0; i < r.path.size(); ++i) {
                    if (i) o << ">";
                    o << r.path[i];
                }
            }
            if (r.detail != "ok" && r.detail != "loopback") o << " (" << r.detail << ")";
            break;
        case PacketResult::Outcome::DROPPED: o << "DROPPED (" << r.detail << ")"; break;
        case PacketResult::Outcome::UNREACHABLE: o << "UNREACHABLE (" << r.detail << ")"; break;
    }
    return o.str();
}

std::string System::ping(const std::string& target, const std::string& observer) const {
    if (!hasNode(target)) return "error: unknown node: " + target;
    if (!observer.empty() && !hasNode(observer)) return "error: unknown node: " + observer;
    const Node& n = get(target);
    std::string src = observer.empty() ? "client" : observer;
    int lat = n.latencyMs;
    // Deception: an observer sees its believed latency.
    if (!observer.empty()) {
        const Node& o = get(observer);
        auto it = o.beliefs.find(target);
        if (it != o.beliefs.end()) {
            auto jt = it->second.find("latency");
            if (jt != it->second.end()) {
                try {
                    lat = parseMs(jt->second);
                } catch (...) {
                    lat = n.latencyMs;
                }
            }
        }
    }
    if (n.state == NodeState::FAILED || n.state == NodeState::CORRUPTED ||
        n.state == NodeState::PAUSED || n.state == NodeState::UNKNOWN)
        return "ping " + target + ": no reply (" + toString(n.state) + ")";
    if (!hasNode(src)) {
        // Default source was removed: direct assessment, no route.
        std::ostringstream o;
        o << "ping " << target << ": reply latency=" << lat << "ms state=" << toString(n.state)
          << " (no route: source missing)";
        return o.str();
    }
    const Node& s = get(src);
    if (s.state == NodeState::FAILED || s.state == NodeState::CORRUPTED ||
        s.state == NodeState::PAUSED || s.state == NodeState::UNKNOWN)
        return "ping " + target + ": source " + src + " is " + toString(s.state) +
               ", cannot send";
    std::string devWhy;
    if (!deviceReady("net0", devWhy))
        return "ping " + target + ": device /dev/net0 unavailable (" + devWhy + ")";
    auto path = findRoute(src, target);
    if (path.empty()) return "ping " + target + ": UNREACHABLE (no route from " + src + ")";
    double maxLoss = 0.0;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const Link* l = net_.find(path[i], path[i + 1]);
        if (l) maxLoss = std::max(maxLoss, l->lossPct);
    }
    if (maxLoss >= 100.0) return "ping " + target + ": 100% packet loss";
    std::ostringstream o;
    o << "ping " << target << ": reply latency=" << lat << "ms state=" << toString(n.state)
      << " hops=" << (path.size() - 1);
    return o.str();
}

std::string System::pingProbes(const std::string& target, const std::string& observer) const {
    // 4-probe statistical ping. Probe jitter/loss are pure functions of
    // (seed, tick, target, probe) -- deterministic, no RNG consumed.
    if (!hasNode(target)) return "error: unknown node: " + target;
    if (!observer.empty() && !hasNode(observer)) return "error: unknown node: " + observer;
    std::ostringstream o;
    o << "PING " << target << "\n";
    const Node& n = get(target);
    std::string src = observer.empty() ? "client" : observer;
    auto failAll = [&](const std::string& why) {
        for (int i = 1; i <= 4; ++i) o << i << "   [FAIL] " << why << "\n";
        o << "packet loss: 100%\n";
        return o.str();
    };
    if (n.state == NodeState::FAILED || n.state == NodeState::CORRUPTED ||
        n.state == NodeState::PAUSED || n.state == NodeState::UNKNOWN)
        return failAll("destination unavailable (" + toString(n.state) + ")");
    if (!hasNode(src)) return failAll("source missing (no route)");
    const Node& s = get(src);
    if (s.state == NodeState::FAILED || s.state == NodeState::CORRUPTED ||
        s.state == NodeState::PAUSED || s.state == NodeState::UNKNOWN)
        return failAll("source " + src + " is " + toString(s.state));
    std::string devWhy;
    if (!deviceReady("net0", devWhy))
        return failAll("device /dev/net0 unavailable (" + devWhy + ")");
    auto path = findRoute(src, target);
    if (path.empty()) return failAll("no route from " + src);
    int base = n.latencyMs;
    if (!observer.empty()) { // believed latency applies to observers
        const Node& ob = get(observer);
        auto it = ob.beliefs.find(target);
        if (it != ob.beliefs.end()) {
            auto jt = it->second.find("latency");
            if (jt != it->second.end()) {
                try {
                    base = parseMs(jt->second);
                } catch (...) {
                }
            }
        }
    }
    double maxLoss = 0.0;
    int pathLat = 0;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const Link* l = net_.find(path[i], path[i + 1]);
        if (l) {
            maxLoss = std::max(maxLoss, l->lossPct);
            pathLat += l->latencyMs + (int)(l->loadPct / 2.0);
        }
    }
    int lost = 0, sum = 0, got = 0;
    for (int i = 1; i <= 4; ++i) {
        uint64_t h = hash64(seed_ ^ (clock_.tickCount() * 31) ^ fnv1a(target) ^ (uint64_t)i);
        if ((h % 100) < (uint64_t)maxLoss) {
            o << i << "   [FAIL] timeout\n";
            ++lost;
            continue;
        }
        int probe = base + pathLat / 2 + (int)(h % 7) - 3; // +/-3ms deterministic jitter
        probe = std::max(1, probe);
        o << i << "   " << probe << "ms\n";
        sum += probe;
        ++got;
    }
    o << "packet loss: " << (lost * 100 / 4) << "%\n";
    if (got > 0) o << "avg: " << (sum / got) << "ms\n";
    return o.str();
}

// ---- time ----

std::string System::topology() const {
    std::ostringstream o;
    o << "topology (" << net_.links().size() << " links):\n";
    for (const auto& l : net_.links()) {
        o << "  " << l.a << " <-> " << l.b << " latency=" << l.latencyMs
          << "ms loss=" << l.lossPct << "% " << (l.up ? "UP" : "DOWN") << "\n";
    }
    o << "dependencies:\n";
    for (const auto& name : nodeNames()) {
        o << "  " << name << " -> ";
        if (get(name).dependencies.empty()) {
            o << "(none)";
        } else {
            bool first = true;
            for (auto& d : get(name).dependencies) {
                if (!first) o << ", ";
                o << d;
                first = false;
            }
        }
        o << "\n";
    }
    return o.str();
}


} // namespace override

