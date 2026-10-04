#pragma once

#include <functional>
#include <string>
#include <vector>

#include "override/achievement.hpp"
#include "override/color.hpp"
#include "override/parser.hpp"
#include "override/system.hpp"

namespace override {

class WorldManager;

// Canonical OVERSHELL verbs (used for help and typo suggestions).
std::vector<std::string> overshellCommands();

// OVERSHELL: thin interactive layer over System.
// Returns false when the shell should exit.
class Shell {
public:
    explicit Shell(System& sys) : sys_(sys) {}
    // World-persistence mode: the live System stays owned by the caller, but
    // world slots, saves, sessions, and uptime run through the manager.
    Shell(System& sys, WorldManager* worlds) : sys_(sys), worlds_(worlds) {}

    int runInteractive();
    // Execute one line. Returns false if shell should exit.
    // `echo` prints output lines; when headless, output still goes to stdout.
    bool execLine(const std::string& line, bool recordHistory = true);
    int runScript(const std::string& path);
    int runCommands(const std::string& batch); // semicolon/newline separated
    // Last line(s) printed via print()/cout-writes the shell made itself.
    // Used by tests to assert diagnostics without capturing stdout.
    const std::string& lastOutput() const { return lastOut_; }
    // Last achievement notification box shown ("" when none this session).
    // Notifications render on stdout, never through print(), so command
    // output assertions stay stable.
    const std::string& lastNotification() const { return lastNotification_; }
    AchievementEngine& achievements() { return achievements_; }
    const AchievementEngine& achievements() const { return achievements_; }
    // Last destructive-operation warning (see `help warnings`). Empty when
    // the previous command needed no warning. Never affects simulation.
    const std::string& lastWarning() const { return lastWarning_; }
    // Interactive mode enables confirmation prompts for CONFIRM-level
    // operations (set by runInteractive; tests stay headless by default).
    void setInteractive(bool on) {
        interactive_ = on;
        color::setInteractive(on);
    }
    bool interactive() const { return interactive_; }
    // Override the confirmation prompt (tests inject yes/no without a tty).
    // Handler receives the prompt text, returns true to proceed.
    void setConfirmHandler(std::function<bool(const std::string&)> h) {
        confirmHandler_ = std::move(h);
    }
    // Password reader for sudo/auth (tests inject without a tty).
    // Receives the prompt, returns the typed password (may be empty).
    void setPasswordReader(std::function<std::string(const std::string&)> h) {
        passwordReader_ = std::move(h);
    }
    // Password source decision (pure state machine, unit-tested):
    // injected reader wins whenever present; otherwise the FLOW-level
    // interactive flag decides console vs fail-closed. The flow flag (not
    // the Shell member flag) matters because first-run account creation
    // runs before runInteractive() ever flags the shell.
    enum class PasswordSource { Injected, Console, FailClosed };
    static PasswordSource passwordSource(bool hasReader, bool interactiveFlow);
    // Current prompt: [root@override]# when simulated root is active,
    // else [<account>@override]$ . Updates immediately with operator state.
    std::string prompt() const;
    // First-run account flow (main calls this at startup). Loads oup.ord;
    // when missing/corrupt and interactive, registers a new account exactly
    // as specified (username + masked password prompts). Headless without an
    // account proceeds as simulated root with a notice. Never throws.
    bool ensureAccount(bool interactive, std::string& msg);
    // Persist-on-exit for world mode: backed worlds save; dirty unbacked
    // sessions claim the lowest free slot; pristine sessions save nothing.
    // Returns a human summary for main() to print. Never throws.
    bool persistOnExit(std::string& msg);
    // Masked password input (asterisks; never echoed or stored in clear).
    // Uses the injected reader when set, else an interactive terminal read;
    // headless without a reader returns "" (callers treat as no-auth).
    // `interactive` is passed explicitly (not member state): first-run
    // account creation runs before runInteractive() ever flags the shell,
    // so it must not depend on the member flag.
    std::string readPassword(const std::string& promptText, bool interactive);

private:
    System& sys_;
    WorldManager* worlds_ = nullptr;
    bool exited_ = false;
    // Output capture is observational (presentation only, like history).
    mutable std::string lastOut_;
    mutable std::string lastWarning_;
    mutable std::string lastNotification_;
    bool interactive_ = false;
    std::function<bool(const std::string&)> confirmHandler_;
    std::function<std::string(const std::string&)> passwordReader_;
    // Achievement meta-state: global, survives rewind/switch/reset (never in
    // digests). Shell fills `observed_` during dispatch; the engine owns the
    // conditions, unlocks, and notification queue.
    AchievementEngine achievements_;
    ShellObserved observed_;

    // Permission kinds for the mode matrix.
    enum class Op { MUTATE, ADVANCE, RESTORE };

    void print(const std::string& s) const;
    bool dispatch(const ParsedCommand& cmd);
    void help(const std::string& topic = "") const;

    // Returns "" when allowed, else a reason string.
    std::string permits(Op op, const std::string& verb);
    bool guard(Op op, const std::string& verb);
    // Destructive-operation gate: prints WARNING, and for CONFIRM-level ops
    // requires --force (headless) or an interactive Continue? [y/N] answer.
    // Returns true when execution may proceed. Never mutates simulation.
    bool riskGate(const std::string& verb, const std::vector<std::string>& args,
                  bool force);
    // --- world-slot operations (world mode only; see worlds.hpp) ---
    bool hasWorlds() const { return worlds_ != nullptr; }
    bool requireWorlds();
    // True when the live System is indistinguishable from a fresh init
    // (nothing would be lost by replacing it).
    bool pristineSession() const;
    // Save the active world (assigning a slot first when unbacked).
    bool saveActive(std::string& err);
    bool createWorld();
    bool joinWorld(const std::string& id);
    bool resetWorld(bool force);
    bool deleteWorld(const std::string& id, bool force);
    void showWorlds() const;
    void showWorld() const;
    void uptCmd(const std::vector<std::string>& args) const;
    // Shared get/load implementation (single copy): `sub` is get|load,
    // `args` the target list, `usagePrefix` "sudo " or "" for diagnostics.
    // Callers enforce privilege first (sudo one-shot auth, or root mode).
    // get never touches world/ledger; load applies verified content
    // transactionally. No SUDO_* audit events here by design.
    void runGetLoad(const std::string& sub, const std::vector<std::string>& args,
                    const std::string& usagePrefix);
    // --- achievements (presentation owns viewer + notifications only) ---
    // Record a real VFS read for FILE SYSTEM EXPLORER area coverage.
    void noteFsArea(const std::string& path);
    // Evaluate unlocks after a command and display new notifications.
    // Interactive: box + ~3s + erase (never sim time). Headless: one box
    // per unlock, no sleeps, no spam beyond the unlock itself.
    void drainAchievements();
    void showUnlock(const std::string& id);
    bool runAchievementViewer();
    void persistAchievements();
    void printCausedBy(uint64_t rootId, const std::string& primary);
    // Hint appended when a service name is unknown but a process shares it.
    // Services and processes are intentionally separate registries.
    std::string svcUnknownHint(const std::string& name) const;
};

} // namespace override
