#pragma once

// Achievement engine: real subsystem, not a command-string checker.
//
// Flow: USER ACTION -> SIMULATION CHANGE -> EVENT/STATE CHANGE ->
// evaluate() -> UNLOCK. Conditions read the unified event ledger and live
// world state (plus shell-observed real reads); definitions, state,
// conditions, unlock logic, progress, and the notification queue all live
// here. The shell only recognizes the command, renders the viewer, and
// displays notifications.
//
// Separation: achievement meta-history is NOT simulation state. Unlocks
// survive rewind/restore/reset/world-switch (those only touch System) and
// never enter world digests (no ledger emission on unlock, by design).
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ordc/ord.hpp"

namespace override {

class System;

// Shell-observed real interactions (read-only subsystem reads, validated
// actions, measured outcomes). Filled by the shell during dispatch; the
// engine treats them as observed behavior, never as command names.
struct ShellObserved {
    std::set<std::string> subsystems; // nodes, causality, network, processes,
                                      // services, vfs, kernel, hardware,
                                      // ledger, resources, system
    bool validated = false;
    bool pingOk = false;
    bool packetOk = false;
    std::set<std::string> fsAreas; // boot, kernel, bin, dev, proc, sys, run,
                                   // etc, var, home, tmp, usr, lib
    bool procInspected = false;
    bool serviceInspected = false;
};

struct AchievementDef {
    const char* id;    // stable identity (display title is NOT the identity)
    const char* title; // display title (hidden until unlocked when hidden)
    const char* desc;
    bool hidden = false;
    int target = 1; // progress target (>= 1)
};

struct AchievementState {
    int progress = 0;
    bool unlocked = false;
    int64_t firstTick = -1; // sim tick of unlock
};

// Viewer navigation model (pure, testable): pages of 10, clamped bounds.
struct AchievementNav {
    int page = 0;
    int pages = 1;
};
enum class NavAction { None, Prev, Next, Exit };
NavAction viewerKey(AchievementNav& nav, int key); // key: 'q'/'e'/arrows
int achievementPageCount(size_t total, int perPage = 10);

class AchievementEngine {
public:
    AchievementEngine();
    static const AchievementDef* defs(size_t& n);

    size_t count() const;
    size_t unlockedCount() const;
    const AchievementDef& def(size_t i) const;
    const AchievementState& state(const std::string& id) const;
    int progressOf(const std::string& id) const;
    bool unlocked(const std::string& id) const;

    // Evaluate new ledger events + live state + shell observations against
    // every locked achievement. Idempotent: already-unlocked never refire.
    // Returns ids unlocked by this call (each queued once for notify).
    std::vector<std::string> evaluate(const System& sys, const ShellObserved& obs);
    // Notification queue: each unlock appears exactly once; drained by the
    // shell after display. Transient: never persisted.
    std::vector<std::string> drainNotifications();

    // achievement.ord persistence (global, independent of worlds).
    void serializeDoc(ordc::OrdDoc& doc) const;
    bool deserializeDoc(const ordc::OrdDoc& doc, std::string& err);

    // One viewer page rendered as text (headless output + interactive pages
    // share this; the interactive loop only adds clearing + key handling).
    std::string renderPage(int page, int perPage = 10) const;
    std::string renderNotification(const std::string& id) const;

private:
    std::map<std::string, AchievementState> states_;
    size_t watermark_ = 0; // ledger events consumed (monotonic)
    std::vector<std::string> queue_;
    int progressFor(const std::string& id, const System& sys, const ShellObserved& obs) const;
};

} // namespace override
