#include "override/hardware.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace override {

std::string toString(Backend b) { return b == Backend::V ? "V" : "R"; }

Backend backendFromString(const std::string& s) {
    std::string u = s;
    for (auto& c : u) c = (char)std::toupper((unsigned char)c);
    if (u == "V" || u == "VIRTUAL") return Backend::V;
    if (u == "R" || u == "BACKING") return Backend::R;
    throw std::runtime_error("unknown backend '" + s + "' (V|R)");
}

std::string toString(OverflowPolicy p) {
    switch (p) {
        case OverflowPolicy::KEEP_R: return "keep-r";
        case OverflowPolicy::RECLAIM: return "reclaim";
        case OverflowPolicy::CANCEL: return "cancel";
    }
    return "keep-r";
}

OverflowPolicy overflowPolicyFromString(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)std::tolower((unsigned char)c);
    if (o == "keep-r" || o == "keep_r" || o == "keep") return OverflowPolicy::KEEP_R;
    if (o == "reclaim" || o == "migrate") return OverflowPolicy::RECLAIM;
    if (o == "cancel" || o == "refuse") return OverflowPolicy::CANCEL;
    throw std::runtime_error("unknown overflow policy '" + s + "'");
}

HardwareManager::HardwareManager() = default;

static int64_t ceilMb(int64_t bytes) { return bytes <= 0 ? 0 : (bytes + 1048575) / 1048576; }

// Resources with byte quotas ("ram"|"storage"), or "" when none applies.
static std::string quotaResource(const std::string& resource) {
    std::string r = resource;
    for (auto& c : r) c = (char)std::tolower((unsigned char)c);
    if (r == "ram" || r == "memory") return "ram";
    if (r == "storage" || r == "disk") return "storage";
    return "";
}

static int64_t rCapBytes(const HardwareManager& hw, const std::string& canon) {
    int64_t mb = (canon == "ram") ? (int64_t)hw.backing().ramMbCap
                                  : (int64_t)hw.backing().storageMbCap;
    return mb * 1048576;
}

// Bytes currently held in R for one resource (derived from stored blobs so
// usage can never drift from content).
static int64_t rUsedBytes(const BackingStore& b, const std::string& canon) {
    int64_t used = 0;
    for (const auto& [key, bytes] : b.blobs) {
        auto it = b.blobResource.find(key);
        if (it != b.blobResource.end() && it->second == canon)
            used += (int64_t)bytes.size();
    }
    return used;
}

bool HardwareManager::setBackend(const std::string& resource, Backend b, uint64_t tick,
                                 std::string& err) {
    std::string r = resource;
    for (auto& c : r) c = (char)std::tolower((unsigned char)c);
    if (r == "cpu") cpu_ = b;
    else if (r == "ram") ram_ = b;
    else if (r == "storage" || r == "disk") storage_ = b;
    else if (r == "net" || r == "network") net_ = b;
    else {
        err = "unknown resource '" + resource + "' (cpu|ram|storage|net)";
        return false;
    }
    (void)tick;
    return true;
}

bool HardwareManager::setPolicy(OverflowPolicy p, uint64_t tick) {
    policy_ = p;
    (void)tick;
    return true;
}

bool HardwareManager::setProfile(const HardwareProfile& p, std::string& err) {
    if (p.cpuModel.empty()) {
        err = "cpu model must be non-empty";
        return false;
    }
    if (p.cpuCores <= 0 || p.cpuCores > 1024) {
        err = "cpu cores must be within [1,1024]";
        return false;
    }
    if (p.ramMb <= 0 || p.storageMb <= 0) {
        err = "ram/storage capacities must be positive";
        return false;
    }
    profile_ = p;
    return true;
}

bool HardwareManager::setBackingCaps(int ramMbCap, int storageMbCap, std::string& err) {
    if (ramMbCap <= 0 || storageMbCap <= 0) {
        err = "backing caps must be positive";
        return false;
    }
    if (rUsedBytes(backing_, "ram") > (int64_t)ramMbCap * 1048576 ||
        rUsedBytes(backing_, "storage") > (int64_t)storageMbCap * 1048576) {
        err = "new caps are below current R usage";
        return false;
    }
    backing_.ramMbCap = ramMbCap;
    backing_.storageMbCap = storageMbCap;
    return true;
}

