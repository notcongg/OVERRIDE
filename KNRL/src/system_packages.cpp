// System (recovery package loads: validated content, transactional apply).
// get/fetch only touches the package store (never the world, never the
// ledger); load is the operation that changes world state, and it commits
// all-or-nothing: every mutation is journaled with its inverse, and any
// failure unwinds the journal so the world (and its digest) is untouched.
#include "override/system.hpp"

#include <algorithm>

namespace override {

namespace {
// Rollback journal for package loads. apply() runs forward; unwind()
// replays inverses so a failed load leaves zero trace.
struct Journal {
    struct FilePrior {
        bool existed = false;
        std::string content;
        std::string owner;
        bool executable = false;
        bool corrupted = false;
        uint64_t mtime = 0;
        std::set<std::string> acl;
    };
    struct Entry {
        enum class Kind { BlobAdded, FileCreated, FileReplaced, DirCreated };
        Kind kind;
        std::string path; // blob key / file / dir
        FilePrior prior;  // FileReplaced only
    };
    std::vector<Entry> entries;

    void unwind(Vfs& vfs, HardwareManager& hw) {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            std::string dummy;
            switch (it->kind) {
                case Entry::Kind::BlobAdded: hw.dropOverflow(it->path, dummy); break;
                case Entry::Kind::FileCreated: vfs.remove(it->path, dummy); break;
                case Entry::Kind::FileReplaced: {
                    const FilePrior& p = it->prior;
                    vfs.write(it->path, p.content, p.owner, p.mtime, dummy, p.executable);
                    const VFile* f = vfs.file(it->path);
                    std::set<std::string> cur = f ? f->acl : std::set<std::string>{};
                    for (const auto& t : cur)
                        if (!p.acl.count(t)) vfs.chmod(it->path, false, t, dummy);
                    for (const auto& t : p.acl)
                        if (!cur.count(t)) vfs.chmod(it->path, true, t, dummy);
                    if (p.corrupted) vfs.corrupt(it->path, dummy);
                    break;
                }
                case Entry::Kind::DirCreated: vfs.remove(it->path, dummy); break;
            }
        }
        entries.clear();
    }
};
} // namespace

OpResult System::loadKernelImage(const std::string& bytes, ProgressCb progress) {
    if (bytes.empty()) return OpResult::failure("refusing to load an empty kernel image");
    auto stage = [&](int done, int total) {
        if (progress) progress(done, total);
    };
    uint64_t root = emit(0, "USER_PACKAGE", "user", "/boot/kernel", "load kernel package");
    const std::string path = "/boot/kernel";
    Journal journal;
    auto fail = [&](const std::string& why) {
        journal.unwind(vfs_, hw_);
        emit(root, "PACKAGE_REJECTED", "engine", path, "kernel load failed: " + why, {},
             "WARNING");
        return OpResult::failure(why, root);
    };
    // 1. Quota first: any failure precedes the tree mutation, and a stored
    //    R blob is journaled for unwind.
    std::string action;
    std::string qerr = hwAccountWrite(path, bytes, action);
    if (!qerr.empty()) return fail(qerr);
    if (action == "r") journal.entries.push_back({Journal::Entry::Kind::BlobAdded, path, {}});
    stage(1, 3);
    // 2. Parent must exist (recreate a deleted /boot the way rm allows).
    if (!vfs_.isDir("/boot")) {
        std::string derr;
        if (!vfs_.mkdir("/boot", derr, "", defaultAcl())) return fail(derr);
        journal.entries.push_back({Journal::Entry::Kind::DirCreated, "/boot", {}});
    }
    // 3. Commit the image (replace preserves owner/mode; create seeds them).
    //    The R blob (if any) was journaled above, so any write failure
    //    unwinds through the single fail() path below.
    {
        const VFile* f = vfs_.file(path);
        Journal::FilePrior prior;
        bool had = (f != nullptr);
        if (had) {
            prior = {true, f->content, f->owner, f->executable, f->corrupted, f->mtime,
                     f->acl};
        }
        std::string werr;
        if (!vfs_.write(path, bytes, "", clock_.tickCount(), werr)) return fail(werr);
        journal.entries.push_back(had ? Journal::Entry{Journal::Entry::Kind::FileReplaced,
                                                       path, prior}
                                      : Journal::Entry{Journal::Entry::Kind::FileCreated,
                                                       path, {}});
    }
    stage(2, 3);
    emit(root, "KERNEL_RESTORED", "engine", path,
         "kernel restored to /boot/kernel (" + std::to_string(bytes.size()) + "B)");
    stage(3, 3);
    return OpResult::success(root, "kernel restored to /boot/kernel.");
}

