#pragma once

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace override {

// Virtual-hardware backend selection (simulation state, never host access).
// V = virtual/native simulated resource. R = isolated in-RAM backing area
// owned by the simulation (conceptually OVERRIDE runtime/backing/storage).
// R never means the host filesystem, host processes, sockets, or permissions.
enum class Backend { V, R };

std::string toString(Backend b);
Backend backendFromString(const std::string& s); // throws on unknown (V|R)

// Overflow policy for V-capacity exhaustion (explicit, no silent deletion).
// KEEP_R: keep overflow bytes in isolated R backing.
// RECLAIM: reclaim V usage then migrate R bytes back when they fit.
// CANCEL: refuse the write that would overflow V.
enum class OverflowPolicy { KEEP_R, RECLAIM, CANCEL };

std::string toString(OverflowPolicy p);
OverflowPolicy overflowPolicyFromString(const std::string& s); // throws

struct HardwareProfile {
    std::string cpuModel = "Virtual Intel Core i5 Gen 8";
    int cpuCores = 4;
    int ramMb = 4096;      // V RAM capacity
    int storageMb = 65536; // V storage capacity (64 GB)
};

// Isolated R backing: pure simulation counters/bytes. No host paths.
// R usage is always derived from stored blobs, never cached separately.
struct BackingStore {
    int ramMbCap = 20480; // 20 GB conceptual R RAM backing
    int storageMbCap = 204800;
    std::map<std::string, std::string> blobs; // key -> bytes (VFS overflow etc.)
    std::map<std::string, std::string> blobResource; // key -> "ram"|"storage"
};

class HardwareManager {
public:
    HardwareManager();

    Backend cpu() const { return cpu_; }
    Backend ram() const { return ram_; }
    Backend storage() const { return storage_; }
    Backend net() const { return net_; }
    const HardwareProfile& profile() const { return profile_; }
    const BackingStore& backing() const { return backing_; }
    BackingStore& backing() { return backing_; }
    OverflowPolicy policy() const { return policy_; }

    bool setBackend(const std::string& resource, Backend b, uint64_t tick, std::string& err);
    bool setPolicy(OverflowPolicy p, uint64_t tick);
    // Simulated hardware provisioning (e.g. smaller profiles for tests).
    bool setProfile(const HardwareProfile& p, std::string& err);
    bool setBackingCaps(int ramMbCap, int storageMbCap, std::string& err);
    std::string describe() const;

    // Deterministic quota accounting for simulated RAM/storage writes.
    // Pure predicate in megabytes: does usedV+want fit the V capacity?
    // action is "v" (fits) or "r" (would overflow; caller applies policy).
    bool checkWrite(const std::string& resource, int usedV, int want, std::string& action,
                    std::string& err);
    // Record bytes held in isolated R backing (no host I/O).
    // Replaces any blob already held under key (same resource only).
    bool storeOverflow(const std::string& resource, const std::string& key,
                       const std::string& bytes, std::string& err);
    bool dropOverflow(const std::string& key, std::string& err);
    // Enforce the overflow policy for a write. Decides AND mutates only the
    // isolated R store: action is "v" (stayed virtual) or "r" (kept in R).
    // RECLAIM first migrates back R blobs that fit, then requires V fit;
    // CANCEL refuses outright. Never silently loses data: a failed write
    // leaves R untouched.
    bool placeWrite(const std::string& resource, int64_t vFreeBytes, int64_t wantBytes,
                    const std::string& key, const std::string& bytes, std::string& action,
                    std::string& err);
    bool validateAll(std::string* err) const;
    void appendDigest(std::ostringstream& o) const;
    void restoreSnapshot(Backend cpu, Backend ram, Backend storage, Backend net,
                         HardwareProfile profile, BackingStore backing,
                         OverflowPolicy policy);

private:
    Backend cpu_ = Backend::V;
    Backend ram_ = Backend::V;
    Backend storage_ = Backend::V;
    Backend net_ = Backend::V;
    HardwareProfile profile_;
    BackingStore backing_;
    OverflowPolicy policy_ = OverflowPolicy::KEEP_R;
};

} // namespace override
