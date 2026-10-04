#include <iostream>
#include <string>

#include "override/shell.hpp"
#include "override/system.hpp"
#include "override/worlds.hpp"

namespace {
void usage(const char* prog) {
    std::cout << "usage: " << prog << " [--script <file>] [--cmd \"<cmds>\"] [--help]\n"
                 "  no args        interactive OVERSHELL\n"
                 "  --script FILE  run OVERSHELL script file (SCRIPT/HEADLESS mode)\n"
                 "  --cmd \"a; b\"   run semicolon-separated commands headlessly\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        override::WorldManager worlds(override::defaultAppDir());
        override::System sys;
        override::Shell shell(sys, &worlds);
        // Achievement meta-state is global: load once, survive everything.
        {
            std::string aerr;
            if (!worlds.loadAchievements(shell.achievements(), aerr))
                std::cout << "warning: " << aerr << " (achievements start locked)\n";
        }
        // Startup discovery: resume the lowest existing world when present,
        // else begin with an unbacked session (nothing is created silently).
        {
            auto found = worlds.discover();
            if (!found.empty()) {
                std::string err;
                if (worlds.loadWorld(sys, found.front(), err)) {
                    sys.beginUptimeSession(worlds.now());
                    std::cout << "Resumed world " << found.front()
                              << " (session uptime restarted).\n";
                } else {
                    std::cout << "warning: " << err << " (starting fresh session)\n";
                }
            } else {
                std::cout << "OVERRIDE\nNo worlds exist.\n\ncreate w\ncrt w\nhelp\n";
            }
        }

    std::string script;
    std::string batch;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--script" || a == "-s" || a == "--run") && i + 1 < argc) {
            script = argv[++i];
        } else if ((a == "--cmd" || a == "-c" || a == "--command") && i + 1 < argc) {
            batch = argv[++i];
        } else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown arg: " << a << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    int rc = 0;
    if (!script.empty() || !batch.empty()) {
        std::string amsg;
        shell.ensureAccount(false, amsg); // headless: notice only, never blocked
        if (!amsg.empty()) std::cout << amsg << "\n";
    }
    if (!script.empty()) rc = shell.runScript(script);
    else if (!batch.empty()) rc = shell.runCommands(batch);
    else {
        // First-run account flow (interactive only; headless without an
        // account proceeds as simulated root with a notice, never blocked).
        std::string amsg;
        if (!shell.ensureAccount(true, amsg)) {
            if (!amsg.empty()) std::cout << amsg << "\n";
            return 1;
        }
        rc = shell.runInteractive();
    }
    // Every normal exit persists the active world (session committed into
    // its total first); pristine sessions persist nothing.
    std::string msg;
    if (!shell.persistOnExit(msg)) rc = 1;
    if (!msg.empty()) std::cout << msg << "\n";
    return rc;
    } catch (const std::exception& e) {
        std::cerr << "fatal error: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "fatal error: unknown exception\n";
        return 1;
    }
}
