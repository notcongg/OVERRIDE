#include "override/system.hpp"

#include <sstream>

namespace override {

// ---- virtual hardware (V/R model; simulation only, never host access) ----

std::string System::hardwareDescribe() const { return hw_.describe(); }

OpResult System::hardwareBackend(const std::string& resource, const std::string& backend) {
    Backend b;
    try {
        b = backendFromString(backend);
    } catch (const std::exception&) {
        return OpResult::failure("unknown backend '" + backend + "' (V|R)");
    }
    uint64_t root = emit(0, "USER_HARDWARE", "user", "system",
                         "hardware backend " + resource + " -> " + toString(b));
    std::string err;
    if (!hw_.setBackend(resource, b, clock_.tickCount(), err)) {
        emit(root, "HARDWARE_REJECTED", "engine", "system", err);
        return OpResult::failure(err, root);
    }
    emit(root, "HARDWARE_BACKEND", "engine", "system",
         "hardware backend for " + resource + " now " + toString(b));
    return OpResult::success(root, resource + " backend: " + toString(b));
}

OpResult System::hardwareOverflow(const std::string& policy) {
    OverflowPolicy p;
    try {
        p = overflowPolicyFromString(policy);
    } catch (const std::exception&) {
        return OpResult::failure("unknown overflow policy '" + policy + "' (keep-r|reclaim|cancel)");
    }
    uint64_t root =
        emit(0, "USER_HARDWARE", "user", "system", "hardware overflow -> " + toString(p));
    hw_.setPolicy(p, clock_.tickCount());
    emit(root, "HARDWARE_POLICY", "engine", "system",
         "overflow policy now " + toString(p));
    return OpResult::success(root, std::string("overflow policy: ") + toString(p));
}

OpResult System::setHardwareProfile(const HardwareProfile& profile) {
    std::string err;
    if (!hw_.setProfile(profile, err)) return OpResult::failure(err);
    uint64_t root = emit(0, "USER_HARDWARE", "user", "system", "hardware profile provisioned");
    emit(root, "HARDWARE_PROFILE", "engine", "system",
         "profile: " + profile.cpuModel + " x" + std::to_string(profile.cpuCores) + ", " +
             std::to_string(profile.ramMb) + "MB RAM, " + std::to_string(profile.storageMb) +
             "MB storage");
    return OpResult::success(root, "hardware profile provisioned");
}

std::string System::hwAccountWrite(const std::string& abs, const std::string& content,
                                   std::string& actionOut) {
    // Stored writes need the block device; metadata-only ops do not.
    std::string devWhy;
    if (!deviceReady("disk0", devWhy)) {
        actionOut.clear();
        return "device /dev/disk0 unavailable (" + devWhy + ")";
    }
    // Quota pool follows the storage backend: V uses the virtual profile,
    // R uses the isolated backing caps. Usage is current VFS bytes.
    int64_t capMb = (hw_.storage() == Backend::V) ? (int64_t)hw_.profile().storageMb
                                                  : (int64_t)hw_.backing().storageMbCap;
    int64_t vFree = capMb * 1048576 - (int64_t)vfs_.totalBytes();
    std::string err;
    if (!hw_.placeWrite("storage", vFree, (int64_t)content.size(), abs, content, actionOut,
                        err)) {
        actionOut.clear();
        return err;
    }
    return "";
}

} // namespace override