std::string HardwareManager::describe() const {
    std::ostringstream o;
    o << "hardware (simulated, no host access)\n";
    o << "  cpu:      " << profile_.cpuModel << " x" << profile_.cpuCores << " [" << toString(cpu_)
      << "]\n";
    o << "  ram:      " << profile_.ramMb << " MB [" << toString(ram_) << "]\n";
    o << "  storage:  " << profile_.storageMb << " MB [" << toString(storage_) << "]\n";
    o << "  net:      virtual network adapter [" << toString(net_) << "]\n";
    o << "  overflow: " << toString(policy_) << "\n";
    o << "  R backing: ram " << ceilMb(rUsedBytes(backing_, "ram")) << "/"
      << backing_.ramMbCap << " MB, storage " << ceilMb(rUsedBytes(backing_, "storage")) << "/"
      << backing_.storageMbCap << " MB (" << backing_.blobs.size() << " blob(s))";
    return o.str();
}

bool HardwareManager::checkWrite(const std::string& resource, int usedV, int want,
                                 std::string& action, std::string& err) {
    std::string canon = quotaResource(resource);
    if (canon.empty()) {
        err = "resource '" + resource + "' has no byte quota (ram|storage)";
        return false;
    }
    if (usedV < 0 || want < 0) {
        err = "usage sizes cannot be negative";
        return false;
    }
    int64_t cap = (canon == "ram") ? (int64_t)profile_.ramMb : (int64_t)profile_.storageMb;
    if ((int64_t)usedV + (int64_t)want <= cap) {
        action = "v";
        return true;
    }
    if (policy_ == OverflowPolicy::CANCEL) {
        err = "write of " + std::to_string(want) + "MB would exceed V " + canon + " capacity (" +
              std::to_string(cap) + "MB) under cancel policy";
        return false;
    }
    action = "r";
    return true;
}

bool HardwareManager::storeOverflow(const std::string& resource, const std::string& key,
                                    const std::string& bytes, std::string& err) {
    std::string canon = quotaResource(resource);
    if (canon.empty()) {
        err = "resource '" + resource + "' has no byte quota (ram|storage)";
        return false;
    }
    if (key.empty()) {
        err = "overflow key must be non-empty";
        return false;
    }
    auto tag = backing_.blobResource.find(key);
    if (tag != backing_.blobResource.end() && tag->second != canon) {
        err = "R blob key '" + key + "' is already held for " + tag->second +
              " (drop it first; no silent retagging)";
        return false;
    }
    int64_t cap = rCapBytes(*this, canon);
    int64_t used = rUsedBytes(backing_, canon);
    int64_t old = 0;
    auto bit = backing_.blobs.find(key);
    if (bit != backing_.blobs.end()) old = (int64_t)bit->second.size();
    if (used - old + (int64_t)bytes.size() > cap) {
        err = "R backing full for " + canon + " (key '" + key + "' would exceed cap)";
        return false;
    }
    backing_.blobs[key] = bytes;
    backing_.blobResource[key] = canon;
    return true;
}

bool HardwareManager::dropOverflow(const std::string& key, std::string& err) {
    auto bit = backing_.blobs.find(key);
    if (bit == backing_.blobs.end()) {
        err = "no R overflow held for key '" + key + "'";
        return false;
    }
    backing_.blobs.erase(bit);
    backing_.blobResource.erase(key);
    return true;
}

