#pragma once

// WorldManager: persistent world slots (w1..w6) plus the global achievement
// file, all under one platform-specific application data directory (never
// inside the simulated VFS, never the host system outside app data).
//
// A world file exists ONLY when that world exists: no placeholders. Saves
// are atomic (tmp + validate + rename, previous kept as one .bak). The live
// System object is owned by the shell; the manager moves bytes and tracks
// the active identity + wall-clock sessions (injectable for tests).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "override/account.hpp"
#include "override/packages.hpp"

namespace override {

class System;
class AchievementEngine;

using WallSecs = int64_t;
WallSecs systemWallClock(); // platform-neutral wall time (seconds)

// Application data directory for this platform:
// Windows %APPDATA%\OVERRIDE, macOS ~/Library/Application Support/OVERRIDE,
// Linux $XDG_CONFIG_HOME/OVERRIDE or ~/.config/OVERRIDE.
std::string defaultAppDir();

class WorldManager {
public:
    static const int kMaxWorlds = 6;

    explicit WorldManager(std::string appDir,
                          std::function<WallSecs()> clock = systemWallClock);

    const std::string& appDir() const { return appDir_; }
    WallSecs now() const { return clock_(); }
    void setClock(std::function<WallSecs()> c) { clock_ = std::move(c); }

    // Slot ids with existing .ord files, sorted (w1..w6 subset).
    std::vector<std::string> discover() const;
    bool worldExists(const std::string& id) const;
    static bool validSlotId(const std::string& id);
    std::string lowestFreeSlot() const; // "" when all six are taken
    std::string worldPath(const std::string& id) const;
    std::string achievementPath() const;

    const std::string& activeId() const { return activeId_; }
    void setActiveId(const std::string& id) { activeId_ = id; }
    // In-memory account (loaded from oup.ord at startup / first run).
    AccountStore& account() { return account_; }
    const AccountStore& account() const { return account_; }
    // Recovery packages (global engine-canonical baselines in recovery/).
    PackageManager& packages() { return packages_; }
    const PackageManager& packages() const { return packages_; }
    std::string recoveryDir() const { return packages_.dir(); }

    // Persist one world: stamps save tick, commits the uptime session
    // (re-baselined, so repeated saves never double-count), serializes, and
    // atomically replaces <id>.ord (previous kept as .bak). The world's id
    // must already be assigned (see Shell `create w` / `save`).
    bool saveWorld(System& sys, std::string& err);
    // Load replaces the System wholesale; on failure it is left unchanged.
    bool loadWorld(System& sys, const std::string& id, std::string& err);
    // Delete the world's files and free its slot (achievement file untouched).
    bool deleteWorld(const std::string& id, std::string& err);
    // Global achievement meta-state (independent of worlds; survives
    // rewind, reset, switch, and relaunch; notifications never persist).
    bool saveAchievements(const AchievementEngine& ach, std::string& err);
    bool loadAchievements(AchievementEngine& ach, std::string& err);
    // Global account file (oup.ord). Created on first-run registration only.
    std::string accountPath() const;
    bool saveAccount(const AccountStore& acc, std::string& err);
    // False + err when missing OR corrupt (caller decides: re-register flow).
    bool loadAccount(AccountStore& acc, std::string& err) const;
    // Lightweight listing info (name + committed all-time seconds) without
    // instantiating the world. False + err when unreadable.
    bool worldInfo(const std::string& id, std::string& name, uint64_t& totalSecs,
                   std::string& err) const;

    // Generic atomic writer inside the app dir (worlds + achievements).
    bool atomicWriteFile(const std::string& path, const std::string& content,
                         std::string& err) const;
    bool readFile(const std::string& path, std::string& content, std::string& err) const;
    bool ensureAppDir(std::string& err) const;

private:
    std::string appDir_;
    std::function<WallSecs()> clock_;
    std::string activeId_;
    AccountStore account_;
    PackageManager packages_;
};

} // namespace override
