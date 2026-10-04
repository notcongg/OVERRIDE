#include "override/system.hpp"

#include <algorithm>
#include <sstream>

namespace override {

// ---- OVERKNRL virtual filesystem operations (host fs never touched) ----

std::string System::vfsAbs(const std::string& path) const {
    return Vfs::normalize(cwd_, path);
}

std::string System::inferOwner(const std::string& abs) const {
    // A file whose basename stem matches a node name is node-owned.
    std::string base = Vfs::baseName(abs);
    auto dot = base.rfind('.');
    std::string stem = (dot == std::string::npos) ? base : base.substr(0, dot);
    if (!stem.empty() && hasNode(stem)) return stem;
    const VFile* f = vfs_.file(abs);
    if (f && !f->owner.empty() && hasNode(f->owner)) return f->owner;
    return "";
}

bool System::rootfsWritable(const std::string& owner, std::string& whyNot) const {
    if (owner.empty() || !hasNode(owner)) return true; // virtual-host files: always writable
    const Node& n = get(owner);
    auto it = n.filesystems.find("rootfs");
    FsState st = (it == n.filesystems.end()) ? FsState::MOUNTED : it->second;
    if (st == FsState::MOUNTED || st == FsState::DEGRADED) return true;
    whyNot = owner + " rootfs is " + toString(st) + " (writes fail)";
    return false;
}

OpResult System::setCwd(const std::string& path) {
    std::string abs = vfsAbs(path);
    if (abs.empty() || !vfs_.isDir(abs))
        return OpResult::failure("no such directory: " + path);
    const VDir* d = vfs_.dir(abs);
    if (d) {
        std::string why;
        if (!vfsAuthorize(d->owner, d->acl, false, why))
            return OpResult::failure("permission denied (" + why + " on " + abs + ")");
    }
    cwd_ = abs;
    return OpResult::success(0, cwd_);
}

std::string System::resolveBinary(const std::string& node, const std::string& binary,
                                  std::string& err) const {
    // Baked-in node images first, then the shared rootfs substrate. A rootfs
    // image counts only when present, executable, and uncorrupted, so
    // deleting or corrupting /bin/<name> genuinely removes that capability.
    // Absolute forms (/bin/sh) resolve directly; bare names also try the
    // node's baked-in set first.
    if (!hasNode(node)) {
        err = "unknown node: " + node;
        return "";
    }
    err.clear();
    auto checkFile = [&](const std::string& p) -> std::string {
        const VFile* f = vfs_.file(p);
        if (!f) return "";
        if (f->corrupted) {
            err = "binary '" + binary + "' corrupted (" + p + " failed integrity)";
            return "";
        }
        if (!f->executable) {
            err = "binary '" + binary + "' not executable (" + p + ")";
            return "";
        }
        err.clear();
        return p;
    };
    if (!binary.empty() && binary[0] == '/') {
        std::string p = Vfs::normalize("/", binary);
        if (p.empty() || vfs_.file(p) == nullptr) {
            err = "no such binary '" + binary + "'";
            return "";
        }
        return checkFile(p);
    }
    if (get(node).binaries.count(binary)) return "<builtin>:" + binary;
    static const char* dirs[] = {"/bin", "/sbin"};
    for (const char* d : dirs) {
        std::string p = std::string(d) + "/" + binary;
        if (vfs_.file(p) != nullptr) {
            std::string hit = checkFile(p);
            if (!hit.empty() || !err.empty()) return hit;
        }
    }
    err = "no such binary '" + binary + "'";
    return "";
}

bool System::deviceReady(const std::string& dev, std::string& whyNot) const {
    std::string p = "/dev/" + dev;
    const VFile* f = vfs_.file(p);
    if (!f) {
        whyNot = p + " missing";
        return false;
    }
    if (f->corrupted) {
        whyNot = p + " corrupted (integrity invalid)";
        return false;
    }
    return true;
}

std::string System::vfsList(const std::string& path) const {
    std::string abs = path.empty() ? cwd_ : vfsAbs(path);
    if (abs.empty()) return "error: invalid path";
    std::vector<VEntry> entries;
    std::string err;
    if (!vfs_.list(abs, entries, err)) return "error: " + err;
    noteSyscall("getdents");
    const VDir* here = vfs_.dir(abs);
    if (here) {
        std::string why;
        if (!vfsAuthorize(here->owner, here->acl, false, why))
            return "error: permission denied (" + why + " on " + abs + ")";
    }
    std::ostringstream o;
    o << abs << " (" << entries.size() << " entries)\n";
    auto aclStr = [](const std::set<std::string>& acl) {
        std::string s;
        for (const auto& t : acl) {
            if (!s.empty()) s += ",";
            s += t;
        }
        return s;
    };
    for (const auto& e : entries) {
        if (e.isDir) {
            o << "  " << e.name;
            if (!e.owner.empty()) o << "  owner=" << e.owner;
            o << "  acl=" << aclStr(e.acl) << "\n";
        } else {
            o << "  " << e.name << (e.executable ? "*" : "") << "  " << e.size << "B";
            if (!e.owner.empty()) o << "  owner=" << e.owner;
            o << "  acl=" << aclStr(e.acl);
            if (e.corrupted) o << "  [CORRUPTED]";
            o << "\n";
        }
    }
    return o.str();
}

std::string System::vfsCat(const std::string& path) const {
    std::string abs = vfsAbs(path);
    if (abs.empty()) return "error: invalid path";
    const VFile* f = vfs_.file(abs);
    if (!f) {
        if (vfs_.isDir(abs)) return "error: is a directory: " + abs;
        return "error: no such file: " + abs;
    }
    {
        std::string why;
        if (!vfsAuthorize(f->owner, f->acl, false, why))
            return "error: permission denied (" + why + " on " + abs + ")";
    }
    noteSyscall("read");
    std::ostringstream o;
    o << "--- " << abs << " (" << f->content.size() << "B";
    if (!f->owner.empty()) o << ", owner=" << f->owner;
    o << ") ---\n";
    o << f->content;
    if (!f->content.empty() && f->content.back() != '\n') o << "\n";
    if (f->corrupted) o << "[CORRUPTED: checksum mismatch, content untrustworthy]\n";
    return o.str();
}

OpResult System::vfsMkdir(const std::string& path) {
    std::string abs = vfsAbs(path);
    if (abs.empty()) return OpResult::failure("invalid path");
    uint64_t root = emit(0, "USER_VFS", "user", "vfs", "mkdir " + abs);
    std::string why;
    if (const VDir* pd = vfs_.dir(Vfs::parentDir(abs))) {
        if (!vfsAuthorize(pd->owner, pd->acl, true, why)) {
            emit(root, "VFS_DENIED", "vfs", abs, "mkdir denied: permission denied (" + why +
                 " on " + Vfs::parentDir(abs) + ")", {}, "WARNING");
            return OpResult::failure("permission denied (" + why + ")", root);
        }
    }
    if (!rootfsWritable(inferOwner(abs), why)) {
        emit(root, "VFS_DENIED", "vfs", abs, "mkdir denied: " + why, {}, "WARNING");
        return OpResult::failure(why, root);
    }
    std::string err;
    std::string owner = inferOwner(abs);
    if (owner.empty() && !opRoot_) owner = opUser_; // creators own what they make
    if (!vfs_.mkdir(abs, err, owner)) {
        emit(root, "VFS_DENIED", "vfs", abs, "mkdir failed: " + err);
        return OpResult::failure(err, root);
    }
    emit(root, "VFS_MKDIR", "vfs", abs, "directory created: " + abs);
    noteSyscall("open");
    return OpResult::success(root, "created directory " + abs);
}

OpResult System::vfsTouch(const std::string& path) {
    std::string abs = vfsAbs(path);
    if (abs.empty()) return OpResult::failure("invalid path");
    uint64_t root = emit(0, "USER_VFS", "user", "vfs", "touch " + abs);
    std::string why;
    if (const VFile* f = vfs_.file(abs)) {
        if (!vfsAuthorize(f->owner, f->acl, true, why)) {
            emit(root, "VFS_DENIED", "vfs", abs,
                 "touch denied: permission denied (" + why + " on " + abs + ")", {}, "WARNING");
            return OpResult::failure("permission denied (" + why + ")", root);
        }
    } else if (const VDir* pd = vfs_.dir(Vfs::parentDir(abs))) {
        if (!vfsAuthorize(pd->owner, pd->acl, true, why)) {
            emit(root, "VFS_DENIED", "vfs", abs,
                 "touch denied: permission denied (" + why + " on " + Vfs::parentDir(abs) +
                     ")",
                 {}, "WARNING");
            return OpResult::failure("permission denied (" + why + ")", root);
        }
    }
    if (!rootfsWritable(inferOwner(abs), why)) {
        emit(root, "VFS_DENIED", "vfs", abs, "touch denied: " + why, {}, "WARNING");
        return OpResult::failure(why, root);
    }
    std::string err;
    std::string owner = inferOwner(abs);
    if (owner.empty() && !opRoot_) owner = opUser_;
    if (!vfs_.touch(abs, owner, clock_.tickCount(), err)) {
        emit(root, "VFS_DENIED", "vfs", abs, "touch failed: " + err);
        return OpResult::failure(err, root);
    }
    emit(root, "VFS_TOUCH", "vfs", abs, "file touched: " + abs);
    noteSyscall("open");
    return OpResult::success(root, "touched " + abs);
}

OpResult System::vfsRemove(const std::string& path) {
    std::string abs = vfsAbs(path);
    if (abs.empty()) return OpResult::failure("invalid path");
    if (abs == cwd_) return OpResult::failure("cannot remove the working directory");
    uint64_t root = emit(0, "USER_VFS", "user", "vfs", "rm " + abs);
    std::string why;
    if (const VDir* pd = vfs_.dir(Vfs::parentDir(abs))) {
        if (!vfsAuthorize(pd->owner, pd->acl, true, why)) {
            emit(root, "VFS_DENIED", "vfs", abs,
                 "rm denied: permission denied (" + why + " on " + Vfs::parentDir(abs) + ")",
                 {}, "WARNING");
            return OpResult::failure("permission denied (" + why + ")", root);
        }
    }
    if (!rootfsWritable(inferOwner(abs), why)) {
        emit(root, "VFS_DENIED", "vfs", abs, "rm denied: " + why, {}, "WARNING");
        return OpResult::failure(why, root);
    }
    std::string err;
    if (!vfs_.remove(abs, err)) {
        emit(root, "VFS_DENIED", "vfs", abs, "rm failed: " + err);
        return OpResult::failure(err, root);
    }
    {
        std::string derr;
        hw_.dropOverflow(abs, derr); // free any R accounting; missing key is fine
    }
    emit(root, "VFS_REMOVE", "vfs", abs, "removed: " + abs);
    noteSyscall("unlink");
    return OpResult::success(root, "removed " + abs);
}

OpResult System::vfsCopy(const std::string& src, const std::string& dst) {
    std::string asrc = vfsAbs(src), adst = vfsAbs(dst);
    if (asrc.empty() || adst.empty()) return OpResult::failure("invalid path");
    uint64_t root = emit(0, "USER_VFS", "user", "vfs", "cp " + asrc + " " + adst);
    std::string why;
    if (const VFile* sf = vfs_.file(asrc)) {
        if (!vfsAuthorize(sf->owner, sf->acl, false, why)) {
            emit(root, "VFS_DENIED", "vfs", adst,
                 "cp denied: permission denied (" + why + " on " + asrc + ")", {}, "WARNING");
            return OpResult::failure("permission denied (" + why + ")", root);
        }
    }
    if (const VDir* pd = vfs_.dir(Vfs::parentDir(adst))) {
        if (!vfsAuthorize(pd->owner, pd->acl, true, why)) {
            emit(root, "VFS_DENIED", "vfs", adst,
                 "cp denied: permission denied (" + why + " on " + Vfs::parentDir(adst) + ")",
                 {}, "WARNING");
            return OpResult::failure("permission denied (" + why + ")", root);
        }
    }
    if (!rootfsWritable(inferOwner(adst), why)) {
        emit(root, "VFS_DENIED", "vfs", adst, "cp denied: " + why, {}, "WARNING");
        return OpResult::failure(why, root);
    }
    std::string err;
    const VFile* srcFile = vfs_.file(asrc);
    std::string action;
    std::string qerr =
        (srcFile != nullptr) ? hwAccountWrite(adst, srcFile->content, action) : std::string();
    if (!qerr.empty()) {
        emit(root, "VFS_DENIED", "vfs", adst, "cp denied: " + qerr, {}, "WARNING");
        return OpResult::failure(qerr, root);
    }
    if (!vfs_.copy(asrc, adst, clock_.tickCount(), err)) {
        emit(root, "VFS_DENIED", "vfs", asrc, "cp failed: " + err);
        return OpResult::failure(err, root);
    }
    std::string done = "copied " + asrc + " -> " + adst;
    if (action == "r") done += " (kept in R backing)";
    emit(root, "VFS_COPY", "vfs", adst, done);
    noteSyscall("sendfile");
    return OpResult::success(root, done);
}

OpResult System::vfsMove(const std::string& src, const std::string& dst) {
    std::string asrc = vfsAbs(src), adst = vfsAbs(dst);
    if (asrc.empty() || adst.empty()) return OpResult::failure("invalid path");
    if (asrc == cwd_ || adst == cwd_) return OpResult::failure("cannot move the working directory");
    uint64_t root = emit(0, "USER_VFS", "user", "vfs", "mv " + asrc + " " + adst);
    std::string why;
    if (const VFile* sf = vfs_.file(asrc)) {
        if (!vfsAuthorize(sf->owner, sf->acl, false, why)) {
            emit(root, "VFS_DENIED", "vfs", asrc,
                 "mv denied: permission denied (" + why + " on " + asrc + ")", {}, "WARNING");
            return OpResult::failure("permission denied (" + why + ")", root);
        }
    }
    if (const VDir* ps = vfs_.dir(Vfs::parentDir(asrc))) {
        if (!vfsAuthorize(ps->owner, ps->acl, true, why)) {
            emit(root, "VFS_DENIED", "vfs", asrc,
                 "mv denied: permission denied (" + why + " on " + Vfs::parentDir(asrc) + ")",
                 {}, "WARNING");
            return OpResult::failure("permission denied (" + why + ")", root);
        }
    }
    if (const VDir* pd = vfs_.dir(Vfs::parentDir(adst))) {
        if (!vfsAuthorize(pd->owner, pd->acl, true, why)) {
            emit(root, "VFS_DENIED", "vfs", asrc,
                 "mv denied: permission denied (" + why + " on " + Vfs::parentDir(adst) + ")",
                 {}, "WARNING");
            return OpResult::failure("permission denied (" + why + ")", root);
        }
    }
    if (!rootfsWritable(inferOwner(adst), why) || !rootfsWritable(inferOwner(asrc), why)) {
        emit(root, "VFS_DENIED", "vfs", asrc, "mv denied: " + why, {}, "WARNING");
        return OpResult::failure(why, root);
    }
    std::string err;
    const VFile* mvFile = vfs_.file(asrc);
    std::string action;
    std::string qerr =
        (mvFile != nullptr) ? hwAccountWrite(adst, mvFile->content, action) : std::string();
    if (!qerr.empty()) {
        emit(root, "VFS_DENIED", "vfs", asrc, "mv denied: " + qerr, {}, "WARNING");
        return OpResult::failure(qerr, root);
    }
    if (!vfs_.move(asrc, adst, err)) {
        emit(root, "VFS_DENIED", "vfs", asrc, "mv failed: " + err);
        return OpResult::failure(err, root);
    }
    // A moved node-owned config keeps its owner (stored in the file).
    {
        std::string derr;
        hw_.dropOverflow(asrc, derr); // accounting follows the destination key
    }
    std::string moved = "moved " + asrc + " -> " + adst;
    if (action == "r") moved += " (kept in R backing)";
    emit(root, "VFS_MOVE", "vfs", adst, moved);
    noteSyscall("rename");
    if (asrc == cwd_ || Vfs::parentDir(cwd_) == asrc) cwd_ = "/";
    return OpResult::success(root, moved);
}

OpResult System::vfsWrite(const std::string& path, const std::string& content) {
    std::string abs = vfsAbs(path);
    if (abs.empty()) return OpResult::failure("invalid path");
    uint64_t root = emit(0, "USER_VFS", "user", "vfs", "write " + abs);
    std::string why;
    std::string owner = inferOwner(abs);
    {
        const VFile* existing = vfs_.file(abs);
        if (existing) {
            if (!vfsAuthorize(existing->owner, existing->acl, true, why)) {
                emit(root, "VFS_DENIED", "vfs", abs,
                     "write denied: permission denied (" + why + " on " + abs + ")", {},
                     "WARNING");
                return OpResult::failure("permission denied (" + why + ")", root);
            }
            if (!existing->owner.empty()) owner = existing->owner;
        } else if (const VDir* pd = vfs_.dir(Vfs::parentDir(abs))) {
            if (!vfsAuthorize(pd->owner, pd->acl, true, why)) {
                emit(root, "VFS_DENIED", "vfs", abs,
                     "write denied: permission denied (" + why + " on " + Vfs::parentDir(abs) +
                         ")",
                     {}, "WARNING");
                return OpResult::failure("permission denied (" + why + ")", root);
            }
            if (owner.empty() && !opRoot_) owner = opUser_; // creators own what they make
        }
    }
    if (!rootfsWritable(owner, why)) {
        emit(root, "VFS_DENIED", "vfs", abs, "write denied: " + why, {}, "WARNING");
        return OpResult::failure(why, root);
    }
    std::string err;
    std::string action;
    std::string qerr = hwAccountWrite(abs, content, action);
    if (!qerr.empty()) {
        emit(root, "VFS_DENIED", "vfs", abs, "write denied: " + qerr, {}, "WARNING");
        return OpResult::failure(qerr, root);
    }
    if (!vfs_.write(abs, content, owner, clock_.tickCount(), err)) {
        emit(root, "VFS_DENIED", "vfs", abs, "write failed: " + err);
        return OpResult::failure(err, root);
    }
    std::string done =
        "wrote " + std::to_string(content.size()) + "B to " + abs;
    if (action == "r") done += " (kept in R backing)";
    emit(root, "VFS_WRITE", "vfs", abs, done);
    noteSyscall("write");
    return OpResult::success(root, done);
}

OpResult System::vfsCorrupt(const std::string& path) {
    std::string abs = vfsAbs(path);
    if (abs.empty()) return OpResult::failure("invalid path");
    uint64_t root = emit(0, "USER_VFS", "user", "vfs", "corrupt " + abs);
    std::string err;
    if (const VFile* f = vfs_.file(abs)) {
        std::string why;
        if (!vfsAuthorize(f->owner, f->acl, true, why)) {
            emit(root, "VFS_DENIED", "vfs", abs,
                 "corrupt denied: permission denied (" + why + " on " + abs + ")", {},
                 "WARNING");
            return OpResult::failure("permission denied (" + why + ")", root);
        }
    }
    if (!vfs_.corrupt(abs, err)) {
        emit(root, "VFS_DENIED", "vfs", abs, "corrupt failed: " + err);
        return OpResult::failure(err, root);
    }
    emit(root, "VFS_CORRUPT", "vfs", abs, "file corrupted: " + abs, {}, "WARNING");
    noteSyscall("ioctl");
    std::string info = "corrupted " + abs;
    // Corrupting a node's /etc/ config corrupts the node's configuration.
    std::string owner = inferOwner(abs);
    if (!owner.empty() && abs.rfind("/etc/", 0) == 0 && hasNode(owner)) {
        Node& n = get(owner);
        n.configCorrupt = true;
        raiseFault(owner, "config-corrupt", Severity::WARNING, root,
                   abs + " corrupted on disk");
        info += " (node " + owner + " configuration now CORRUPTED)";
    }
    // Corrupting the shared kernel configuration faults every running
    // kernel: boot will refuse until the file is repaired (no magical
    // recovery), and the dependency is visible in why/trace/faults.
    if (abs == "/kernel/kernel.conf") {
        for (const auto& name : nodeNames()) {
            Node& n = get(name);
            if (n.kernel == KernelState::RUNNING &&
                n.faults.count("kernel-config") == 0) {
                raiseFault(name, "kernel-config", Severity::WARNING, root,
                           "/kernel/kernel.conf corrupted on disk");
            }
        }
        info += " (kernel configuration now CORRUPTED)";
    }
    return OpResult::success(root, info);
}

bool System::vfsAuthorize(const std::string& owner, const std::set<std::string>& acl,
                          bool needWrite, std::string& whyNot) const {
    if (opRoot_) return true; // simulated root bypass (hard safety gates still apply)
    std::string actor = (!owner.empty() && owner == opUser_) ? "o" : "u";
    std::string need = actor + (needWrite ? "w" : "r");
    if (acl.count(need)) return true;
    whyNot = "need " + need + " (as " + opUser_ + ")";
    return false;
}

OpResult System::vfsChmod(const std::string& path, bool add, const std::string& token) {
    std::string abs = vfsAbs(path);
    if (abs.empty()) return OpResult::failure("invalid path");
    if (!validPermToken(token))
        return OpResult::failure("invalid permission '" + token +
                                 "' (actor rt|o|u + capability r|w: ur uw or ow rtr rtw)");
    uint64_t root = emit(0, "USER_VFS", "user", "vfs",
                         std::string(add ? "chmod +" : "chmod -") + token + " " + abs);
    // Only the entry owner (or simulated root) may change permissions.
    std::string owner;
    if (const VFile* f = vfs_.file(abs)) owner = f->owner;
    else if (const VDir* d = vfs_.dir(abs)) owner = d->owner;
    else {
        emit(root, "VFS_DENIED", "vfs", abs, "chmod failed: no such file or directory");
        return OpResult::failure("no such file or directory: " + abs, root);
    }
    if (!opRoot_ && (owner.empty() || owner != opUser_)) {
        emit(root, "VFS_DENIED", "vfs", abs,
             "chmod denied: only the owner or root may change permissions", {}, "WARNING");
        return OpResult::failure("only the owner or root may change permissions", root);
    }
    std::string err;
    if (!vfs_.chmod(abs, add, token, err)) {
        emit(root, "VFS_DENIED", "vfs", abs, "chmod failed: " + err);
        return OpResult::failure(err, root);
    }
    emit(root, "VFS_CHMOD", "vfs", abs,
         std::string("permissions ") + (add ? "granted " : "removed ") + token + " on " + abs);
    noteSyscall("chmod");
    return OpResult::success(root, std::string("chmod ") + (add ? "+" : "-") + token + " " + abs);
}

} // namespace override