OpResult System::loadRootfsBaseline(const RootfsContent& content, ProgressCb progress) {
    auto stage = [&](int done, int total) {
        if (progress) progress(done, total);
    };
    const int totalUnits = (int)content.files.size() + 2;
    uint64_t root = emit(0, "USER_PACKAGE", "user", "vfs", "load rootfs package");
    Journal journal;
    auto fail = [&](const std::string& why) {
        journal.unwind(vfs_, hw_);
        emit(root, "PACKAGE_REJECTED", "engine", "vfs", "rootfs load failed: " + why, {},
             "WARNING");
        return OpResult::failure(why, root);
    };
    int done = 0;
    // 1. Recreate baseline dirs missing from the live tree (journaled).
    for (const auto& d : content.dirs) {
        if (d.path == "/") continue;
        if (vfs_.isDir(d.path)) continue;
        std::string derr;
        if (!vfs_.mkdir(d.path, derr, d.owner, d.acl)) return fail(derr);
        journal.entries.push_back({Journal::Entry::Kind::DirCreated, d.path, {}});
    }
    stage(++done, totalUnits);
    // 2. Overlay files (create-or-replace; replace clears corruption, keeps
    //    mode bits; quota runs per file ahead of its mutation, and every
    //    mutation is journaled, so any failure unwinds to zero trace).
    for (const auto& f : content.files) {
        const VFile* before = vfs_.file(f.path);
        Journal::FilePrior prior;
        bool had = (before != nullptr);
        if (had) {
            prior = {true, before->content, before->owner,     before->executable,
                     before->corrupted,     before->mtime,     before->acl};
        }
        std::string action, werr;
        // Account first so any quota failure precedes the tree mutation.
        std::string qerr = hwAccountWrite(f.path, f.content, action);
        if (!qerr.empty()) return fail(qerr);
        if (action == "r")
            journal.entries.push_back({Journal::Entry::Kind::BlobAdded, f.path, {}});
        if (!vfs_.write(f.path, f.content, f.owner, clock_.tickCount(), werr, f.executable))
            return fail(werr);
        // Fresh writes take seed mode bits; align stored owner/exec/acl.
        if (!had) {
            std::string dummy;
            const VFile* wf = vfs_.file(f.path);
            std::set<std::string> cur = wf ? wf->acl : std::set<std::string>{};
            for (const auto& t : cur)
                if (!f.acl.count(t)) vfs_.chmod(f.path, false, t, dummy);
            for (const auto& t : f.acl)
                if (!cur.count(t)) vfs_.chmod(f.path, true, t, dummy);
            journal.entries.push_back({Journal::Entry::Kind::FileCreated, f.path, {}});
        } else {
            journal.entries.push_back({Journal::Entry::Kind::FileReplaced, f.path, prior});
        }
        stage(++done, totalUnits);
    }
    // 3. A restored node-owned config genuinely repairs that condition:
    //    clear the flag + resolve the fault with standard causal events.
    int repaired = 0;
    for (const auto& f : content.files) {
        if (f.path.rfind("/etc/", 0) != 0) continue;
        std::string owner = inferOwner(f.path);
        if (owner.empty() || !hasNode(owner)) continue;
        Node& n = get(owner);
        if (!n.configCorrupt && n.faults.count("config-corrupt") == 0) continue;
        n.configCorrupt = false;
        if (n.faults.erase("config-corrupt") > 0) {
            emit(root, "FAULT_RESOLVED", "engine", owner,
                 owner + " fault 'config-corrupt' resolved (config restored)",
                 {{"fault", "config-corrupt"}});
        }
        ++repaired;
    }
    (void)repaired;
    emit(root, "VFS_RESTORED", "engine", "vfs",
         "rootfs restored (" + std::to_string(content.files.size()) + " files, " +
             std::to_string(content.dirs.size()) + " dirs)");
    stage(totalUnits, totalUnits);
    propagate(root);
    return OpResult::success(root, "rootfs restored.");
}

} // namespace override
