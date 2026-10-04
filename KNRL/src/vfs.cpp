#include "override/vfs.hpp"

#include <algorithm>

namespace override {

void Vfs::clear() { root_ = VDir{}; }

bool validPermToken(const std::string& t) {
    // actor (rt|o|u) + capability (r|w): ur uw or ow rtr rtw.
    if (t.size() < 2 || t.size() > 3) return false;
    std::string cap = t.substr(t.size() - 1);
    if (cap != "r" && cap != "w") return false;
    std::string actor = t.substr(0, t.size() - 1);
    return actor == "rt" || actor == "o" || actor == "u";
}

std::set<std::string> defaultAcl() { return {"or", "ow", "ur", "uw"}; }

std::string canonicalKernelBytes() {
    return "OVERKNRL kernel image v0.1.0\nformat: simulated-elf\n";
}

void Vfs::seedDefaults(const std::vector<std::string>& nodeNames) {
    // OVERKNRL simulated OS substrate. Pure in-memory seed; the host
    // filesystem is never read. Every file below is mirrored byte-for-byte
    // under KNRL/OVERKNRL/rootfs (verified by t_vfs_physical).
    clear();
    std::string err;
    for (const char* d :
         {"/boot", "/kernel", "/lib", "/lib/modules", "/lib/firmware", "/bin", "/sbin",
          "/dev", "/proc", "/sys", "/run", "/run/services", "/run/pid", "/etc", "/home",
          "/root", "/tmp", "/usr", "/usr/bin", "/usr/sbin", "/usr/lib", "/usr/share",
          "/var", "/var/log", "/var/cache", "/var/lib", "/var/run"}) {
        mkdir(d, err);
    }
    auto put = [&](const std::string& p, const std::string& content, const std::string& owner,
                   bool exec) { write(p, content, owner, 0, err, exec); };
    // Boot image + loader config: deleting/corrupting /boot/kernel makes the
    // next reboot fail (see boot pipeline); the image is shared substrate.
    put("/boot/kernel", canonicalKernelBytes(), "", false);
    put("/boot/initramfs", "simulated initramfs\nmodules: sched,net,disk\n", "", false);
    put("/boot/boot.conf", "timeout=5\ndefault=overknrl\n", "", false);
    // Kernel tree: config, syscall symbols, loadable-module inventory.
    put("/kernel/kernel.conf", "panic_on_fault=0\nscheduler=priority\n", "", false);
    put("/kernel/symbols", "sys_open\nsys_read\nsys_write\nsched_run\nipc_send\n", "", false);
    put("/kernel/modules.list", "sched\nnet\ndisk\n", "", false);
    // Loadable modules: presence here gates DRIVER_INIT at boot and
    // module-load at runtime for the matching node module entry.
    put("/lib/modules/sched.ko", "simulated module: sched\nversion=0.1.0\n", "", false);
    put("/lib/modules/net.ko", "simulated module: net\nversion=0.1.0\n", "", false);
    put("/lib/modules/disk.ko", "simulated module: disk\nversion=0.1.0\n", "", false);
    put("/lib/firmware/fw-base.bin", "simulated firmware blob\n", "", false);
    // Simulated executables: recognised by the process/service subsystem.
    // `exec`/service start resolve a binary via the node's baked-in set or
    // one of these rootfs images (must exist, be executable, uncorrupted).
    for (const char* b : {"init", "sh", "echo", "sleep", "netd", "logger", "app"}) {
        put(std::string("/bin/") + b,
            "simulated executable: " + std::string(b) + "\nentry: main\n", "", true);
    }
    for (const char* b :
         {"mount", "umount", "reboot", "shutdown", "modprobe", "lsmod", "sysctl", "init"}) {
        put(std::string("/sbin/") + b,
            "simulated executable: " + std::string(b) + "\nentry: main\n", "", true);
    }
    // Devices: metadata only. Missing/corrupt net0 gates iface/packet/ping;
    // missing/corrupt disk0 gates stored writes; the rest are inspectable.
    put("/dev/null", "simulated device: null\ntype: sink\n", "", false);
    put("/dev/zero", "simulated device: zero\ntype: source\n", "", false);
    put("/dev/console", "simulated device: console\ntype: console\n", "", false);
    put("/dev/tty0", "simulated device: tty0\ntype: tty\n", "", false);
    put("/dev/random", "simulated device: random\ntype: entropy\n", "", false);
    put("/dev/net0", "simulated device: net0\ntype: net\n", "", false);
    put("/dev/disk0", "simulated device: disk0\ntype: block\n", "", false);
    put("/dev/cpu0", "simulated device: cpu0\ntype: cpu\n", "", false);
    // Boot-time kernel snapshots (static seed; live state via ps/kernel/dmesg).
    put("/proc/cpuinfo", "cpu0: Virtual CPU\ncores: 4\n", "", false);
    put("/proc/meminfo", "MemTotal: 4096 MB\n", "", false);
    put("/proc/uptime", "uptime: 0 ticks\n", "", false);
    put("/proc/processes", "(live state; use `ps`)\n", "", false);
    put("/proc/interrupts", "timer: 0\nnet: 0\n", "", false);
    put("/proc/modules", "sched LOADED\nnet LOADED\ndisk LOADED\n", "", false);
    put("/proc/mounts", "rootfs / MOUNTED\ndata /data MOUNTED\n", "", false);
    put("/sys/kernel", "subsystem: kernel\nstate: nominal\n", "", false);
    put("/sys/devices", "subsystem: devices\nstate: nominal\n", "", false);
    put("/sys/thermal", "subsystem: thermal\nstate: nominal\n", "", false);
    put("/sys/memory", "subsystem: memory\nstate: nominal\n", "", false);
    put("/sys/network", "subsystem: network\nstate: nominal\n", "", false);
    // Volatile runtime (wiped conceptually on reboot; snapshotted like all VFS).
    put("/run/lock", "(no locks)\n", "", false);
    put("/run/state", "state: running\n", "", false);
    // Configuration.
    put("/etc/override.conf", "simulation=sandboxed\nseed=auto\n", "", false);
    put("/etc/hostname", "override-local\n", "", false);
    put("/etc/hosts", "127.0.0.1 localhost\n", "", false);
    put("/etc/resolv.conf", "nameserver 10.0.0.1\n", "", false);
    put("/etc/fstab", "rootfs / MOUNTED\n", "", false);
    put("/etc/services.conf", "policy.default=ALWAYS\n", "", false);
    put("/etc/kernel.conf", "panic_on_fault=0\n", "", false);
    put("/etc/modules.conf", "autoload=sched,net,disk\n", "", false);
    put("/var/log/boot.log", "override-local boot ok\n", "", false);
    put("/var/log/messages", "(no messages)\n", "", false);
    put("/var/log/kernel.log", "(empty)\n", "", false);
    for (const auto& n : nodeNames) {
        put("/etc/" + n + ".conf", "node=" + n + "\nmode=auto\n", n, false);
    }
}

std::string Vfs::normalize(const std::string& cwd, const std::string& in) {
    if (in.empty()) return "";
    for (char c : in) {
        if (c == '\0') return "";
    }
    std::string joined = (!in.empty() && in[0] == '/') ? in : (cwd + "/" + in);
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i <= joined.size(); ++i) {
        char c = (i < joined.size()) ? joined[i] : '/';
        if (c == '/') {
            if (cur.empty() || cur == ".") {
                // skip
            } else if (cur == "..") {
                if (!parts.empty()) parts.pop_back(); // past root clamps
            } else {
                if (cur.size() > 64) return "";
                parts.push_back(cur);
            }
            cur.clear();
        } else {
            cur += c;
            if (cur.size() > 64) return "";
        }
    }
    if (parts.empty()) return "/";
    std::string out;
    for (const auto& p : parts) out += "/" + p;
    return out;
}

