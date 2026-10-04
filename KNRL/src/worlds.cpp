// WorldManager: slot discovery, atomic .ord persistence, session timing.
// All host filesystem access is confined to the app data directory; the
// simulated VFS and the rest of the host are never touched.
#include "override/worlds.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#include "ordc/ord.hpp"
#include "override/account.hpp"
#include "override/achievement.hpp"
#include "override/system.hpp"

namespace override {

namespace fs = std::filesystem;

WallSecs systemWallClock() { return (WallSecs)std::time(nullptr); }

std::string defaultAppDir() {
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    std::string base = (appdata && *appdata) ? appdata : ".";
    return base + "\\OVERRIDE";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    std::string base = (home && *home) ? home : ".";
    return base + "/Library/Application Support/OVERRIDE";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/OVERRIDE";
    const char* home = std::getenv("HOME");
    std::string base = (home && *home) ? home : ".";
    return base + "/.config/OVERRIDE";
#endif
}

WorldManager::WorldManager(std::string appDir, std::function<WallSecs()> clock)
    : appDir_(std::move(appDir)),
      clock_(std::move(clock)),
      packages_((fs::path(appDir_) / "recovery").string()) {}

bool WorldManager::validSlotId(const std::string& id) {
    return id.size() == 2 && id[0] == 'w' && id[1] >= '1' && id[1] <= '6';
}

std::string WorldManager::worldPath(const std::string& id) const {
    return (fs::path(appDir_) / (id + ".ord")).string();
}

std::string WorldManager::achievementPath() const {
    return (fs::path(appDir_) / "achievement.ord").string();
}

std::vector<std::string> WorldManager::discover() const {
    std::vector<std::string> out;
    for (int i = 1; i <= kMaxWorlds; ++i) {
        std::string id = "w" + std::to_string(i);
        std::error_code ec;
        if (fs::is_regular_file(worldPath(id), ec)) out.push_back(id);
    }
    return out;
}

bool WorldManager::worldExists(const std::string& id) const {
    if (!validSlotId(id)) return false;
    std::error_code ec;
    return fs::is_regular_file(worldPath(id), ec);
}

std::string WorldManager::lowestFreeSlot() const {
    for (int i = 1; i <= kMaxWorlds; ++i) {
        std::string id = "w" + std::to_string(i);
        if (!worldExists(id)) return id;
    }
    return "";
}

bool WorldManager::ensureAppDir(std::string& err) const {
    std::error_code ec;
    fs::create_directories(appDir_, ec);
    if (ec) {
        err = "cannot create application data directory '" + appDir_ + "': " + ec.message();
        return false;
    }
    return true;
}

bool WorldManager::atomicWriteFile(const std::string& path, const std::string& content,
                                   std::string& err) const {
    // Confined: only paths inside the app dir are ever written.
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
            std::error_code ec;
            fs::remove(tmp, ec);
            err = "failed writing temporary file '" + tmp + "'";
            return false;
        }
    }
    std::error_code ec;
    if (fs::exists(path, ec)) {
        fs::rename(path, path + ".bak", ec); // keep last valid state
        if (ec) {
            fs::remove(tmp, ec);
            err = "cannot rotate backup for '" + path + "': " + ec.message();
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

bool WorldManager::readFile(const std::string& path, std::string& content,
                            std::string& err) const {
    FILE* f = nullptr;
#if defined(_WIN32)
    fopen_s(&f, path.c_str(), "rb");
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) {
        err = "cannot open '" + path + "'";
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

bool WorldManager::saveWorld(System& sys, std::string& err) {
    const std::string& id = sys.worldId();
    if (id.empty() || !validSlotId(id)) {
        err = "active world has no slot id (assign one before saving)";
        return false;
    }
    if (!ensureAppDir(err)) return false;
    sys.stampWorldSavedTick();
    sys.commitUptimeSession(now());
    ordc::OrdDoc doc;
    if (!sys.serializeWorldDoc(doc, err)) return false; // nothing written on failure
    return atomicWriteFile(worldPath(id), ordc::serializeOrd(doc), err);
}

bool WorldManager::loadWorld(System& sys, const std::string& id, std::string& err) {
    if (!validSlotId(id)) {
        err = "invalid world id '" + id + "' (w1..w6)";
        return false;
    }
    std::string text;
    if (!readFile(worldPath(id), text, err)) {
        err = "world " + id + " does not exist (no " + id + ".ord)";
        return false;
    }
    ordc::OrdDoc doc;
    ordc::OrdError oerr;
    if (!ordc::parseOrd(text, id + ".ord", doc, oerr)) {
        err = "world " + id + " is corrupt: " + oerr.str();
        return false;
    }
    if (!sys.deserializeWorldDoc(doc, err)) {
        err = "world " + id + " failed validation: " + err;
        return false;
    }
    activeId_ = id;
    return true;
}

bool WorldManager::deleteWorld(const std::string& id, std::string& err) {
    if (!validSlotId(id)) {
        err = "invalid world id '" + id + "' (w1..w6)";
        return false;
    }
    if (!worldExists(id)) {
        err = "world " + id + " does not exist";
        return false;
    }
    std::error_code ec;
    fs::remove(worldPath(id), ec);
    if (ec) {
        err = "cannot delete " + id + ".ord: " + ec.message();
        return false;
    }
    fs::remove(worldPath(id) + ".bak", ec); // backup goes with the world
    if (activeId_ == id) activeId_.clear();
    return true;
}

bool WorldManager::saveAchievements(const AchievementEngine& ach, std::string& err) {
    if (!ensureAppDir(err)) return false;
    ordc::OrdDoc doc;
    ach.serializeDoc(doc);
    return atomicWriteFile(achievementPath(), ordc::serializeOrd(doc), err);
}

bool WorldManager::loadAchievements(AchievementEngine& ach, std::string& err) {
    std::error_code ec;
    if (!fs::is_regular_file(achievementPath(), ec)) return true; // none yet: start locked
    std::string text;
    if (!readFile(achievementPath(), text, err)) return false;
    ordc::OrdDoc doc;
    ordc::OrdError oerr;
    if (!ordc::parseOrd(text, "achievement.ord", doc, oerr)) {
        err = "achievements corrupt: " + oerr.str();
        return false;
    }
    return ach.deserializeDoc(doc, err);
}

bool WorldManager::worldInfo(const std::string& id, std::string& name, uint64_t& totalSecs,
                             std::string& err) const {
    if (!worldExists(id)) {
        err = "world " + id + " does not exist";
        return false;
    }
    std::string text;
    if (!readFile(worldPath(id), text, err)) return false;
    ordc::OrdDoc doc;
    ordc::OrdError oerr;
    if (!ordc::parseOrd(text, id + ".ord", doc, oerr)) {
        err = oerr.str();
        return false;
    }
    if (!ordc::getString(doc, "meta", "name", name, oerr)) {
        err = oerr.str();
        return false;
    }
    if (!ordc::getUint64(doc, "uptime", "total_active_seconds", totalSecs, oerr)) {
        err = oerr.str();
        return false;
    }
    return true;
}

std::string WorldManager::accountPath() const {
    return (fs::path(appDir_) / "oup.ord").string();
}

bool WorldManager::saveAccount(const AccountStore& acc, std::string& err) {
    if (!ensureAppDir(err)) return false;
    ordc::OrdDoc doc;
    acc.serializeDoc(doc);
    return atomicWriteFile(accountPath(), ordc::serializeOrd(doc), err);
}

bool WorldManager::loadAccount(AccountStore& acc, std::string& err) const {
    std::error_code ec;
    if (!fs::is_regular_file(accountPath(), ec)) {
        err = "no account configured (no oup.ord)";
        return false;
    }
    std::string text;
    if (!readFile(accountPath(), text, err)) return false;
    ordc::OrdDoc doc;
    ordc::OrdError oerr;
    if (!ordc::parseOrd(text, "oup.ord", doc, oerr)) {
        err = "account file corrupt: " + oerr.str();
        return false;
    }
    if (!acc.deserializeDoc(doc, err)) {
        err = "account file corrupt: " + err;
        return false;
    }
    return true;
}

} // namespace override