bool HardwareManager::placeWrite(const std::string& resource, int64_t vFreeBytes, int64_t wantBytes,
                                 const std::string& key, const std::string& bytes,
                                 std::string& action, std::string& err) {
    std::string canon = quotaResource(resource);
    if (canon.empty()) {
        err = "resource '" + resource + "' has no byte quota (ram|storage)";
        return false;
    }
    if (vFreeBytes < 0 || wantBytes < 0) {
        err = "sizes cannot be negative";
        return false;
    }
    if (key.empty()) {
        err = "overflow key must be non-empty";
        return false;
    }
    if (wantBytes <= vFreeBytes && policy_ != OverflowPolicy::RECLAIM) {
        action = "v";
        return true;
    }
    if (policy_ == OverflowPolicy::CANCEL) {
        err = "write of " + std::to_string(wantBytes) + "B exceeds V " + canon +
              " free space (" + std::to_string(vFreeBytes) + "B) under cancel policy";
        return false;
    }
    if (policy_ == OverflowPolicy::KEEP_R) {
        if (!storeOverflow(canon, key, bytes, err)) return false;
        action = "r";
        return true;
    }
    // RECLAIM: migrate back every R holding that fits in current V free
    // space, then require the write itself to fit in V. Simulated first so a
    // failed write leaves R untouched (no silent data loss: the VFS tree,
    // not R, is authoritative for content).
    std::vector<std::string> drop;
    int64_t free2 = vFreeBytes;
    {
        std::vector<std::pair<std::string, int64_t>> cands;
        for (const auto& [k, b] : backing_.blobs) {
            auto rit = backing_.blobResource.find(k);
            if (rit == backing_.blobResource.end() || rit->second != canon) continue;
            cands.push_back({k, (int64_t)b.size()});
        }
        std::sort(cands.begin(), cands.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second < b.second;
            return a.first < b.first;
        });
        for (const auto& [k, sz] : cands) {
            if (sz <= free2) {
                drop.push_back(k);
                free2 += sz;
            }
        }
    }
    if (wantBytes > free2) {
        err = "write of " + std::to_string(wantBytes) + "B exceeds V " + canon +
              " free space (" + std::to_string(vFreeBytes) +
              "B) even after reclaiming R holdings under reclaim policy";
        return false;
    }
    for (const auto& k : drop) {
        backing_.blobs.erase(k);
        backing_.blobResource.erase(k);
    }
    action = "v";
    return true;
}

bool HardwareManager::validateAll(std::string* err) const {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };
    if (profile_.cpuModel.empty()) return fail("hardware: cpu model must be non-empty");
    if (profile_.cpuCores <= 0 || profile_.cpuCores > 1024)
        return fail("hardware: cpu cores out of [1,1024]");
    if (profile_.ramMb <= 0 || profile_.storageMb <= 0)
        return fail("hardware: ram/storage capacities must be positive");
    if (backing_.ramMbCap <= 0 || backing_.storageMbCap <= 0)
        return fail("hardware: backing caps must be positive");
    for (const auto& [key, bytes] : backing_.blobs) {
        if (key.empty()) return fail("hardware: empty R blob key");
        auto rit = backing_.blobResource.find(key);
        if (rit == backing_.blobResource.end() || (rit->second != "ram" && rit->second != "storage"))
            return fail("hardware: R blob without resource tag");
    }
    for (const auto& [key, res] : backing_.blobResource) {
        if (backing_.blobs.count(key) == 0) return fail("hardware: dangling R blob tag");
        if (res != "ram" && res != "storage") return fail("hardware: bad R blob tag");
    }
    if (rUsedBytes(backing_, "ram") > (int64_t)backing_.ramMbCap * 1048576)
        return fail("hardware: R ram usage exceeds cap");
    if (rUsedBytes(backing_, "storage") > (int64_t)backing_.storageMbCap * 1048576)
        return fail("hardware: R storage usage exceeds cap");
    return true;
}

void HardwareManager::appendDigest(std::ostringstream& o) const {
    o << "hw(cpu=" << toString(cpu_) << ",ram=" << toString(ram_)
      << ",storage=" << toString(storage_) << ",net=" << toString(net_)
      << ",policy=" << toString(policy_) << ",model=" << profile_.cpuModel
      << ",cores=" << profile_.cpuCores << ",ramMb=" << profile_.ramMb
      << ",storageMb=" << profile_.storageMb << ",rRamCap=" << backing_.ramMbCap
      << ",rStorageCap=" << backing_.storageMbCap << ",blobs[";
    for (const auto& [key, bytes] : backing_.blobs) {
        auto rit = backing_.blobResource.find(key);
        std::string res = (rit == backing_.blobResource.end()) ? "?" : rit->second;
        o << key << "=" << res << ":" << bytes.size() << ":" << bytes << ";";
    }
    o << "]);";
}

void HardwareManager::restoreSnapshot(Backend cpu, Backend ram, Backend storage, Backend net,
                                      HardwareProfile profile, BackingStore backing,
                                      OverflowPolicy policy) {
    cpu_ = cpu;
    ram_ = ram;
    storage_ = storage;
    net_ = net;
    profile_ = profile;
    backing_ = backing;
    policy_ = policy;
}

} // namespace override
