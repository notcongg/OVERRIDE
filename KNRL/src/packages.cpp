// Recovery packages: engine-canonical baselines persisted as .ord files.
// Kernel bytes come from the same literal the VFS seed uses; rootfs content
// is a fresh seed for the requesting node list. Nothing here reads the host
// kernel, host rootfs, or arbitrary host paths: the recovery directory is
// the only filesystem surface, and loads apply through System methods.
#include "override/packages.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <sstream>

#include "detail.hpp"
#include "override/account.hpp"
#include "override/vfs.hpp"

namespace override {

namespace fs = std::filesystem;

std::string PackageManager::kernelPath() const {
    return (fs::path(dir_) / "kernel.ord").string();
}

std::string PackageManager::rootfsPath() const {
    return (fs::path(dir_) / "rootfs.ord").string();
}

bool PackageManager::kernelAvailable() const {
    std::error_code ec;
    return fs::is_regular_file(kernelPath(), ec);
}

bool PackageManager::rootfsAvailable() const {
    std::error_code ec;
    return fs::is_regular_file(rootfsPath(), ec);
}

bool PackageManager::writeFile(const std::string& path, const std::string& content,
                               std::string& err) const {
    // Atomic within the recovery dir: tmp + rename, stale tmp cleaned.
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) {
        err = "cannot create recovery directory '" + dir_ + "': " + ec.message();
        return false;
    }
    std::string tmp = path + ".tmp";
    {
        FILE* f = nullptr;
#if defined(_WIN32)
        fopen_s(&f, tmp.c_str(), "wb");
#else
        f = std::fopen(tmp.c_str(), "wb");
#endif
        if (!f) {
            err = "cannot write temporary file '" + tmp + "'";
            return false;
        }
        size_t done = content.empty() ? 0 : std::fwrite(content.data(), 1, content.size(), f);
        bool ok = (done == content.size()) && (std::fflush(f) == 0);
        std::fclose(f);
        if (!ok) {
            fs::remove(tmp, ec);
            err = "failed writing temporary file '" + tmp + "'";
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        err = "cannot replace '" + path + "': " + ec.message();
        return false;
    }
    return true;
}

bool PackageManager::readFile(const std::string& path, std::string& content,
                              std::string& err) const {
    FILE* f = nullptr;
#if defined(_WIN32)
    fopen_s(&f, path.c_str(), "rb");
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) {
        err = "no such file: " + path;
        return false;
    }
    std::string out;
    char buf[8192];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    content = out;
    return true;
}

bool PackageManager::ensureKernel(bool& created, std::string& msg, std::string& err,
                                  uint64_t createdAt) {
    created = false;
    if (kernelAvailable()) {
        std::string bytes;
        if (readKernel(bytes, err)) {
            msg = "kernel package already available.";
            return true;
        }
        // Corrupt on disk: regenerating is necessary, not gratuitous.
        err.clear();
    }
    ordc::OrdDoc doc;
    doc.type = "recovery";
    doc.id = "kernel";
    doc.set("package", "kind", "kernel");
    doc.set("package", "version", "1");
    doc.set("package", "created_at", std::to_string(createdAt));
    std::string bytes = canonicalKernelBytes();
    doc.set("kernel", "content", bytes);
    doc.set("kernel", "sha256", sha256Hex(bytes));
    if (!writeFile(kernelPath(), ordc::serializeOrd(doc), err)) return false;
    created = true;
    msg = "kernel package retrieved.";
    return true;
}

