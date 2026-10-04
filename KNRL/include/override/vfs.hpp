#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace override {

// OVERRIDE permission tokens (actor + capability):
//   r = read (5), o = owner (0), u = user (2), w = write (7), rt = root (8)
// Combos: ur uw (user), or ow (owner), rtr rtw (root). Root bypasses checks
// inside the simulation (subject to the engine's hard safety rules); the
// tokens remain explicit for inspection and chmod.
bool validPermToken(const std::string& t); // ^(rt|o|u)(r|w)$
// Default ACL for fresh entries: owner read/write, world read/write.
// Permissive by default for usability; chmod restricts meaningfully.
std::set<std::string> defaultAcl();

// Canonical known-good simulated kernel image bytes: the single literal
// shared by the VFS seed and the kernel recovery package.
std::string canonicalKernelBytes();

struct VFile {
    std::string content;
    std::string owner;              // node name, user name, or "" (system)
    std::string perms = "rw-r--r--";
    bool executable = false;        // simulated exec bit (/bin, /sbin, ...)
    bool corrupted = false;
    uint64_t mtime = 0;             // sim tick of last write
    std::set<std::string> acl = defaultAcl();
};

struct VDir {
    std::map<std::string, VDir> dirs;   // name -> subdir (sorted)
    std::map<std::string, VFile> files; // name -> file (sorted)
    std::string owner;                  // creator user/node or "" (system)
    std::set<std::string> acl = defaultAcl();
};

struct VEntry {
    std::string name;
    bool isDir = false;
    size_t size = 0; // files only
    std::string owner;
    bool executable = false;
    bool corrupted = false;
    std::set<std::string> acl;
};

// OVERKNRL virtual filesystem: purely simulated file tree. No host paths
// are ever touched; every operation resolves inside this in-memory tree.
// Paths are absolute, normalized Unix-style ("/etc/app.conf").
//
// A file whose basename stem matches a node name (e.g. `/etc/server.conf`
// when node `server` exists) is *node-owned*: writes to it are gated by that
// node's rootfs mount state, and corrupting an /etc/ config also corrupts
// the node's configuration. All other files belong to the virtual host.
//
// Access additionally follows per-entry ACLs (see validPermToken above);
// the simulated root operator bypasses them, everyone else needs the
// matching actor token (ur/uw for others, or/ow when operating as owner).

class Vfs {
public:
    Vfs() = default;
    void clear();
    // Seed the standard tree (/bin /sbin /etc /home /root /tmp /usr /var...)
    // plus per-node /etc/<node>.conf stubs owned by each listed node.
    void seedDefaults(const std::vector<std::string>& nodeNames);

    // Normalize to absolute form. "" input or illegal input -> "".
    // Relative paths resolve against cwd. ".." past root clamps to "/".
    static std::string normalize(const std::string& cwd, const std::string& in);
    static std::string parentDir(const std::string& abs);
    static std::string baseName(const std::string& abs);
    static bool validAbs(const std::string& abs);

    bool exists(const std::string& abs) const;
    bool isDir(const std::string& abs) const;
    const VFile* file(const std::string& abs) const;
    const VDir* dir(const std::string& abs) const;

    // Mutations. All return false + err on failure, true on success.
    // tick stamps mtime on writes.
    bool mkdir(const std::string& abs, std::string& err, const std::string& owner = "",
               const std::set<std::string>& acl = {});
    bool touch(const std::string& abs, const std::string& owner, uint64_t tick,
               std::string& err);
    bool write(const std::string& abs, const std::string& content, const std::string& owner,
               uint64_t tick, std::string& err,
               bool executable = false); // create-or-replace
    bool remove(const std::string& abs, std::string& err); // files + empty dirs only
    bool copy(const std::string& src, const std::string& dst, uint64_t tick, std::string& err);
    bool move(const std::string& src, const std::string& dst, std::string& err);
    bool corrupt(const std::string& abs, std::string& err); // files only
    // Adjust one ACL token on a file or directory (+add / -remove).
    // Empty ACLs are allowed (deny-by-default); validation rejects bad tokens.
    bool chmod(const std::string& abs, bool add, const std::string& token, std::string& err);

    // Sorted entries of a directory (dirs first, then files).
    bool list(const std::string& abs, std::vector<VEntry>& out, std::string& err) const;
    size_t fileCount() const;
    // Total stored bytes across all files (for hardware quota accounting).
    size_t totalBytes() const;

    bool validate(std::string* err) const;
    void appendDigest(std::ostringstream& o) const;

private:
    VDir root_;
    const VDir* navDir(const std::string& abs) const;
    VDir* navDirMut(const std::string& abs);
};

} // namespace override