std::string Vfs::parentDir(const std::string& abs) {
    if (abs.empty() || abs == "/") return "/";
    size_t pos = abs.rfind('/');
    if (pos == 0) return "/";
    return abs.substr(0, pos);
}

std::string Vfs::baseName(const std::string& abs) {
    if (abs.empty() || abs == "/") return "";
    size_t pos = abs.rfind('/');
    return abs.substr(pos + 1);
}

bool Vfs::validAbs(const std::string& abs) {
    return !abs.empty() && abs[0] == '/';
}

const VDir* Vfs::navDir(const std::string& abs) const {
    if (!validAbs(abs)) return nullptr;
    if (abs == "/") return &root_;
    const VDir* d = &root_;
    std::string cur;
    for (size_t i = 1; i <= abs.size(); ++i) {
        char c = (i < abs.size()) ? abs[i] : '/';
        if (c == '/') {
            if (!cur.empty()) {
                auto it = d->dirs.find(cur);
                if (it == d->dirs.end()) return nullptr;
                // A file shadows traversal: cannot descend through it.
                if (d->files.count(cur)) return nullptr;
                d = &it->second;
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    return d;
}

VDir* Vfs::navDirMut(const std::string& abs) {
    if (!validAbs(abs)) return nullptr;
    if (abs == "/") return &root_;
    VDir* d = &root_;
    std::string cur;
    for (size_t i = 1; i <= abs.size(); ++i) {
        char c = (i < abs.size()) ? abs[i] : '/';
        if (c == '/') {
            if (!cur.empty()) {
                auto it = d->dirs.find(cur);
                if (it == d->dirs.end()) return nullptr;
                if (d->files.count(cur)) return nullptr;
                d = &it->second;
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    return d;
}

bool Vfs::exists(const std::string& abs) const {
    if (!validAbs(abs)) return false;
    if (abs == "/") return true;
    const VDir* p = navDir(parentDir(abs));
    if (!p) return false;
    std::string b = baseName(abs);
    return p->dirs.count(b) || p->files.count(b);
}

bool Vfs::isDir(const std::string& abs) const {
    if (!validAbs(abs)) return false;
    if (abs == "/") return true;
    const VDir* p = navDir(parentDir(abs));
    if (!p) return false;
    return p->dirs.count(baseName(abs)) > 0;
}

const VFile* Vfs::file(const std::string& abs) const {
    if (!validAbs(abs) || abs == "/") return nullptr;
    const VDir* p = navDir(parentDir(abs));
    if (!p) return nullptr;
    auto it = p->files.find(baseName(abs));
    return it == p->files.end() ? nullptr : &it->second;
}

const VDir* Vfs::dir(const std::string& abs) const { return navDir(abs); }

bool Vfs::mkdir(const std::string& abs, std::string& err, const std::string& owner,
                const std::set<std::string>& acl) {
    if (!validAbs(abs) || abs == "/") {
        err = "invalid directory path";
        return false;
    }
    VDir* p = navDirMut(parentDir(abs));
    if (!p) {
        err = "no such directory: " + parentDir(abs);
        return false;
    }
    std::string b = baseName(abs);
    if (p->dirs.count(b)) {
        err = "directory already exists: " + abs;
        return false;
    }
    if (p->files.count(b)) {
        err = "file already exists: " + abs;
        return false;
    }
    VDir nd;
    nd.owner = owner;
    nd.acl = acl.empty() ? defaultAcl() : acl;
    p->dirs[b] = std::move(nd);
    return true;
}

bool Vfs::touch(const std::string& abs, const std::string& owner, uint64_t tick,
                std::string& err) {
    if (!validAbs(abs) || abs == "/") {
        err = "invalid file path";
        return false;
    }
    VDir* p = navDirMut(parentDir(abs));
    if (!p) {
        err = "no such directory: " + parentDir(abs);
        return false;
    }
    std::string b = baseName(abs);
    if (p->dirs.count(b)) {
        err = "is a directory: " + abs;
        return false;
    }
    auto it = p->files.find(b);
    if (it != p->files.end()) {
        it->second.mtime = tick; // touch existing: bump time only
        return true;
    }
    VFile f;
    f.owner = owner;
    f.mtime = tick;
    p->files[b] = std::move(f);
    return true;
}

bool Vfs::write(const std::string& abs, const std::string& content, const std::string& owner,
                uint64_t tick, std::string& err, bool executable) {
    if (!validAbs(abs) || abs == "/") {
        err = "invalid file path";
        return false;
    }
    VDir* p = navDirMut(parentDir(abs));
    if (!p) {
        err = "no such directory: " + parentDir(abs);
        return false;
    }
    std::string b = baseName(abs);
    if (p->dirs.count(b)) {
        err = "is a directory: " + abs;
        return false;
    }
    auto it = p->files.find(b);
    if (it == p->files.end()) {
        VFile f; // create: mode comes from the caller (seed sets exec for /bin, /sbin)
        f.content = content;
        f.owner = owner;
        f.mtime = tick;
        f.executable = executable;
        p->files[b] = std::move(f);
        return true;
    }
    // Replace: content changes, mode bits are preserved; fresh bytes restore
    // integrity (a rewrite heals corruption, a delete + rewrite also does).
    it->second.content = content;
    it->second.corrupted = false;
    if (it->second.owner.empty()) it->second.owner = owner;
    it->second.mtime = tick;
    return true;
}

bool Vfs::remove(const std::string& abs, std::string& err) {
    if (!validAbs(abs) || abs == "/") {
        err = "invalid path (refusing to remove root)";
        return false;
    }
    VDir* p = navDirMut(parentDir(abs));
    if (!p) {
        err = "no such path: " + abs;
        return false;
    }
    std::string b = baseName(abs);
    auto fit = p->files.find(b);
    if (fit != p->files.end()) {
        p->files.erase(fit);
        return true;
    }
    auto dit = p->dirs.find(b);
    if (dit == p->dirs.end()) {
        err = "no such file or directory: " + abs;
        return false;
    }
    if (!dit->second.dirs.empty() || !dit->second.files.empty()) {
        err = "directory not empty: " + abs;
        return false;
    }
    p->dirs.erase(dit);
    return true;
}

bool Vfs::copy(const std::string& src, const std::string& dst, uint64_t tick, std::string& err) {
    const VFile* f = file(src);
    if (!f) {
        err = "no such file: " + src;
        return false;
    }
    VDir* p = navDirMut(parentDir(dst));
    if (!p) {
        err = "no such directory: " + parentDir(dst);
        return false;
    }
    std::string b = baseName(dst);
    if (p->dirs.count(b)) {
        err = "is a directory: " + dst;
        return false;
    }
    VFile c = *f;
    c.mtime = tick;
    p->files[b] = std::move(c);
    return true;
}

bool Vfs::move(const std::string& src, const std::string& dst, std::string& err) {
    if (src == "/" || dst == "/") {
        err = "cannot move the root directory";
        return false;
    }
    const VDir* sp = navDir(parentDir(src));
    VDir* dp = navDirMut(parentDir(dst));
    if (!sp || !dp) {
        err = "no such path";
        return false;
    }
    std::string sb = baseName(src), db = baseName(dst);
    // Moving a directory into itself (or its own child) is refused.
    if (dst == src || (dst.size() > src.size() && dst.compare(0, src.size(), src) == 0 &&
                       dst[src.size()] == '/')) {
        err = "cannot move a directory into itself";
        return false;
    }
    auto sfit = sp->files.find(sb);
    if (sfit != sp->files.end()) {
        if (dp->dirs.count(db)) {
            err = "is a directory: " + dst;
            return false;
        }
        // Source and destination live in (maybe) different dirs; copy fields.
        VFile c = sfit->second;
        VDir* smut = navDirMut(parentDir(src));
        if (!smut) {
            err = "no such path: " + src;
            return false;
        }
        smut->files.erase(sb);
        dp->files[db] = std::move(c);
        return true;
    }
    auto sdit = sp->dirs.find(sb);
    if (sdit == sp->dirs.end()) {
        err = "no such file or directory: " + src;
        return false;
    }
    if (dp->files.count(db) || dp->dirs.count(db)) {
        err = "destination already exists: " + dst;
        return false;
    }
    VDir* smut = navDirMut(parentDir(src));
    if (!smut) {
        err = "no such path: " + src;
        return false;
    }
    dp->dirs[db] = std::move(sdit->second);
    smut->dirs.erase(sb);
    return true;
}

bool Vfs::corrupt(const std::string& abs, std::string& err) {
    if (!validAbs(abs) || abs == "/") {
        err = "invalid file path";
        return false;
    }
    VDir* p = navDirMut(parentDir(abs));
    if (!p) {
        err = "no such file: " + abs;
        return false;
    }
    auto it = p->files.find(baseName(abs));
    if (it == p->files.end()) {
        err = "no such file: " + abs;
        return false;
    }
    it->second.corrupted = true;
    return true;
}

bool Vfs::chmod(const std::string& abs, bool add, const std::string& token, std::string& err) {
    if (!validPermToken(token)) {
        err = "invalid permission '" + token + "' (actor rt|o|u + capability r|w,"
              " e.g. ur uw or ow rtr rtw)";
        return false;
    }
    if (!validAbs(abs) || abs == "/") {
        err = "invalid path";
        return false;
    }
    VDir* p = navDirMut(parentDir(abs));
    if (!p) {
        err = "no such path: " + abs;
        return false;
    }
    std::string b = baseName(abs);
    auto fit = p->files.find(b);
    if (fit != p->files.end()) {
        if (add) fit->second.acl.insert(token);
        else fit->second.acl.erase(token);
        return true;
    }
    auto dit = p->dirs.find(b);
    if (dit != p->dirs.end()) {
        if (add) dit->second.acl.insert(token);
        else dit->second.acl.erase(token);
        return true;
    }
    err = "no such file or directory: " + abs;
    return false;
}

bool Vfs::list(const std::string& abs, std::vector<VEntry>& out, std::string& err) const {
    const VDir* d = navDir(abs);
    if (!d) {
        err = "no such directory: " + abs;
        return false;
    }
    for (const auto& [name, sub] : d->dirs) {
        VEntry e;
        e.name = name + "/";
        e.isDir = true;
        e.owner = sub.owner;
        e.acl = sub.acl;
        out.push_back(std::move(e));
    }
    for (const auto& [name, f] : d->files) {
        VEntry e;
        e.name = name;
        e.isDir = false;
        e.size = f.content.size();
        e.owner = f.owner;
        e.executable = f.executable;
        e.corrupted = f.corrupted;
        e.acl = f.acl;
        out.push_back(std::move(e));
    }
    return true;
}

size_t Vfs::fileCount() const {
    size_t n = 0;
    std::vector<const VDir*> stack{&root_};
    while (!stack.empty()) {
        const VDir* d = stack.back();
        stack.pop_back();
        n += d->files.size();
        for (const auto& [name, sub] : d->dirs) {
            stack.push_back(&sub);
        }
    }
    return n;
}

size_t Vfs::totalBytes() const {
    size_t n = 0;
    std::vector<const VDir*> stack{&root_};
    while (!stack.empty()) {
        const VDir* d = stack.back();
        stack.pop_back();
        for (const auto& [name, f] : d->files) n += f.content.size();
        for (const auto& [name, sub] : d->dirs) {
            stack.push_back(&sub);
        }
    }
    return n;
}

bool Vfs::validate(std::string* err) const {
    auto badAcl = [&](const std::set<std::string>& acl) {
        for (const auto& t : acl)
            if (!validPermToken(t)) return true;
        return false;
    };
    std::vector<std::pair<std::string, const VDir*>> stack{{"/", &root_}};
    while (!stack.empty()) {
        auto [path, d] = stack.back();
        stack.pop_back();
        if (badAcl(d->acl)) {
            if (err) *err = "vfs: bad acl under " + path;
            return false;
        }
        for (const auto& [name, sub] : d->dirs) {
            if (name.empty() || name.find('/') != std::string::npos) {
                if (err) *err = "vfs: bad dir name under " + path;
                return false;
            }
            stack.push_back({path == "/" ? "/" + name : path + "/" + name, &sub});
        }
        for (const auto& [name, f] : d->files) {
            if (name.empty() || name.find('/') != std::string::npos) {
                if (err) *err = "vfs: bad file name under " + path;
                return false;
            }
            if (f.perms.empty()) {
                if (err) *err = "vfs: empty perms on " + path + "/" + name;
                return false;
            }
            if (badAcl(f.acl)) {
                if (err) *err = "vfs: bad acl on " + path + "/" + name;
                return false;
            }
        }
    }
    return true;
}

void Vfs::appendDigest(std::ostringstream& o) const {
    auto aclStr = [](const std::set<std::string>& acl) {
        std::string s;
        for (const auto& t : acl) {
            if (!s.empty()) s += ",";
            s += t;
        }
        return s;
    };
    std::vector<std::pair<std::string, const VDir*>> stack{{"/", &root_}};
    std::vector<std::string> lines;
    while (!stack.empty()) {
        auto [path, d] = stack.back();
        stack.pop_back();
        lines.push_back("d(" + path + "," + d->owner + "," + aclStr(d->acl) + ")");
        for (const auto& [name, sub] : d->dirs)
            stack.push_back({path == "/" ? "/" + name : path + "/" + name, &sub});
        for (const auto& [name, f] : d->files) {
            std::string fp = path == "/" ? "/" + name : path + "/" + name;
            lines.push_back("f(" + fp + "," + f.owner + "," + f.perms + "," +
                            (f.executable ? "x" : "-") + "," + (f.corrupted ? "1" : "0") + "," +
                            std::to_string(f.mtime) + "," + aclStr(f.acl) + "," + f.content +
                            ")");
        }
    }
    std::sort(lines.begin(), lines.end());
    for (const auto& l : lines) o << l << ";";
}

} // namespace override