bool PackageManager::ensureRootfs(const std::vector<std::string>& nodes, bool& created,
                                  std::string& msg, std::string& err, uint64_t createdAt,
                                  ProgressCb progress) {
    created = false;
    if (rootfsAvailable()) {
        RootfsContent existing;
        if (readRootfs(existing, err)) {
            msg = "rootfs package already available.";
            return true;
        }
        err.clear();
    }
    // Canonical baseline: a fresh seed for the requesting node list.
    Vfs v;
    v.seedDefaults(nodes);
    std::vector<std::string> dirs, files;
    {
        // Same deterministic walk the world serializer uses (sorted order).
        std::vector<std::string> stack{"/"};
        std::vector<VEntry> entries;
        std::string lerr;
        while (!stack.empty()) {
            std::string d = stack.back();
            stack.pop_back();
            entries.clear();
            if (!v.list(d, entries, lerr)) continue;
            for (const auto& e : entries) {
                std::string full = (d == "/") ? ("/" + e.name) : (d + "/" + e.name);
                if (e.isDir) {
                    std::string dd = full.substr(0, full.size() - 1);
                    dirs.push_back(dd);
                    stack.push_back(dd);
                } else {
                    files.push_back(full);
                }
            }
        }
        std::sort(dirs.begin(), dirs.end());
        std::sort(files.begin(), files.end());
    }
    ordc::OrdDoc doc;
    doc.type = "recovery";
    doc.id = "rootfs";
    doc.set("package", "kind", "rootfs");
    doc.set("package", "version", "1");
    doc.set("package", "created_at", std::to_string(createdAt));
    {
        std::string joined;
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (i) joined += ",";
            joined += nodes[i];
        }
        doc.set("package", "nodes", joined);
    }
    doc.set("vfs", "dir_count", std::to_string(dirs.size()));
    for (size_t i = 0; i < dirs.size(); ++i) {
        std::string pre = "dir." + std::to_string(i);
        doc.set("vfs", pre, dirs[i]);
        const VDir* dd = v.dir(dirs[i]);
        doc.set("vfs", pre + ".owner", dd ? dd->owner : "");
        int ai = 0;
        if (dd) {
            for (const auto& t : dd->acl)
                doc.set("vfs", pre + ".acl." + std::to_string(ai++), t);
        }
        doc.set("vfs", pre + ".acl_count", std::to_string(ai));
    }
    doc.set("vfs", "file_count", std::to_string(files.size()));
    int done = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        std::string sec = "file." + std::to_string(i);
        const VFile* f = v.file(files[i]);
        if (!f) {
            err = "internal: seed file vanished: " + files[i];
            return false;
        }
        doc.set(sec, "path", files[i]);
        doc.set(sec, "owner", f->owner);
        doc.set(sec, "exec", f->executable ? "1" : "0");
        doc.set(sec, "corrupt", "0");
        doc.set(sec, "mtime", std::to_string(f->mtime));
        doc.set(sec, "content", f->content);
        int ai = 0;
        for (const auto& t : f->acl) doc.set(sec, "acl." + std::to_string(ai++), t);
        doc.set(sec, "acl_count", std::to_string(ai));
        if (progress) progress(++done, (int)files.size());
    }
    {
        std::ostringstream o;
        v.appendDigest(o);
        doc.set("rootfs", "vfs_digest", detail::hex16(detail::fnv1a(o.str())));
    }
    if (!writeFile(rootfsPath(), ordc::serializeOrd(doc), err)) return false;
    created = true;
    msg = "rootfs package retrieved.";
    return true;
}

bool PackageManager::readKernel(std::string& bytes, std::string& err) const {
    std::string text;
    if (!readFile(kernelPath(), text, err)) {
        err = "kernel package missing: " + kernelPath();
        return false;
    }
    ordc::OrdDoc doc;
    ordc::OrdError oerr;
    if (!ordc::parseOrd(text, "kernel.ord", doc, oerr)) {
        err = "kernel recovery package is invalid (parse: " + oerr.str() + ")";
        return false;
    }
    if (doc.type != "recovery" || doc.id != "kernel") {
        err = "kernel recovery package is invalid (not a kernel package)";
        return false;
    }
    std::string kind, ver, content, digest;
    if (!ordc::getString(doc, "package", "kind", kind, oerr) || kind != "kernel" ||
        !ordc::getString(doc, "package", "version", ver, oerr) || ver != "1" ||
        !ordc::getString(doc, "kernel", "content", content, oerr) || content.empty() ||
        !ordc::getString(doc, "kernel", "sha256", digest, oerr)) {
        err = "kernel recovery package is invalid (schema)";
        return false;
    }
    if (sha256Hex(content) != digest) {
        err = "kernel recovery package is invalid (reason: integrity check failed)";
        return false;
    }
    bytes = content;
    return true;
}

