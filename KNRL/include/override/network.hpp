#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace override {

struct Link {
    std::string a;
    std::string b;
    int latencyMs = 10;
    double lossPct = 0.0;
    double loadPct = 20.0; // congestion 0..100: raises packet delay/loss
    bool up = true;
};

class Network {
public:
    void ensureLink(const std::string& a, const std::string& b, int latencyMs = 10);
    bool hasLink(const std::string& a, const std::string& b) const;
    bool setUp(const std::string& a, const std::string& b, bool up);
    bool setLatency(const std::string& a, const std::string& b, int ms);
    bool setLoss(const std::string& a, const std::string& b, double pct);
    bool removeLink(const std::string& a, const std::string& b);
    const Link* find(const std::string& a, const std::string& b) const;
    Link* find(const std::string& a, const std::string& b);
    std::vector<Link> links() const;
    std::vector<Link> linksFor(const std::string& node) const;
    void clear() { links_.clear(); }

private:
    static std::pair<std::string, std::string> key(const std::string& a, const std::string& b) {
        return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
    }
    std::map<std::pair<std::string, std::string>, Link> links_;
};

} // namespace override
