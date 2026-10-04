#include "override/network.hpp"

namespace override {

void Network::ensureLink(const std::string& a, const std::string& b, int latencyMs) {
    if (a == b) return; // self-links are never valid topology; see System::validateAll
    auto k = key(a, b);
    auto it = links_.find(k);
    if (it == links_.end()) {
        Link l;
        l.a = a;
        l.b = b;
        l.latencyMs = latencyMs;
        links_[k] = l;
    }
}

bool Network::hasLink(const std::string& a, const std::string& b) const {
    return links_.count(key(a, b)) > 0;
}

bool Network::setUp(const std::string& a, const std::string& b, bool up) {
    if (a == b) return false;
    auto* l = find(a, b);
    if (!l) return false;
    l->up = up;
    return true;
}

bool Network::setLatency(const std::string& a, const std::string& b, int ms) {
    if (a == b) return false;
    auto* l = find(a, b);
    if (!l) return false;
    l->latencyMs = ms;
    return true;
}

bool Network::setLoss(const std::string& a, const std::string& b, double pct) {
    if (a == b) return false;
    auto* l = find(a, b);
    if (!l) return false;
    l->lossPct = pct;
    return true;
}

bool Network::removeLink(const std::string& a, const std::string& b) {
    if (a == b) return false;
    return links_.erase(key(a, b)) > 0;
}

const Link* Network::find(const std::string& a, const std::string& b) const {
    auto it = links_.find(key(a, b));
    return it == links_.end() ? nullptr : &it->second;
}

Link* Network::find(const std::string& a, const std::string& b) {
    auto it = links_.find(key(a, b));
    return it == links_.end() ? nullptr : &it->second;
}

std::vector<Link> Network::links() const {
    std::vector<Link> out;
    for (const auto& [k, l] : links_) out.push_back(l);
    return out;
}

std::vector<Link> Network::linksFor(const std::string& node) const {
    std::vector<Link> out;
    for (const auto& [k, l] : links_)
        if (l.a == node || l.b == node) out.push_back(l);
    return out;
}

} // namespace override