bool PackageManager::readRootfs(RootfsContent& out, std::string& err) const {
    RootfsContent content;
    std::string text;
    if (!readFile(rootfsPath(), text, err)) {
        err = "rootfs package missing: " + rootfsPath();
        return false;
    }
    ordc::OrdDoc doc;
    ordc::OrdError oerr;
    if (!ordc::parseOrd(text, "rootfs.ord", doc, oerr)) {
        err = "rootfs recovery package is invalid (parse: " + oerr.str() + ")";
        return false;
    }
    if (doc.type != "recovery" || doc.id != "rootfs") {
        err = "rootfs recovery package is invalid (not a rootfs package)";
        return false;
    }
    std::string kind, ver, nodes, wantDigest;
    if (!ordc::getString(doc, "package", "kind", kind, oerr) || kind != "rootfs" ||
        !ordc::getString(doc, "package", "version", ver, oerr) || ver != "1" ||
        !ordc::getString(doc, "package", "nodes", nodes, oerr)) {
        err = "rootfs recovery package is invalid (schema)";
        return false;
    }
    {
        std::string cur;
        for (char c : nodes + ",") {
            if (c == ',') {
                if (!cur.empty()) content.nodes.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
    }
    if (!ordc::getString(doc, "rootfs", "vfs_digest", wantDigest, oerr) ||
        wantDigest.empty()) {
        err = "rootfs recovery package is invalid (schema)";
        return false;
    }
    int dirCount = 0, fileCount = 0;
    if (!ordc::getInt(doc, "vfs", "dir_count", dirCount, oerr) || dirCount < 0 ||
        !ordc::getInt(doc, "vfs", "file_count", fileCount, oerr) || fileCount <= 0) {
        err = "rootfs recovery package is invalid (schema)";
        return false;
    }
    // Rebuild a scratch tree and verify its digest: integrity over content.
    Vfs scratch;
    scratch.clear();
    for (int i = 0; i < dirCount; ++i) {
        std::string pre = "dir." + std::to_string(i), path, owner;
        int acount = 0;
        if (!ordc::getString(doc, "vfs", pre, path, oerr) ||
            !ordc::getString(doc, "vfs", pre + ".owner", owner, oerr) ||
            !ordc::getInt(doc, "vfs", pre + ".acl_count", acount, oerr) || acount < 0) {
            err = "rootfs recovery package is invalid (schema)";
            return false;
        }
        std::set<std::string> acl;
        for (int j = 0; j < acount; ++j) {
            std::string tok;
            if (!ordc::getString(doc, "vfs", pre + ".acl." + std::to_string(j), tok, oerr) ||
                !validPermToken(tok)) {
                err = "rootfs recovery package is invalid (schema)";
                return false;
            }
            acl.insert(tok);
        }
        if (path != "/") {
            std::string derr;
            if (!scratch.mkdir(path, derr, owner, acl)) {
                err = "rootfs recovery package is invalid (tree: " + derr + ")";
                return false;
            }
        }
        content.dirs.push_back({path, owner, acl});
    }
    for (int i = 0; i < fileCount; ++i) {
                std::string sec = "file." + std::to_string(i);
                std::string path, owner, fcontent;
        int exec = 0, corrupt = 0, acount = 0;
        uint64_t mtime = 0;
        if (!ordc::requireSection(doc, sec, oerr) ||
            !ordc::getString(doc, sec, "path", path, oerr) ||
            !ordc::getString(doc, sec, "owner", owner, oerr) ||
            !ordc::getInt(doc, sec, "exec", exec, oerr) || (exec != 0 && exec != 1) ||
            !ordc::getInt(doc, sec, "corrupt", corrupt, oerr) || corrupt != 0 ||
            !ordc::getUint64(doc, sec, "mtime", mtime, oerr) ||
            !ordc::getString(doc, sec, "content", fcontent, oerr) ||
            !ordc::getInt(doc, sec, "acl_count", acount, oerr) || acount < 0) {
            err = "rootfs recovery package is invalid (schema)";
            return false;
        }
        RootfsFile rf{path, owner, fcontent, exec == 1, {}};
        for (int j = 0; j < acount; ++j) {
            std::string tok;
            if (!ordc::getString(doc, sec, "acl." + std::to_string(j), tok, oerr) ||
                !validPermToken(tok)) {
                err = "rootfs recovery package is invalid (schema)";
                return false;
            }
            rf.acl.insert(tok);
        }
        std::string derr;
        if (!scratch.write(path, fcontent, owner, mtime, derr, rf.executable)) {
            err = "rootfs recovery package is invalid (tree: " + derr + ")";
            return false;
        }
                for (const auto& t : rf.acl) scratch.chmod(path, true, t, derr);
                content.files.push_back(std::move(rf));
            }
    {
        std::ostringstream o;
        scratch.appendDigest(o);
        if (detail::hex16(detail::fnv1a(o.str())) != wantDigest) {
            err = "rootfs recovery package is invalid (reason: integrity check failed)";
            return false;
        }
    }
    out = std::move(content);
    return true;
}

} // namespace override
