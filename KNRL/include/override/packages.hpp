#pragma once

// Recovery packages: engine-canonical baselines (never world snapshots,
// never host files) persisted under <appdir>/recovery/ as .ord documents.
//
//   recovery/kernel.ord  known-good simulated kernel image (/boot/kernel)
//   recovery/rootfs.ord  known-good OS filesystem baseline (full VFS seed)
//
// PackageManager owns paths, generation, parsing, and validation. Applying
// a package to a world is System's job (transactional load methods); the
// shell only orchestrates. No host kernel/rootfs is ever read or touched.
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "ordc/ord.hpp"
#include "override/progress.hpp"

namespace override {

// Canonical known-good simulated kernel image bytes (identical to the VFS
// seed; the single source of truth both flows share).
std::string canonicalKernelBytes();

struct RootfsFile {
    std::string path;
    std::string owner;
    std::string content;
    bool executable = false;
    std::set<std::string> acl;
};

struct RootfsDir {
    std::string path;
    std::string owner;
    std::set<std::string> acl;
};

struct RootfsContent {
    std::vector<std::string> nodes; // node list the baseline was seeded for
    std::vector<RootfsDir> dirs;    // sorted by path
    std::vector<RootfsFile> files;  // sorted by path
};

class PackageManager {
public:
    explicit PackageManager(std::string dir) : dir_(std::move(dir)) {}

    const std::string& dir() const { return dir_; }
    std::string kernelPath() const;
    std::string rootfsPath() const;
    // Presence on disk (validity is established on read, never assumed).
    bool kernelAvailable() const;
    bool rootfsAvailable() const;

    // Create the package iff missing or invalid. `created` reports whether
    // anything was written (valid existing packages are never regenerated).
    // Rootfs reports genuine per-file build units via progress.
    bool ensureKernel(bool& created, std::string& msg, std::string& err,
                      uint64_t createdAt = 0);
    bool ensureRootfs(const std::vector<std::string>& nodes, bool& created, std::string& msg,
                      std::string& err, uint64_t createdAt = 0, ProgressCb progress = {});

    // Read + fully validate (schema, version, identity, integrity, digest).
    bool readKernel(std::string& bytes, std::string& err) const;
    bool readRootfs(RootfsContent& out, std::string& err) const;

private:
    std::string dir_;
    bool writeFile(const std::string& path, const std::string& content, std::string& err) const;
    bool readFile(const std::string& path, std::string& content, std::string& err) const;
};

} // namespace override
