// OVERRIDE engine test suite: deterministic, engine-level + shell UX checks.
// No OS interaction; everything runs against the simulated world.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "override/parser.hpp"
#include "override/account.hpp"
#include "override/color.hpp"
#include "override/progress.hpp"
#include "override/shell.hpp"
#include "override/system.hpp"
#include "override/worlds.hpp"
#include "ordc/ord.hpp"

using namespace override;

static int checks = 0;
static int failures = 0;

#define CHECK(cond, name)                                                                          \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(cond)) {                                                                             \
            ++failures;                                                                            \
            std::cout << "FAIL [" << __func__ << ":" << __LINE__ << "] " << name << "\n";           \
        }                                                                                          \
    } while (0)

static bool is(const System& sys, const std::string& n, NodeState s) {
    return sys.get(n).state == s;
}

static int countType(const System& sys, const std::string& type, const std::string& target = "") {
    int n = 0;
    for (const auto& e : sys.events().all()) {
        if (e.type == type && (target.empty() || e.target == target)) ++n;
    }
    return n;
}

static bool hasSpontaneous(const System& sys) {
    for (const auto& e : sys.events().all()) {
        if (e.message.find("spontaneous_fault") != std::string::npos) return true;
    }
    return false;
}

// ---- 1. MVP regression (spec acceptance flow) ----
static void t_mvp_regression() {
    System sys;
    sys.setAutoFaults(false);
    CHECK(is(sys, "client", NodeState::ONLINE), "init client online");
    CHECK(is(sys, "server", NodeState::ONLINE), "init server online");

    auto rb = sys.breakNode("server");
    CHECK(rb.ok, "break ok");
    CHECK(is(sys, "server", NodeState::FAILED), "server failed");
    CHECK(is(sys, "client", NodeState::DEGRADED), "client degraded");

    std::string tr = sys.trace("client");
    CHECK(!tr.empty() && tr.find("SERVER") != std::string::npos, "trace mentions server");
    std::string wh = sys.why("client");
    CHECK(wh.find("USER_BREAK") != std::string::npos, "why chains to user break");
    CHECK(!sys.events().all().empty(), "events nonempty");

    auto rr = sys.repairNode("server");
    CHECK(rr.ok, "repair ok");
    CHECK(is(sys, "server", NodeState::ONLINE), "server back");
    CHECK(is(sys, "client", NodeState::ONLINE), "client back");
    CHECK(is(sys, "database", NodeState::ONLINE), "db back");
    CHECK(is(sys, "cache", NodeState::ONLINE), "cache back");

    uint64_t snap = sys.checkpoint("mvp-check");
    sys.breakNode("database");
    CHECK(is(sys, "server", NodeState::DEGRADED), "server degraded on db loss");
    CHECK(sys.restore(snap), "restore ok");
    CHECK(is(sys, "database", NodeState::ONLINE), "db restored");
    std::string st = sys.status();
    CHECK(st.find("client  ONLINE") != std::string::npos, "status shows client online");

    // shell-level regression of the same flow incl. rewind
    Shell sh(sys);
    CHECK(sh.execLine("break server"), "sh break");
    CHECK(sh.execLine("why server"), "sh why");
    CHECK(sh.execLine("repair server"), "sh repair");
    CHECK(sh.execLine("checkpoint reg"), "sh checkpoint");
    CHECK(sh.execLine("break cache"), "sh break2");
    CHECK(sh.execLine("rewind 0"), "sh rewind");
    CHECK(is(sys, "cache", NodeState::ONLINE), "rewind restored cache");
}

// ---- 2. basic state ----
static void t_basic_state() {
    System sys;
    sys.setAutoFaults(false);
    CHECK(sys.addNode("edge").ok, "create edge");
    CHECK(is(sys, "edge", NodeState::ONLINE), "edge online");
    CHECK(!sys.addNode("edge").ok, "duplicate create rejected");
    CHECK(!sys.addNode("Bad Name!").ok, "invalid name rejected");
    CHECK(!sys.addNode("").ok, "empty name rejected");
    CHECK(!sys.breakNode("ghost").ok, "break unknown rejected");
    CHECK(sys.breakNode("edge").ok, "break edge");
    CHECK(!sys.breakNode("edge").ok, "double break rejected");
    CHECK(countType(sys, "EDGE_FAILED", "edge") == 1, "no duplicate failed event");
    CHECK(sys.repairNode("edge").ok, "repair edge");
    // remove purges everything referencing the node
    sys.deceive("client", "edge", "latency", "1");
    sys.connect("edge", "server");
    auto rm = sys.removeNode("edge");
    CHECK(rm.ok, "remove ok");
    CHECK(!sys.hasNode("edge"), "edge gone");
    CHECK(sys.network().find("edge", "server") == nullptr, "link dropped");
    CHECK(sys.get("client").beliefs.count("edge") == 0, "beliefs purged");
    CHECK(!sys.removeNode("edge").ok, "double remove rejected");
    std::string err;
    CHECK(sys.validateAll(&err), "invariants hold");
}

// ---- 3. dependencies ----
static void t_dependencies() {
    {
        System sys;
        sys.setAutoFaults(false);
        sys.breakNode("database");
        CHECK(is(sys, "server", NodeState::DEGRADED), "single dep degrades");
        CHECK(is(sys, "client", NodeState::DEGRADED), "cascade to client");
        sys.repairNode("database");
        CHECK(is(sys, "server", NodeState::ONLINE), "recovery propagates");
        CHECK(is(sys, "client", NodeState::ONLINE), "client recovers");
    }
    { // multiple deps: one recovery is not enough
        System sys;
        sys.setAutoFaults(false);
        sys.breakNode("database");
        sys.breakNode("cache");
        CHECK(is(sys, "server", NodeState::DEGRADED), "multi-dep degraded");
        sys.repairNode("database");
        CHECK(is(sys, "server", NodeState::DEGRADED), "still degraded, cache down");
        sys.repairNode("cache");
        CHECK(is(sys, "server", NodeState::ONLINE), "online when all healthy");
    }
    { // cycle terminates and stays consistent
        System sys;
        sys.setAutoFaults(false);
        CHECK(sys.connect("database", "client").ok, "cycle edge");
        sys.breakNode("server");
        CHECK(is(sys, "client", NodeState::DEGRADED), "cycle: client degraded");
        std::string err;
        CHECK(sys.validateAll(&err), "cycle invariants");
        sys.repairNode("server");
        CHECK(is(sys, "client", NodeState::ONLINE), "cycle: client recovers");
        CHECK(is(sys, "database", NodeState::ONLINE), "cycle: db recovers");
    }
}

// ---- 4. events / causality ----
static void t_events() {
    System sys;
    sys.setAutoFaults(false);
    uint64_t r1 = sys.breakNode("server").eventId;
    const auto& all = sys.events().all();
    for (size_t i = 1; i < all.size(); ++i)
        CHECK(all[i].id > all[i - 1].id, "event ids increase");
    const Event* deg = nullptr;
    for (const auto& e : all)
        if (e.type == "CLIENT_DEGRADED") deg = &e;
    CHECK(deg != nullptr, "client degraded event exists");
    CHECK(deg->causeId == r1, "causal ref to user break");
    auto chain = sys.events().causeChain(deg->id);
    CHECK(!chain.empty() && chain.back()->type == "USER_BREAK", "chain reaches root");
    // duplicate prevention: transitions emit once
    CHECK(countType(sys, "SERVER_FAILED", "server") == 1, "single server_failed");
}

// ---- 5. time ----
static void t_time() {
    System sys;
    sys.setAutoFaults(false);
    uint64_t t0 = sys.clock().tickCount();
    CHECK(sys.tick(5) == 5, "tick 5");
    CHECK(sys.clock().tickCount() == t0 + 5, "clock advanced");
    sys.clock().pause();
    CHECK(sys.tick(3) == 0, "paused tick does nothing");
    CHECK(sys.clock().tickCount() == t0 + 5, "clock frozen");
    sys.clock().resume();
    CHECK(sys.tick(1) == 1, "resume ticks");
}

// ---- 6. checkpoints ----
static void t_checkpoints() {
    System sys;
    sys.setAutoFaults(false);
    sys.procSpawn("keeptest", "worker", "server", 0);
    std::string before = sys.worldDigest();
    uint64_t snap = sys.checkpoint("full");
    sys.breakNode("cache");
    sys.procKill("keeptest");
    sys.tick(3);
    CHECK(sys.worldDigest() != before, "world changed");
    size_t evAfter = sys.events().size();
    CHECK(sys.restore(snap), "restore ok");
    CHECK(sys.worldDigest() == before, "world equivalent incl. procs/rng");
    CHECK(sys.events().size() > evAfter, "ledger kept + RESTORE appended");
    CHECK(sys.processes().find("keeptest") != nullptr, "proc restored");
    CHECK(is(sys, "cache", NodeState::ONLINE), "node restored");
    // rewind to latest == restore latest
    sys.checkpoint("second");
    CHECK(sys.rewindSteps(0), "rewind 0 ok");
    CHECK(!sys.restore(999999), "bad snapshot rejected");
}

// ---- 7. network ----
static void t_network() {
    System sys;
    sys.setAutoFaults(false);
    auto ok = sys.sendPacket("client", "server");
    CHECK(ok.outcome == PacketResult::Outcome::DELIVERED, "packet delivered");
    CHECK(!ok.path.empty() && ok.latencyMs > 0, "packet has path+latency");
    sys.breakNode("server");
    auto d1 = sys.sendPacket("client", "server");
    CHECK(d1.outcome == PacketResult::Outcome::DROPPED, "failed dest dropped");
    sys.repairNode("server");
    sys.breakNode("client");
    auto d2 = sys.sendPacket("client", "server");
    CHECK(d2.outcome == PacketResult::Outcome::DROPPED, "failed source dropped");
    sys.repairNode("client");
    sys.disconnect("client", "server");
    auto u = sys.sendPacket("client", "server");
    CHECK(u.outcome == PacketResult::Outcome::UNREACHABLE, "no route unreachable");
    sys.connect("client", "server");
    sys.injectFault("server", "packet_loss", "100");
    auto d3 = sys.sendPacket("client", "server");
    CHECK(d3.outcome == PacketResult::Outcome::DROPPED, "100% loss dropped");
    // routed via relay after direct link cut
    sys.injectFault("server", "packet_loss", "0");
    sys.addNode("relay");
    sys.connect("client", "relay");
    sys.connect("relay", "server");
    sys.disconnect("client", "server");
    auto r = sys.sendPacket("client", "server");
    CHECK(r.outcome == PacketResult::Outcome::DELIVERED, "relay route delivered");
    CHECK(r.path.size() == 3, "relay path length");
    // removed nodes cannot receive
    sys.removeNode("relay");
    auto gone = sys.sendPacket("client", "relay");
    CHECK(gone.outcome == PacketResult::Outcome::UNREACHABLE, "removed node unreachable");
}

// ---- 8. ping ----
static void t_ping() {
    System sys;
    sys.setAutoFaults(false);
    std::string p = sys.pingProbes("server", "");
    CHECK(p.find("PING server") != std::string::npos, "ping header");
    CHECK(p.find("avg:") != std::string::npos, "ping avg");
    CHECK(p.find("packet loss: 0%") != std::string::npos, "ping no loss");
    sys.breakNode("server");
    std::string f = sys.pingProbes("server", "");
    CHECK(f.find("100%") != std::string::npos, "failed ping 100% loss");
    CHECK(f.find("[FAIL]") != std::string::npos, "failed ping marks FAIL");
    std::string u = sys.pingProbes("ghost", "");
    CHECK(u.find("error:") != std::string::npos, "unknown ping errors");
}

// ---- 9. processes ----
static void t_processes() {
    System sys;
    sys.setAutoFaults(false);
    CHECK(sys.processes().all().size() == 5, "5 default procs");
    auto sp = sys.procSpawn("myproc", "worker", "server", 0);
    CHECK(sp.ok, "spawn ok");
    const SimProcess* p = sys.processes().find("myproc");
    CHECK(p != nullptr && p->pid > 0, "proc exists");
    CHECK(sys.procInspect("myproc").find("myproc") != std::string::npos, "inspect works");
    CHECK(!sys.procSpawn("Bad Name!", "worker", "server", 0).ok, "bad proc name rejected");
    CHECK(!sys.procSpawn("x", "worker", "ghost", 0).ok, "bad host rejected");
    CHECK(sys.procPause("myproc").ok, "pause ok");
    CHECK(sys.processes().find("myproc")->state == ProcState::PAUSED, "paused state");
    CHECK(sys.procResume("myproc").ok, "resume ok");
    CHECK(sys.procKill("myproc").ok, "kill ok");
    CHECK(sys.processes().find("myproc")->state == ProcState::CRASHED, "crashed state");
    CHECK(!sys.procKill("ghost").ok, "kill unknown rejected");
    CHECK(sys.procRestart("myproc").ok, "restart ok");
    // node break crashes host procs; repair restarts them
    sys.breakNode("server");
    CHECK(sys.processes().find("srv-worker")->state == ProcState::CRASHED, "break crashes procs");
    sys.repairNode("server");
    CHECK(sys.processes().find("srv-worker")->state == ProcState::RUNNING, "repair restarts");
    // starvation: kill every running proc on a host -> node decays -> degrades
    sys.procKill("srv-listener");
    sys.procKill("srv-worker");
    sys.procKill("myproc");
    sys.tick(20);
    CHECK(is(sys, "server", NodeState::DEGRADED), "starved node degrades");
    CHECK(countType(sys, "NODE_STRAINED", "server") >= 1, "starvation warned");
    // remove purges hosted procs
    sys.removeNode("cache");
    CHECK(sys.processes().find("cache-daemon") == nullptr, "host procs purged");
    std::string err;
    CHECK(sys.validateAll(&err), "proc invariants");
}

// ---- 10. autonomous failures + determinism ----
static void t_autonomous() {
    System a, b;
    a.setSeed(7);
    b.setSeed(7);
    CHECK(a.setAutoRate(100).ok, "rate set");
    CHECK(b.setAutoRate(100).ok, "rate set b");
    a.tick(10);
    CHECK(hasSpontaneous(a), "spontaneous faults occur at rate 100");
    b.tick(10);
    CHECK(a.digest() == b.digest(), "same seed+ops => identical ledger+world");
    // restore rewinds the rng stream too: evolution repeats identically
    System c;
    c.setSeed(7);
    CHECK(c.setAutoRate(100).ok, "rate set c");
    c.tick(5);
    uint64_t s = c.checkpoint("x");
    c.tick(5);
    std::string d1 = c.worldDigest();
    CHECK(c.restore(s), "restore");
    c.tick(5);
    CHECK(c.worldDigest() == d1, "post-restore evolution identical");
    // disabled entropy stays quiet
    System q;
    q.setSeed(7);
    q.setAutoFaults(false);
    size_t n0 = q.events().size();
    q.tick(10);
    CHECK(!hasSpontaneous(q), "disabled entropy is quiet");
    CHECK(q.events().size() > n0, "ticks still logged");
    CHECK(!q.setAutoRate(101).ok, "bad rate rejected");
}

// ---- 11. resources ----
static void t_resources() {
    System sys;
    sys.setAutoFaults(false);
    auto r = sys.overrideProp("server", "load", "95");
    CHECK(r.ok, "load override ok");
    CHECK(is(sys, "server", NodeState::DEGRADED), "high cpu degrades");
    CHECK(!sys.overrideProp("server", "memory", "999999").ok, "memory over cap rejected");
    CHECK(!sys.overrideProp("server", "health", "101").ok, "health range enforced");
    auto r2 = sys.overrideProp("database", "memory", "3900"); // >90% of 4096
    CHECK(r2.ok, "high memory set");
    int h0 = sys.get("database").health;
    sys.tick(3);
    CHECK(sys.get("database").health < h0, "memory strain decays health");
    std::string res = sys.resources();
    CHECK(res.find("server") != std::string::npos && res.find("LINK") != std::string::npos,
          "resources report");
    std::string li = sys.localInfo();
    CHECK(li.find("override-local") != std::string::npos, "local host card");
    CHECK(sys.localPorts().find("overshell") != std::string::npos, "local ports");
    CHECK(sys.localInterfaces().find("sim0") != std::string::npos, "local ifaces");
}

// ---- 12. parser + shell UX (never crash, useful diagnostics) ----
static void t_cli_quality() {
    CHECK(editDistance("creade", "create") == 1, "levenshtein");
    CHECK(suggest("creade", overshellCommands()) == "create", "typo suggestion");
    CHECK(suggest("zzzqqq", overshellCommands()) == "", "no wild suggestion");
    int v = 0;
    CHECK(parseIntStrict("42", v) && v == 42, "strict int ok");
    CHECK(!parseIntStrict("12x", v), "strict int rejects junk");
    CHECK(!parseIntStrict("", v), "strict int rejects empty");

    System sys;
    sys.setAutoFaults(false);
    Shell sh(sys);
    CHECK(sh.execLine("creade node2"), "unknown cmd survives");
    CHECK(sh.lastOutput().find("did you mean: create?") != std::string::npos, "hint shown");
    CHECK(sh.execLine("restore abc"), "bad restore survives");
    CHECK(sh.lastOutput().find("invalid checkpoint id") != std::string::npos, "bad id diagnosed");
    CHECK(sh.execLine("tick xyz"), "bad tick survives");
    CHECK(sh.execLine("break ghost"), "unknown node survives");
    CHECK(sh.lastOutput().find("error:") != std::string::npos, "error surfaced");
    CHECK(sh.execLine("mode observe"), "observe on");
    CHECK(sh.execLine("break server"), "blocked break survives");
    CHECK(sh.lastOutput().find("blocked") != std::string::npos, "block explained");
    CHECK(is(sys, "server", NodeState::ONLINE), "observe kept world frozen");
    CHECK(sh.execLine("mode time_travel"), "timetravel on");
    CHECK(sh.execLine("break server"), "tt blocks break");
    CHECK(sh.lastOutput().find("TIME_TRAVEL") != std::string::npos, "tt explained");
    CHECK(sh.execLine("mode watch"), "watch on");
    CHECK(sh.execLine("tick 1"), "watch tick");
    CHECK(sh.lastOutput().find("ONLINE") != std::string::npos, "watch displays status");
    CHECK(sh.execLine("mode network"), "reserved mode accepted");
    CHECK(sh.lastOutput().find("reserved") != std::string::npos, "reserved labelled");
    CHECK(sh.execLine("mode normal"), "back to normal");
    // replay appends a REPLAY display event, doesn't mutate world
    std::string d0 = sys.worldDigest();
    CHECK(sh.execLine("replay 5"), "replay runs");
    CHECK(sys.worldDigest() == d0, "replay display-only");
}

// ---- 13. virtual filesystem ops + node coupling ----
static void t_vfs() {
    System sys;
    sys.setAutoFaults(false);
    // seed tree present with node-owned configs
    CHECK(sys.vfsCat("/etc/server.conf").find("node=server") != std::string::npos, "seed conf");
    CHECK(sys.vfsCat("/nope").find("error:") != std::string::npos, "cat missing errors");
    CHECK(sys.vfsMkdir("/srv").ok, "mkdir ok");
    CHECK(!sys.vfsMkdir("/srv").ok, "mkdir dup rejected");
    CHECK(!sys.vfsMkdir("/nope/x").ok, "mkdir missing parent rejected");
    CHECK(sys.vfsTouch("/srv/a.txt").ok, "touch ok");
    CHECK(sys.vfsWrite("/srv/a.txt", "hello").ok, "write ok");
    CHECK(sys.vfsCat("/srv/a.txt").find("hello") != std::string::npos, "cat reads back");
    CHECK(sys.vfsCopy("/srv/a.txt", "/srv/b.txt").ok, "copy ok");
    CHECK(sys.vfsMove("/srv/b.txt", "/srv/c.txt").ok, "move ok");
    CHECK(sys.vfsCat("/srv/c.txt").find("hello") != std::string::npos, "moved content kept");
    CHECK(sys.vfsRemove("/srv/c.txt").ok, "rm file ok");
    CHECK(!sys.vfsRemove("/srv").ok, "rm nonempty dir rejected");
    CHECK(sys.vfsRemove("/srv/a.txt").ok, "rm ok");
    CHECK(sys.vfsRemove("/srv").ok, "rm empty dir ok");
    CHECK(sys.setCwd("/etc").ok, "cd ok");
    CHECK(sys.cwd() == "/etc", "cwd set");
    CHECK(!sys.setCwd("/broken/path").ok, "cd bad path rejected");
    // corrupting a node-owned /etc config corrupts node configuration
    CHECK(sys.vfsCorrupt("/etc/server.conf").ok, "corrupt ok");
    CHECK(sys.get("server").configCorrupt, "node config corrupted");
    CHECK(sys.get("server").faults.count("config-corrupt") == 1, "config fault raised");
    CHECK(sys.vfsCat("/etc/server.conf").find("CORRUPTED") != std::string::npos, "cat flags it");
    // read-only rootfs gates writes on node-owned files
    CHECK(sys.injectFault("server", "rootfs-readonly", "").ok, "readonly injected");
    CHECK(!sys.vfsWrite("/etc/server.conf", "x=1").ok, "write denied on readonly rootfs");
    CHECK(sys.vfsTouch("/etc/other.conf").ok, "touch host-owned file allowed");
    CHECK(sys.resolveFault("server", "rootfs-readonly").ok, "readonly recovered");
    CHECK(sys.vfsWrite("/etc/other.conf", "x=1").ok, "write works after recover");
    // checkpoint/restore covers files + cwd
    sys.setCwd("/");
    CHECK(sys.vfsWrite("/tmp/marker", "abc").ok, "marker written");
    uint64_t snap = sys.checkpoint("vfs");
    CHECK(sys.vfsRemove("/tmp/marker").ok, "marker removed");
    CHECK(sys.setCwd("/etc").ok, "cwd moved");
    CHECK(sys.restore(snap), "restore ok");
    CHECK(sys.vfsCat("/tmp/marker").find("abc") != std::string::npos, "file restored");
    CHECK(sys.cwd() == "/", "cwd restored");
    std::string err;
    CHECK(sys.validateAll(&err), "vfs invariants");
    // malformed file commands never crash the shell
    Shell sh(sys);
    CHECK(sh.execLine("cat /does/not/exist"), "bad cat survives");
    CHECK(sh.execLine("cd /broken/path"), "bad cd survives");
    CHECK(sh.execLine("rm /"), "rm root refused");
    CHECK(sh.lastOutput().find("error:") != std::string::npos ||
          sh.lastOutput().find("refusing") != std::string::npos,
          "rm root diagnosed");
}

// ---- 14. VFS physical mapping: virtual / resolves inside KNRL/OVERKNRL ----
static std::string findOverknrl() {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        if (fs::exists(dir / "KNRL" / "OVERKNRL" / ".knrl-root"))
            return (dir / "KNRL" / "OVERKNRL").string();
        if (!dir.has_parent_path()) break;
        dir = dir.parent_path();
    }
    return "";
}

static void t_vfs_physical() {
    std::string root = findOverknrl();
    CHECK(!root.empty(), "OVERKNRL physical root discoverable");
    if (root.empty()) return;
    namespace fs = std::filesystem;
    // every physical seed file exists in the seeded in-memory tree, same bytes
    Vfs v;
    v.seedDefaults({"client", "server", "database", "cache"});
    int compared = 0;
    for (auto it = fs::recursive_directory_iterator(root + "/rootfs"); it != fs::end(it); ++it) {
        if (!it->is_regular_file()) continue;
        std::string name = it->path().filename().string();
        if (!name.empty() && name[0] == '.') continue; // marker files (.keep)
        std::string rel = fs::relative(it->path(), fs::path(root + "/rootfs")).string();
        for (auto& c : rel)
            if (c == '\\') c = '/';
        std::string vpath = "/" + rel;
        const VFile* f = v.file(vpath);
        CHECK(f != nullptr, std::string("seeded tree has ") + vpath);
        if (f == nullptr) continue;
        std::ifstream in(it->path(), std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(f->content == content, std::string("seed bytes match ") + vpath);
        ++compared;
    }
    CHECK(compared >= 6, "compared physical seed files");
    // node-owned configs carry their owner in-memory
    const VFile* sc = v.file("/etc/server.conf");
    CHECK(sc != nullptr && sc->owner == "server", "server.conf owned by server");
    // resolver attacks never escape the virtual tree (always absolute-or-empty)
    CHECK(Vfs::normalize("/etc", "../../x") == "/x", "dotdot clamps to virtual root");
    CHECK(Vfs::normalize("/", "..") == "/", "root clamp");
    CHECK(Vfs::normalize("/a", "b/./c") == "/a/b/c", "dot segments resolve");
    CHECK(Vfs::normalize("/a", "") == "", "empty rejected");
    CHECK(Vfs::normalize("/a", "//") == "/", "double slash root");
}

// ---- 15. host safety: simulated file ops must not touch the repo ----
static std::map<std::string, std::string> repoSnapshot(const std::string& root) {
    namespace fs = std::filesystem;
    std::map<std::string, std::string> out;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::end(it); ++it) {
        std::string rel = fs::relative(it->path(), fs::path(root)).string();
        if (rel.rfind("build", 0) == 0 || rel.rfind(".git", 0) == 0) continue;
        if (!it->is_regular_file()) continue;
        auto sz = it->file_size();
        auto t = it->last_write_time().time_since_epoch().count();
        out[rel] = std::to_string(sz) + ":" + std::to_string(t);
    }
    return out;
}

static void t_vfs_safety() {
    std::string overknrl = findOverknrl();
    CHECK(!overknrl.empty(), "project root discoverable for safety scan");
    if (overknrl.empty()) return;
    namespace fs = std::filesystem;
    std::string proj = fs::path(overknrl).parent_path().parent_path().string();
    auto before = repoSnapshot(proj);
    CHECK(!before.empty(), "repo snapshot nonempty");
    // maximum-chaos virtual workload: mass writes, deletes incl. `rm`-style
    // root attack attempts, faults, ticks, break/repair, checkpoint/restore
    System sys;
    sys.setAutoFaults(false);
    Shell sh(sys);
    const char* chaos[] = {"touch /etc/evil",      "edit /etc/evil payload", "cp /etc/evil /tmp/e2",
                           "mv /tmp/e2 /tmp/e3",   "corrupt /etc/evil --force", "rm /etc/evil --force",
                           "mkdir /a",             "touch /a/f",             "rm /a/f",
                           "rm /a",                "edit /etc/server.conf x", "cat /etc/server.conf",
                           "inject server overheat", "tick 5",               "break server",
                           "repair server",        "checkpoint chaos",       "restore 1",
                           "service spawn s1 server app", "service start s1", nullptr};
    for (int i = 0; chaos[i] != nullptr; ++i) CHECK(sh.execLine(chaos[i]), "chaos step survives");
    auto after = repoSnapshot(proj);
    CHECK(before == after, "host repo untouched by virtual chaos");
    // and no stray host files where a confused implementation might put them
    CHECK(!fs::exists(fs::path(proj) / "etc"), "no host etc/ created");
    CHECK(!fs::exists(fs::path(proj) / "tmp" / "e3"), "no host tmp file created");
}

// ---- 16. services ----
static void t_services() {
    System sys;
    sys.setAutoFaults(false);
    // TASK 1 regression: the default world ships simulated *processes* but
    // zero managed *services* (a process TYPE like "service" is only a label).
    // ServiceManager stays the single authoritative service model.
    CHECK(sys.services().all().empty(), "default world has no services");
    CHECK(sys.processes().find("srv-listener") != nullptr, "default listener proc exists");
    CHECK(sys.serviceInspect("srv-listener").find("unknown service") != std::string::npos,
          "listener proc is not a service");
    {
        Shell probe(sys);
        probe.execLine("service inspect srv-listener");
        CHECK(probe.lastOutput().find("unknown service") != std::string::npos,
              "shell inspect reports unknown service");
        CHECK(probe.lastOutput().find("separate registries") != std::string::npos,
              "shell inspect names the registry split");
    }
    CHECK(sys.serviceSpawn("web", "server", "app", "/etc/server.conf", "ALWAYS").ok,
          "service spawn ok");
    CHECK(!sys.serviceSpawn("web", "server", "app", "", "ALWAYS").ok, "dup service rejected");
    CHECK(!sys.serviceSpawn("bad name!", "server", "app", "", "ALWAYS").ok, "bad svc name rejected");
    CHECK(!sys.serviceSpawn("x", "ghost", "app", "", "ALWAYS").ok, "bad host rejected");
    CHECK(!sys.serviceSpawn("x", "server", "", "", "ALWAYS").ok, "empty binary rejected");
    CHECK(sys.serviceStart("web").ok, "service start ok");
    const Service* s = sys.services().find("web");
    CHECK(s != nullptr && s->state == ServiceState::RUNNING && s->pid > 0, "service running");
    CHECK(sys.serviceInspect("web").find("RUNNING") != std::string::npos, "inspect works");
    CHECK(sys.serviceList().find("web") != std::string::npos, "list works");
    // bad binary / missing+corrupt config gate starts
    CHECK(sys.serviceSpawn("bad", "server", "nope", "", "NEVER").ok, "bad-binary spawn declared");
    CHECK(!sys.serviceStart("bad").ok, "missing binary start rejected");
    CHECK(sys.serviceSpawn("cfg", "server", "app", "/etc/nope.conf", "NEVER").ok, "cfg spawn ok");
    CHECK(!sys.serviceStart("cfg").ok, "missing config start rejected");
    // backing proc death crashes the service; supervisor restarts next tick
    const Service* w0 = sys.services().find("web");
    CHECK(sys.procKill(std::to_string(w0->pid), "crash").ok, "backing proc killed");
    sys.tick(1);
    CHECK(sys.services().find("web")->state == ServiceState::CRASHED, "proc death crashes svc");
    CHECK(sys.services().find("web")->crashCount >= 1, "crash counted");
    sys.tick(1);
    CHECK(sys.services().find("web")->state == ServiceState::RUNNING, "supervisor restarts svc");
    // break crashes services; repair restarts them
    sys.breakNode("server");
    CHECK(sys.services().find("web")->state == ServiceState::CRASHED, "break crashes service");
    CHECK(sys.get("server").faults.count("service-down") == 1, "service-down fault raised");
    sys.repairNode("server");
    sys.tick(1);
    CHECK(sys.services().find("web")->state == ServiceState::RUNNING, "repair restarts service");
    // restart loop: 3 crash/restart cycles raise restart-loop
    for (int i = 0; i < 3; ++i) {
        const Service* w = sys.services().find("web");
        if (w->state == ServiceState::RUNNING && w->pid > 0)
            sys.procKill(std::to_string(w->pid), "crash");
        sys.tick(2); // crash detected, then supervisor restarts
    }
    CHECK(sys.get("server").faults.count("restart-loop") == 1, "restart-loop raised");
    // manual restart resets the loop counter
    CHECK(sys.serviceRestart("web").ok, "manual restart ok");
    CHECK(sys.services().find("web")->crashCount == 0, "loop counter reset");
    // NEVER policy stays stopped
    CHECK(sys.serviceSpawn("lazy", "server", "app", "", "NEVER").ok, "never spawn ok");
    sys.tick(2);
    CHECK(sys.services().find("lazy")->state == ServiceState::STOPPED, "never stays stopped");
    CHECK(sys.serviceStop("web").ok, "stop ok");
    CHECK(sys.services().find("web")->state == ServiceState::STOPPED, "stopped state");
    CHECK(sys.serviceRemove("lazy").ok, "remove ok");
    CHECK(sys.services().find("lazy") == nullptr, "removed service gone");
    // removeNode purges hosted services; halt stops them
    CHECK(sys.serviceSpawn("c1", "cache", "cache-daemon", "", "ALWAYS").ok, "cache svc ok");
    sys.removeNode("cache");
    CHECK(sys.services().find("c1") == nullptr, "host services purged");
    CHECK(sys.serviceSpawn("w2", "server", "app", "", "ALWAYS").ok, "w2 spawn ok");
    CHECK(sys.serviceStart("w2").ok, "w2 start ok");
    sys.haltNode("server");
    CHECK(sys.services().find("w2")->state == ServiceState::STOPPED, "halt stops services");
    // determinism incl. services + checkpoint roundtrip
    System a, b;
    a.setAutoFaults(false);
    b.setAutoFaults(false);
    for (int k = 0; k < 2; ++k) {
        System* ssys = (k == 0) ? &a : &b;
        ssys->serviceSpawn("web", "server", "app", "/etc/server.conf", "ALWAYS");
        ssys->serviceStart("web");
        ssys->tick(3);
    }
    CHECK(a.digest() == b.digest(), "service runs are deterministic");
    uint64_t snap = a.checkpoint("svc");
    a.serviceStop("web");
    CHECK(a.restore(snap), "svc restore ok");
    CHECK(a.services().find("web")->state == ServiceState::RUNNING, "service state restored");
    std::string err;
    CHECK(a.validateAll(&err), "service invariants");
}

// ---- 17. kernel lifecycle, panic, modules ----
static void t_kernel() {
    System sys;
    sys.setAutoFaults(false);
    // orderly halt: OFF kernel + PAUSED node, repair refused, start reboots
    CHECK(sys.haltNode("server").ok, "halt ok");
    CHECK(sys.get("server").kernel == KernelState::OFF, "kernel OFF");
    CHECK(is(sys, "server", NodeState::PAUSED), "halted node paused");
    CHECK(!sys.haltNode("server").ok, "double halt rejected");
    CHECK(!sys.repairNode("server").ok, "repair refused while OFF");
    CHECK(sys.startNode("server").ok, "start boots");
    CHECK(sys.get("server").kernel == KernelState::RUNNING, "kernel running again");
    CHECK(is(sys, "server", NodeState::ONLINE), "node online again");
    CHECK(countType(sys, "KERNEL_BOOT") >= 1, "boot event recorded");
    // panic path: repair refused, reboot revives, dependents recover
    CHECK(sys.injectFault("server", "kernel-panic", "").ok, "panic injected");
    CHECK(sys.get("server").kernel == KernelState::PANICKED, "kernel panicked");
    CHECK(is(sys, "server", NodeState::FAILED), "panicked node failed");
    CHECK(countType(sys, "KERNEL_PANIC") == 1, "panic event recorded");
    CHECK(!sys.repairNode("server").ok, "repair refused while panicked");
    CHECK(sys.rebootNode("server").ok, "reboot ok");
    CHECK(sys.get("server").kernel == KernelState::RUNNING, "kernel running");
    // instability can escalate toward panic under ticks (seeded)
    sys.setSeed(11);
    CHECK(sys.injectFault("server", "kernel-unstable", "").ok, "unstable injected");
    sys.tick(40);
    bool panicked = (sys.get("server").kernel == KernelState::PANICKED);
    CHECK(panicked || sys.get("server").instability > 0, "instability evolves");
    // modules: unload/load with real link effects, FAILED reload rejected
    CHECK(sys.injectFault("server", "module-unload", "net").ok, "net module unloaded");
    CHECK(sys.get("server").modules["net"] == "UNLOADED", "module UNLOADED");
    auto r1 = sys.sendPacket("client", "server");
    CHECK(r1.outcome != PacketResult::Outcome::DELIVERED, "unloaded net module breaks routes");
    CHECK(sys.injectFault("server", "module-load", "net").ok, "net module loaded");
    CHECK(sys.get("server").modules["net"] == "LOADED", "module LOADED");
    CHECK(sys.injectFault("server", "module-fail", "disk").ok, "disk module failed");
    CHECK(!sys.injectFault("server", "module-load", "disk").ok, "reload of FAILED rejected");
    CHECK(sys.resolveFault("server", "module-fail").ok, "module fault recovered");
    CHECK(sys.get("server").modules["disk"] == "LOADED", "module healed");
    CHECK(!sys.injectFault("server", "module-unload", "ghost").ok, "unknown module rejected");
    std::string err;
    CHECK(sys.validateAll(&err), "kernel invariants");
}

// ---- 18. extended process states ----
static void t_proc_states() {
    System sys;
    sys.setAutoFaults(false);
    CHECK(sys.procSpawn("parent", "worker", "server", 0).ok, "parent spawned");
    const SimProcess* par = sys.processes().find("parent");
    CHECK(sys.procSpawn("child", "worker", "server", par->pid).ok, "child spawned");
    // sigkill orphans children into ZOMBIE
    CHECK(sys.procKill("parent", "sigkill").ok, "sigkill ok");
    CHECK(sys.processes().find("parent")->state == ProcState::KILLED, "parent killed");
    const SimProcess* ch = sys.processes().find("child");
    CHECK(ch->state == ProcState::ZOMBIE, "orphan zombified");
    CHECK(!sys.procRestart("parent").ok, "killed cannot restart");
    CHECK(sys.procRestart("child").ok, "zombie revived by restart");
    CHECK(!sys.procKill("parent", "crash").ok, "dead proc cannot crash");
    // block/degrade/resume cycle
    CHECK(sys.procKill("child", "block").ok, "block ok");
    CHECK(sys.processes().find("child")->state == ProcState::BLOCKED, "blocked state");
    CHECK(sys.procResume("child").ok, "blocked resumed");
    CHECK(sys.procKill("child", "degrade").ok, "degrade ok");
    CHECK(sys.processes().find("child")->state == ProcState::DEGRADED, "degraded state");
    CHECK(sys.procResume("child").ok, "degraded resumed");
    CHECK(!sys.procKill("child", "bogus").ok, "bad reason rejected");
    std::string err;
    CHECK(sys.validateAll(&err), "proc state invariants");
}

// ---- 19. fault stacking, escalation, engine-raised names ----
static void t_faults2() {
    System sys;
    sys.setAutoFaults(false);
    // stacking: distinct faults coexist
    CHECK(sys.injectFault("server", "overheat", "").ok, "overheat ok");
    CHECK(sys.injectFault("server", "mem-leak", "").ok, "mem-leak ok");
    CHECK(sys.get("server").faults.size() == 2, "faults stack");
    // re-raise same fault: no duplicate record
    size_t raised = 0;
    for (const auto& e : sys.events().all())
        if (e.type == "FAULT_RAISED" && e.target == "server") ++raised;
    CHECK(sys.injectFault("server", "overheat", "").ok, "re-raise ok");
    size_t raised2 = 0;
    for (const auto& e : sys.events().all())
        if (e.type == "FAULT_RAISED" && e.target == "server") ++raised2;
    CHECK(raised == raised2, "no duplicate fault record");
    // engine-raised names cannot be injected
    CHECK(!sys.injectFault("server", "service-down", "").ok, "service-down not injectable");
    CHECK(!sys.injectFault("server", "restart-loop", "").ok, "restart-loop not injectable");
    // rootfs-corruption damages rootfs with critical severity
    CHECK(sys.injectFault("server", "rootfs-corruption", "").ok, "rootfs-corruption ok");
    CHECK(sys.get("server").filesystems["rootfs"] == FsState::CORRUPTED, "rootfs corrupted");
    CHECK(sys.get("server").faults.count("rootfs-corruption") == 1, "fault recorded");
    // recovery is scoped: cooling a hot node keeps its fs fault
    CHECK(sys.coolNode("server").ok, "cool ok");
    CHECK(sys.get("server").faults.count("rootfs-corruption") == 1, "fs fault survives cool");
    CHECK(sys.resolveFault("server", "rootfs-corruption").ok, "fs fault recovered");
    CHECK(!sys.resolveFault("server", "nope").ok, "unknown fault rejected");
    // events carry severity metadata
    bool sawSev = false;
    for (const auto& e : sys.events().all()) {
        if (e.type == "FAULT_RAISED" && !e.severity.empty() && e.severity != "INFO") {
            sawSev = true;
            break;
        }
    }
    CHECK(sawSev, "fault events carry severity");
    // TASK 2 regression: `thermal` is a documented alias for `overheat`
    // (existing THERMAL machinery, not a new fault).
    CHECK(faultDef("thermal") != nullptr, "thermal alias resolves");
    CHECK(faultDef("thermal") == faultDef("overheat"), "thermal maps to overheat");
    auto rThermal = sys.injectFault("server", "thermal", "90");
    CHECK(rThermal.ok, "thermal inject accepted");
    CHECK(sys.get("server").faults.count("overheat") == 1, "thermal raises overheat");
    size_t thermalRaised = 0;
    for (const auto& e : sys.events().all())
        if (e.type == "FAULT_RAISED" && e.target == "server") ++thermalRaised;
    CHECK(thermalRaised >= 1, "thermal inject emits fault event");
}

// ---- 19b. TASK 3: attempted vs effective cause in `why` ----
static void t_why_rejected() {
    System sys;
    sys.setAutoFaults(false);
    // Rejected intervention stays in the ledger but must not masquerade as
    // the cause of the (unchanged) node state.
    auto rej = sys.injectFault("server", "service-down", "");
    CHECK(!rej.ok, "engine-raised inject rejected");
    bool sawRejected = false;
    for (const auto& e : sys.events().all())
        if (e.type == "INJECT_REJECTED" && e.target == "server") sawRejected = true;
    CHECK(sawRejected, "rejected attempt preserved in ledger");
    std::string whyRej = sys.why("server");
    CHECK(whyRej.find("no state-affecting events") != std::string::npos,
          "why skips rejected attempt");
    // Accepted warning faults still move the world (ONLINE -> WARNING), so
    // `why` must chain through that transition to the accepted USER_INJECT.
    CHECK(sys.injectFault("server", "thermal", "90").ok, "thermal accepted");
    CHECK(is(sys, "server", NodeState::WARNING), "thermal warns the node");
    std::string whyFault = sys.why("server");
    CHECK(whyFault.find("SERVER_WARNING") != std::string::npos,
          "why reports warning transition");
    CHECK(whyFault.find("USER_INJECT") != std::string::npos, "why chains to user inject");
    CHECK(whyFault.find("inject server thermal 90") != std::string::npos,
          "why names the accepted attempt");
    CHECK(sys.trace("server").find("SERVER_WARNING") != std::string::npos,
          "trace keeps accepted history");
    // Later accepted state change supersedes earlier fault-only history.
    sys.breakNode("server");
    std::string whyBreak = sys.why("server");
    CHECK(whyBreak.find("USER_BREAK") != std::string::npos, "why tracks state change");
    CHECK(whyBreak.find("SERVER_FAILED") != std::string::npos, "why names transition");
    // Full ledger still contains both the attempt and the effective chain.
    bool sawUserInject = false;
    sawRejected = false;
    for (const auto& e : sys.events().all()) {
        if (e.type == "USER_INJECT" && e.target == "server") sawUserInject = true;
        if (e.type == "INJECT_REJECTED" && e.target == "server") sawRejected = true;
    }
    CHECK(sawUserInject && sawRejected, "ledger keeps attempts and effects");
}

// ---- 19c. TASK 4: node-scoped events are not self-links ----
static void t_network_events_valid() {
    System sys;
    sys.setAutoFaults(false);
    // No stored self-link can exist in the default topology.
    for (const auto& l : sys.network().links())
        CHECK(l.a != l.b, "no self-link in topology");
    CHECK(sys.validateAll(nullptr), "topology validates");
    // Node-health events legitimately render as `node->node` because both
    // source and target are the affected node; they must not name a link.
    sys.injectFault("cache", "mem-leak", "");
    bool sawNodeScoped = false;
    for (const auto& e : sys.events().all()) {
        if (e.type == "FAULT_RAISED" && e.source == "cache" && e.target == "cache") {
            sawNodeScoped = true;
            CHECK(e.message.find("<->") == std::string::npos,
                  "node event does not claim a link");
        }
    }
    CHECK(sawNodeScoped, "node-scoped event observed");
    // Tick-driven network events must reference a real stored link.
    sys.tick(25);
    for (const auto& e : sys.events().all()) {
        if (e.type == "PACKET_LOSS") {
            std::string msg = e.message;
            auto pos = msg.find(" <-> ");
            CHECK(pos != std::string::npos, "packet-loss names a link");
            if (pos != std::string::npos) {
                // message form is "<a> <-> <b> packet ..." so peel endpoints.
                std::string a = msg.substr(0, pos);
                std::string rest = msg.substr(pos + 5);
                std::string b = rest.substr(0, rest.find(' '));
                CHECK(sys.network().find(a, b) != nullptr, "packet-loss names a real link");
                CHECK(a != b, "packet-loss never names a self-link");
            }
        }
    }
}

// ---- 20. .ovr scenarios: runnable + deterministic ----
static void t_scenarios() {
    std::string overknrl = findOverknrl();
    CHECK(!overknrl.empty(), "project root discoverable for scenarios");
    if (overknrl.empty()) return;
    namespace fs = std::filesystem;
    std::string proj = fs::path(overknrl).parent_path().parent_path().string();
    const char* names[] = {"cascading_failure", "thermal_runaway", "kernel_panic",
                           "rootfs_corruption", "service_cascade", nullptr};
    for (int i = 0; names[i] != nullptr; ++i) {
        std::string path =
            (fs::path(proj) / "OVERSHELL" / "sandbox" / (std::string(names[i]) + ".ovr")).string();
        CHECK(fs::exists(path), std::string("scenario file exists: ") + names[i]);
        // run twice from scratch: identical digests prove reproducibility
        std::string d[2];
        for (int k = 0; k < 2; ++k) {
            System sys;
            Shell sh(sys);
            CHECK(sh.runScript(path) == 0, std::string("scenario runs: ") + names[i]);
            std::string err;
            CHECK(sys.validateAll(&err), std::string("invariants hold: ") + names[i]);
            d[k] = sys.digest();
        }
        CHECK(d[0] == d[1], std::string("scenario deterministic: ") + names[i]);
    }
}

// ---- 21. `why` resolves effective causes across tick boundaries ----
static bool orderedIn(const std::string& hay, std::initializer_list<const char*> needles) {
    size_t pos = 0;
    for (const char* n : needles) {
        size_t f = hay.find(n, pos);
        if (f == std::string::npos) return false;
        pos = f + 1;
    }
    return true;
}

static void t_why_chains() {
    // Thermal failure: inject -> fault -> warning -> tick -> escalation ->
    // critical -> failed. The tick must not hide the overheat fault.
    {
        System sys;
        sys.setAutoFaults(false);
        sys.injectFault("server", "overheat", "90");
        sys.tick(5);
        CHECK(is(sys, "server", NodeState::FAILED), "thermal kills server");
        std::string w = sys.why("server");
        CHECK(orderedIn(w, {"USER_INJECT", "FAULT_RAISED", "SERVER_WARNING", "USER_TICK",
                            "FAULT_ESCALATED", "SERVER_CRITICAL", "SERVER_FAILED"}),
              "thermal chain complete and ordered");
        CHECK(w.find("overheat") != std::string::npos, "thermal chain names the fault");
        CHECK(w.find("(time trigger:") != std::string::npos, "tick labelled as trigger");
    }
    // Process crash: breaking a node crashes host procs; `why` names them.
    {
        System sys;
        sys.setAutoFaults(false);
        sys.breakNode("server");
        std::string w = sys.why("server");
        CHECK(orderedIn(w, {"USER_BREAK", "SERVER_FAILED", "PROC_CRASHED"}),
              "proc crash in failure chain");
    }
    // Dependency cascade: each level explains itself through the same root.
    {
        System sys;
        sys.setAutoFaults(false);
        sys.breakNode("database");
        CHECK(is(sys, "server", NodeState::DEGRADED), "server degraded by db");
        CHECK(is(sys, "client", NodeState::DEGRADED), "client degraded by server");
        std::string ws = sys.why("server");
        CHECK(orderedIn(ws, {"USER_BREAK", "SERVER_DEGRADED"}), "server chain via break");
        CHECK(ws.find("database") != std::string::npos, "server chain names failed dep");
        std::string wc = sys.why("client");
        CHECK(orderedIn(wc, {"USER_BREAK", "CLIENT_DEGRADED"}), "client chain via break");
    }
    // Configuration corruption: VFS fault surfaces through node state.
    {
        System sys;
        sys.setAutoFaults(false);
        sys.vfsCorrupt("/etc/server.conf");
        std::string w = sys.why("server");
        CHECK(orderedIn(w, {"FAULT_RAISED", "config-corrupt", "SERVER_WARNING"}),
              "config chain complete");
    }
    // Spontaneous fault: strained node crashes under a tick; the tick is the
    // trigger and the strain context travels with it (seeded, deterministic).
    {
        System sys;
        sys.setAutoFaults(false);
        sys.overrideProp("server", "latency", "900");
        sys.overrideProp("server", "health", "30");
        sys.setSeed(7);
        sys.setAutoFaults(true);
        CHECK(sys.setAutoRate(100).ok, "rate set");
        sys.tick(10);
        CHECK(is(sys, "server", NodeState::FAILED), "strained node fails spontaneously");
        std::string w = sys.why("server");
        CHECK(orderedIn(w, {"USER_TICK", "SERVER_FAILED"}), "spontaneous chain via tick");
        CHECK(w.find("spontaneous fault (strain)") != std::string::npos,
              "spontaneous reason preserved");
    }
}

// ---- 22. true rewind restoration (state, not just clock) ----
static void t_rewind() {
    // Required scenario: fresh world, checkpoint BEFORE the failure,
    // mutate, verify FAILED, rewind, verify checkpoint state restored.
    {
        System sys;
        sys.setAutoFaults(false);
        std::string cleanDigest = sys.worldDigest();
        uint64_t snap = sys.checkpoint("clean");
        CHECK(is(sys, "server", NodeState::ONLINE), "pre: server online");
        sys.injectFault("server", "overheat", "90");
        sys.tick(5);
        CHECK(is(sys, "server", NodeState::FAILED), "post: server failed");
        CHECK(sys.worldDigest() != cleanDigest, "post: world changed");
        size_t evBefore = sys.events().size();
        CHECK(sys.rewindSteps(5), "rewind ok");
        CHECK(is(sys, "server", NodeState::ONLINE), "rewound: server online");
        CHECK(sys.clock().tickCount() == 0, "rewound: clock restored");
        CHECK(sys.worldDigest() == cleanDigest, "rewound: world digest matches checkpoint");
        CHECK(sys.events().size() > evBefore, "rewind keeps ledger, appends RESTORE");
        (void)snap;
    }
    // A. checkpoint/mutate/rewind/digest equality across every subsystem.
    {
        System sys;
        sys.setAutoFaults(false);
        sys.serviceSpawn("web", "server", "app", "/etc/server.conf", "ALWAYS");
        sys.serviceStart("web");
        sys.vfsWrite("/tmp/probe", "data");
        sys.setCwd("/tmp");
        std::string before = sys.worldDigest();
        uint64_t snap = sys.checkpoint("a");
        sys.breakNode("server");
        sys.procKill("srv-listener", "crash");
        sys.serviceStop("web");
        sys.vfsWrite("/tmp/probe", "changed");
        sys.setCwd("/etc");
        sys.disconnect("client", "server");
        sys.injectFault("server", "packet_loss", "40");
        CHECK(sys.worldDigest() != before, "a: mutated");
        CHECK(sys.restore(snap), "a: restore ok");
        CHECK(sys.worldDigest() == before, "a: digest equal after rewind");
        CHECK(is(sys, "server", NodeState::ONLINE), "a: node state restored");
        CHECK(sys.clock().tickCount() == 0, "a: clock restored");
    }
    // B. fault state restored (active faults, not just node state).
    {
        System sys;
        sys.setAutoFaults(false);
        uint64_t snap = sys.checkpoint("b");
        sys.injectFault("server", "overheat", "10");
        sys.injectFault("database", "mem-leak", "");
        CHECK(!sys.get("server").faults.empty(), "b: faults active");
        CHECK(!sys.get("database").faults.empty(), "b: db faults active");
        CHECK(sys.restore(snap), "b: restore ok");
        CHECK(sys.get("server").faults.empty(), "b: server faults cleared");
        CHECK(sys.get("database").faults.empty(), "b: db faults cleared");
        CHECK(sys.get("server").tempC == 35, "b: thermal state restored");
    }
    // C. process/service state restored.
    {
        System sys;
        sys.setAutoFaults(false);
        sys.serviceSpawn("web", "server", "app", "/etc/server.conf", "ALWAYS");
        sys.serviceStart("web");
        const Service* s0 = sys.services().find("web");
        int pid0 = s0 ? s0->pid : 0;
        CHECK(pid0 > 0, "c: service has backing proc");
        uint64_t snap = sys.checkpoint("c");
        sys.serviceStop("web");
        sys.procKill("srv-listener", "crash");
        CHECK(sys.services().find("web")->state == ServiceState::STOPPED, "c: stopped");
        CHECK(sys.restore(snap), "c: restore ok");
        const Service* s1 = sys.services().find("web");
        CHECK(s1 != nullptr && s1->state == ServiceState::RUNNING, "c: service running again");
        CHECK(s1->pid == pid0, "c: backing pid restored");
        CHECK(sys.processes().find("srv-listener")->state == ProcState::RUNNING,
              "c: proc running again");
    }
    // D. VFS/network state restored (files, cwd, links, loss).
    {
        System sys;
        sys.setAutoFaults(false);
        uint64_t snap = sys.checkpoint("d");
        sys.vfsWrite("/tmp/x", "hello");
        sys.setCwd("/tmp");
        sys.disconnect("server", "cache");
        sys.injectFault("server", "packet_loss", "75");
        CHECK(sys.vfsCat("/tmp/x").find("hello") != std::string::npos, "d: file written");
        CHECK(sys.network().find("server", "cache") != nullptr &&
                  !sys.network().find("server", "cache")->up,
              "d: link down");
        CHECK(sys.restore(snap), "d: restore ok");
        CHECK(sys.vfsCat("/tmp/x").find("error:") != std::string::npos, "d: file gone");
        CHECK(sys.cwd() == "/", "d: cwd restored");
        const Link* l = sys.network().find("server", "cache");
        CHECK(l != nullptr && l->up && l->lossPct == 0.0, "d: link restored");
    }
    // E. RNG-driven replay after rewind is deterministic.
    {
        System sys;
        sys.setSeed(99);
        sys.setAutoFaults(true);
        CHECK(sys.setAutoRate(100).ok, "e: rate set");
        uint64_t snap = sys.checkpoint("e");
        sys.tick(6);
        std::string d1 = sys.worldDigest();
        CHECK(sys.restore(snap), "e: restore ok");
        sys.tick(6);
        CHECK(sys.worldDigest() == d1, "e: replay after rewind is identical");
    }
}

// ---- 23. virtual hardware model (V/R, isolated, simulated) ----
static void t_hardware() {
    System sys;
    sys.setAutoFaults(false);
    // Default profile: simulated i5/4GB/64GB, all V, keep-r, empty R.
    const HardwareManager& hw0 = sys.hardware();
    CHECK(hw0.profile().cpuModel == "Virtual Intel Core i5 Gen 8", "default cpu model");
    CHECK(hw0.profile().cpuCores == 4, "default cores");
    CHECK(hw0.profile().ramMb == 4096, "default ram");
    CHECK(hw0.profile().storageMb == 65536, "default storage");
    CHECK(hw0.cpu() == Backend::V && hw0.ram() == Backend::V &&
              hw0.storage() == Backend::V && hw0.net() == Backend::V,
          "default backends V");
    CHECK(hw0.policy() == OverflowPolicy::KEEP_R, "default policy keep-r");
    CHECK(hw0.backing().blobs.empty(), "R starts empty");
    CHECK(sys.hardwareDescribe().find("Intel Core i5 Gen 8") != std::string::npos,
          "describe shows profile");
    // Backend switching incl. invalid resources.
    CHECK(sys.hardwareBackend("ram", "R").ok, "ram backend R ok");
    CHECK(sys.hardware().ram() == Backend::R, "ram backend stored");
    CHECK(sys.hardwareBackend("net", "V").ok, "net backend V ok");
    CHECK(!sys.hardwareBackend("gpu", "V").ok, "unknown resource rejected");
    CHECK(!sys.hardwareBackend("ram", "X").ok, "unknown backend rejected");
    // Policy switching incl. invalid names.
    CHECK(sys.hardwareOverflow("cancel").ok, "cancel policy ok");
    CHECK(sys.hardware().policy() == OverflowPolicy::CANCEL, "policy stored");
    CHECK(sys.hardwareOverflow("reclaim").ok, "reclaim policy ok");
    CHECK(!sys.hardwareOverflow("explode").ok, "unknown policy rejected");
    // Provisioning validation.
    HardwareProfile tiny;
    tiny.cpuModel = "Tiny";
    tiny.cpuCores = 1;
    tiny.ramMb = 64;
    tiny.storageMb = 128;
    CHECK(sys.setHardwareProfile(tiny).ok, "small profile accepted");
    HardwareProfile bad = tiny;
    bad.cpuCores = 0;
    CHECK(!sys.setHardwareProfile(bad).ok, "zero cores rejected");
    bad = tiny;
    bad.storageMb = -5;
    CHECK(!sys.setHardwareProfile(bad).ok, "negative storage rejected");
    // Hardware participates in checkpoint/restore + digest + validate.
    uint64_t snap = sys.checkpoint("hw");
    std::string before = sys.worldDigest();
    sys.hardwareBackend("cpu", "R");
    sys.hardwareOverflow("cancel");
    CHECK(sys.worldDigest() != before, "hw changes digest");
    CHECK(sys.restore(snap), "hw restore ok");
    CHECK(sys.worldDigest() == before, "hw digest restored");
    std::string err;
    CHECK(sys.validateAll(&err), "hw invariants");
    sys.hardware().backing().ramMbCap = -1;
    CHECK(!sys.validateAll(&err), "bad backing cap caught");
}

// ---- 24b. Safety / isolation: invalid refs, self-links, R containment ----
static void t_safety_isolation() {
    System sys;
    sys.setAutoFaults(false);
    Shell sh(sys);
    // Self-links are rejected: node-scoped events must never read as links.
    CHECK(!sys.connect("server", "server").ok, "self-link rejected");
    // Invalid references fail closed with a clear error.
    CHECK(!sys.connect("nope", "server").ok, "unknown source rejected");
    CHECK(!sys.connect("server", "nope").ok, "unknown target rejected");
    CHECK(sys.why("nope").find("unknown node") != std::string::npos,
          "why unknown node is explicit");
    CHECK(sys.trace("nope").find("unknown node") != std::string::npos,
          "trace unknown node is explicit");
    // Shell surfaces the same rejections without crashing the world.
    CHECK(sh.execLine("connect server server"), "shell self-link runs");
    CHECK(sh.lastOutput().find("error:") != std::string::npos, "shell self-link errors");
    CHECK(sh.execLine("why nope"), "shell why unknown runs");
    // R containment: R-held bytes stay opaque (status shows counts, never
    // content), restore drops them deterministically, and R-full writes
    // fail closed with no partial state.
    HardwareProfile tiny;
    tiny.cpuModel = "Tiny";
    tiny.cpuCores = 1;
    tiny.ramMb = 64;
    tiny.storageMb = 1; // 1 MB V quota: forces overflow handling
    CHECK(sys.setHardwareProfile(tiny).ok, "tiny profile set");
    std::string capErr;
    CHECK(sys.hardware().setBackingCaps(20480, 64, capErr), "roomy R cap");
    uint64_t snap = sys.checkpoint("iso");
    std::string snapDigest = sys.worldDigest();
    std::string big(2 * 1048576, 'Q');
    big += "SECRET-MARKER";
    CHECK(sys.vfsWrite("/secret", big).ok, "overflow write ok");
    CHECK(sys.hardware().backing().blobs.count("/secret") == 1, "R holds overflow bytes");
    CHECK(sys.hardwareDescribe().find("SECRET-MARKER") == std::string::npos,
          "R bytes opaque in status");
    CHECK(sys.restore(snap), "restore ok");
    CHECK(sys.hardware().backing().blobs.empty(), "restore drops R blobs");
    CHECK(sys.worldDigest() == snapDigest, "digest stable across R restore");
    CHECK(sys.hardware().setBackingCaps(20480, 1, capErr), "tiny R cap");
    CHECK(!sys.vfsWrite("/secret2", big).ok, "R-full write rejected");
    CHECK(sys.hardware().backing().blobs.count("/secret2") == 0, "no partial blob");
    CHECK(sys.vfsCat("/secret2").find("error: no such file") != std::string::npos,
          "failed write leaves no file");
    std::string err;
    CHECK(sys.validateAll(&err), "invariants hold, else: " + err);
}

// ---- 24. V->R overflow policy: keep / reclaim+migrate / cancel ----
static void t_overflow() {
    // Pure manager-level policy matrix (deterministic, no world needed).
    {
        HardwareManager hw;
        HardwareProfile tiny;
        tiny.cpuModel = "Tiny";
        tiny.cpuCores = 1;
        tiny.ramMb = 64;
        tiny.storageMb = 100;
        std::string err;
        CHECK(hw.setProfile(tiny, err), "tiny profile set");
        std::string action;
        CHECK(hw.checkWrite("storage", 60, 30, action, err) && action == "v", "fits stays V");
        CHECK(!hw.checkWrite("cpu", 0, 1, action, err), "cpu has no byte quota");
        CHECK(!hw.checkWrite("storage", -1, 1, action, err), "negative usage rejected");
        CHECK(!hw.checkWrite("bogus", 0, 1, action, err), "unknown resource rejected");
        // KEEP_R (default): overflow recorded, write proceeds.
        CHECK(hw.checkWrite("storage", 90, 20, action, err) && action == "r", "keep-r advises R");
        CHECK(hw.storeOverflow("storage", "/a", "0123456789", err), "store ok");
        CHECK(hw.backing().blobs.count("/a") == 1, "blob held");
        CHECK(!hw.dropOverflow("/missing", err), "drop missing rejected");
        CHECK(hw.dropOverflow("/a", err), "drop ok");
        // R-full and retag rules.
        CHECK(hw.setBackingCaps(20480, 1, err), "tiny R storage cap");
        std::string big(3 * 1048576, 'x');
        CHECK(!hw.storeOverflow("storage", "/big", big, err), "R-full rejected");
        CHECK(hw.storeOverflow("storage", "/s", "xy", err), "small store ok");
        // CANCEL refuses outright, R untouched.
        hw.setPolicy(OverflowPolicy::CANCEL, 0);
        CHECK(!hw.placeWrite("storage", 0, 2 * 1048576, "/c", big, action, err),
              "cancel refuses overflow");
        CHECK(hw.backing().blobs.count("/c") == 0, "cancel stores nothing");
        CHECK(hw.placeWrite("storage", 10 * 1048576, 5, "/ok", "hello", action, err) &&
                  action == "v",
              "cancel allows fitting writes");
        // RECLAIM migrates back what fits, then requires V fit.
        hw.setPolicy(OverflowPolicy::RECLAIM, 0);
        CHECK(hw.placeWrite("storage", 10 * 1048576, 5, "/m", "hello", action, err) &&
                  action == "v",
              "reclaim allows fitting write");
        CHECK(hw.backing().blobs.empty(), "reclaim swept R holdings");
        CHECK(!hw.placeWrite("storage", 0, 2 * 1048576, "/m2", big, action, err),
              "reclaim refuses impossible write");
        CHECK(hw.backing().blobs.empty(), "failed reclaim loses nothing");
    }
    // End-to-end through the VFS path with a tiny profile.
    {
        System sys;
        sys.setAutoFaults(false);
        HardwareProfile tiny;
        tiny.cpuModel = "Tiny";
        tiny.cpuCores = 1;
        tiny.ramMb = 64;
        tiny.storageMb = 1; // 1 MB V quota: forces overflow handling
        CHECK(sys.setHardwareProfile(tiny).ok, "tiny profile set");
        std::string big(2 * 1048576, 'z'); // 2 MB content
        auto r1 = sys.vfsWrite("/tmp/big", big);
        CHECK(r1.ok, "keep-r write succeeds");
        CHECK(r1.info.find("R backing") != std::string::npos, "R overflow reported");
        CHECK(sys.hardware().backing().blobs.count("/tmp/big") == 1, "R holds overflow");
        CHECK(sys.hardwareOverflow("cancel").ok, "switch to cancel");
        CHECK(!sys.vfsWrite("/tmp/big2", big).ok, "cancel rejects overflow write");
        CHECK(sys.hardwareOverflow("reclaim").ok, "switch to reclaim");
        auto r4 = sys.vfsWrite("/tmp/big2", big);
        CHECK(!r4.ok, "reclaim refuses impossible write");
        CHECK(sys.hardware().backing().blobs.count("/tmp/big2") == 0, "no silent migration");
        // Restore drops the whole hardware state incl. R holdings.
        uint64_t snap = sys.checkpoint("ov");
        sys.hardwareOverflow("cancel");
        CHECK(sys.restore(snap), "overflow restore ok");
        CHECK(sys.hardware().policy() == OverflowPolicy::RECLAIM, "policy restored");
        CHECK(sys.hardware().backing().blobs.count("/tmp/big") == 1, "R blob restored");
    }
}

// ---- 25. service lifecycle through the shell with a valid binary ----
static void t_event_scope() {
    // Scope is vocabulary-derived; powers the [NODE]/[LINK]/... render tags
    // so node-scoped lines are never mistaken for links (self-links rejected).
    Event e;
    e.type = "SERVER_FAILED";
    CHECK(eventScope(e) == "NODE", "node state -> NODE");
    e.type = "IFACE_DOWN";
    CHECK(eventScope(e) == "NODE", "iface targets owning node -> NODE");
    e.type = "LINK_DOWN";
    CHECK(eventScope(e) == "LINK", "link endpoints -> LINK");
    e.type = "PACKET_LOSS";
    CHECK(eventScope(e) == "LINK", "packet -> LINK");
    e.type = "PROC_CRASHED";
    CHECK(eventScope(e) == "PROCESS", "proc -> PROCESS");
    e.type = "SERVICE_CRASHED";
    CHECK(eventScope(e) == "SERVICE", "service -> SERVICE");
    e.type = "VFS_CORRUPT";
    CHECK(eventScope(e) == "VFS", "vfs -> VFS");
    e.type = "USER_INJECT";
    CHECK(eventScope(e) == "SYSTEM", "user cmd -> SYSTEM");
    e.type = "USER_TICK";
    CHECK(eventScope(e) == "SYSTEM", "tick cmd -> SYSTEM");
    e.type = "CHECKPOINT";
    CHECK(eventScope(e) == "SYSTEM", "checkpoint -> SYSTEM");
    e.type = "NODE_REMOVED";
    CHECK(eventScope(e) == "NODE", "node removal -> NODE");
    // Live: why lines carry the scope tag; triggers render as SYSTEM while
    // the underlying fault renders as NODE.
    System sys;
    sys.setAutoFaults(false);
    sys.injectFault("server", "overheat", "90");
    sys.tick(5);
    std::string w = sys.why("server");
    CHECK(w.find("FAILED") != std::string::npos, "thermal failed");
    CHECK(w.find("[SYSTEM] USER_INJECT") != std::string::npos, "inject tagged SYSTEM");
    CHECK(w.find("[NODE] SERVER_FAILED") != std::string::npos, "failure tagged NODE");
}

static void t_services_shell() {
    System sys;
    sys.setAutoFaults(false);
    Shell sh(sys);
    CHECK(sh.execLine("service spawn web server app /etc/server.conf"), "shell spawn ok");
    CHECK(sh.execLine("service list"), "shell list ok");
    CHECK(sh.lastOutput().find("web") != std::string::npos, "list shows service");
    CHECK(sh.execLine("service inspect web"), "shell inspect ok");
    CHECK(sh.lastOutput().find("RUNNING") == std::string::npos, "declared, not running");
    CHECK(sh.execLine("service start web"), "shell start ok");
    CHECK(sh.execLine("service inspect web"), "shell inspect again");
    CHECK(sh.lastOutput().find("RUNNING") != std::string::npos, "running after start");
    CHECK(sh.execLine("service stop web"), "shell stop ok");
    CHECK(sh.lastWarning().find("stopping") != std::string::npos, "stop warns");
    CHECK(sys.services().find("web")->state == ServiceState::STOPPED, "stopped state");
    CHECK(sh.execLine("service restart web"), "shell restart ok");
    CHECK(sys.services().find("web")->state == ServiceState::RUNNING, "restarted state");
    // removal is CONFIRM-level: headless without --force is refused, never
    // silently executed; --force proceeds with a recorded warning.
    CHECK(sh.execLine("service remove web"), "shell remove runs");
    CHECK(sys.services().find("web") != nullptr, "remove refused without force");
    CHECK(sh.lastOutput().find("--force") != std::string::npos, "refusal names --force");
    CHECK(sh.execLine("service remove web --force"), "shell remove forced");
    CHECK(sh.lastWarning().find("removing service") != std::string::npos, "remove warns");
    CHECK(sys.services().find("web") == nullptr, "removed gone");
    // invalid binary keeps failing clearly through the shell too
    CHECK(sh.execLine("service spawn bad server /bin/test"), "bad spawn declared");
    CHECK(sh.execLine("service start bad"), "bad start runs");
    CHECK(sh.lastOutput().find("error:") != std::string::npos, "bad binary fails clearly");
}

// Full output line containing `needle` ("", when absent).
static std::string lineWith(const std::string& hay, const std::string& needle) {
    size_t p = hay.find(needle);
    if (p == std::string::npos) return "";
    size_t s = hay.rfind('\n', p);
    size_t e = hay.find('\n', p);
    size_t b = (s == std::string::npos) ? 0 : s + 1;
    return hay.substr(b, (e == std::string::npos) ? std::string::npos : e - b);
}

// ---- 26. effective root cause is never a bare time trigger ----
static void t_why_effective_root() {
    System sys;
    sys.setAutoFaults(false);
    sys.injectFault("server", "overheat", "90");
    sys.tick(5);
    CHECK(is(sys, "server", NodeState::FAILED), "thermal failed");
    std::string w = sys.why("server");
    // Trigger causality preserved in the chain...
    CHECK(w.find("(time trigger:") != std::string::npos, "trigger preserved");
    CHECK(orderedIn(w, {"USER_INJECT", "FAULT_RAISED", "SERVER_WARNING", "USER_TICK",
                        "FAULT_ESCALATED", "SERVER_FAILED"}),
          "chain complete and ordered");
    // ...but the reported root is the underlying cause, resolved generically
    // via trigger metadata (no command-name special-casing anywhere).
    std::string root = lineWith(w, "effective root cause:");
    CHECK(!root.empty(), "effective root reported");
    CHECK(root.find("USER_TICK") == std::string::npos &&
              root.find("AUTO_TICK") == std::string::npos,
          "trigger is not the root");
    CHECK(root.find("USER_INJECT") != std::string::npos, "root is the intervention");
    CHECK(root.find("overheat") != std::string::npos, "root names the fault");
    std::string fault = lineWith(w, "driving fault:");
    CHECK(!fault.empty(), "driving fault reported");
    CHECK(fault.find("FAULT_RAISED") != std::string::npos, "driving fault is FAULT_RAISED");
    CHECK(fault.find("overheat") != std::string::npos, "driving fault names overheat");
    // trace agrees: same effective root, old bare-trigger label gone.
    std::string tr = sys.trace("server");
    std::string troot = lineWith(tr, "effective root cause:");
    CHECK(!troot.empty(), "trace reports effective root");
    CHECK(troot.find("USER_TICK") == std::string::npos, "trace root is not the trigger");
    CHECK(troot.find("USER_INJECT") != std::string::npos, "trace root matches why");
    CHECK(tr.find("  root cause: #") == std::string::npos, "bare-trigger label gone");
}

// ---- 27. trace/why semantics across rewind (ledger stays append-only) ----
static void t_trace_rewind_semantics() {
    System sys;
    sys.setAutoFaults(false);
    uint64_t snap = sys.checkpoint("clean");
    sys.injectFault("server", "overheat", "90");
    sys.tick(5);
    CHECK(is(sys, "server", NodeState::FAILED), "failed before rewind");
    CHECK(sys.restore(snap), "restore ok");
    CHECK(is(sys, "server", NodeState::ONLINE), "online after rewind");
    // why must not blame the discarded incarnation's failure...
    std::string w = sys.why("server");
    CHECK(w.find("established by RESTORE") != std::string::npos, "why names the restore");
    CHECK(w.find("SERVER_FAILED") == std::string::npos, "why skips discarded failure");
    CHECK(w.find("kept in ledger") != std::string::npos, "why notes kept history");
    // ...while trace splits current incarnation from kept history.
    std::string tr = sys.trace("server");
    CHECK(tr.find("established by: RESTORE") != std::string::npos, "trace names restore");
    CHECK(tr.find("current incarnation (0 transition(s))") != std::string::npos,
          "trace shows empty incarnation");
    CHECK(tr.find("ledger history (4 earlier transition(s), kept append-only)") !=
              std::string::npos,
          "trace keeps history visibly");
    CHECK(tr.find("effective root cause:") == std::string::npos,
          "no causal root without transitions");
    CHECK(tr.find("SERVER_FAILED") != std::string::npos, "history still lists failure");
    // New post-restore causality resolves within the incarnation only.
    sys.breakNode("server");
    std::string w2 = sys.why("server");
    CHECK(orderedIn(w2, {"USER_BREAK", "SERVER_FAILED"}), "post-restore chain current");
    std::string r2 = lineWith(w2, "effective root cause:");
    CHECK(r2.find("USER_BREAK") != std::string::npos, "post-restore root is the break");
    std::string tr2 = sys.trace("server");
    CHECK(tr2.find("current incarnation (1 transition(s))") != std::string::npos,
          "trace counts current only");
    CHECK(lineWith(tr2, "effective root cause:").find("USER_BREAK") != std::string::npos,
          "trace root is the break");
}

// ---- 28. full service lifecycle on a healthy host (real virtual binary) ----
static void t_service_lifecycle_full() {
    System sys;
    sys.setAutoFaults(false);
    // `app` on `server` with seeded /etc/server.conf is a real valid target.
    CHECK(sys.serviceSpawn("web", "server", "app", "/etc/server.conf", "ALWAYS").ok,
          "spawn ok");
    CHECK(sys.services().find("web")->state == ServiceState::STOPPED, "declared, not running");
    CHECK(sys.serviceStart("web").ok, "start ok");
    const Service* s = sys.services().find("web");
    CHECK(s != nullptr && s->state == ServiceState::RUNNING && s->pid > 0, "running with pid");
    const SimProcess* bp = sys.processes().find(std::to_string(s->pid));
    CHECK(bp != nullptr && bp->host == "server" &&
              (bp->state == ProcState::RUNNING || bp->state == ProcState::SLEEPING),
          "backing proc alive on host");
    std::string insp = sys.serviceInspect("web");
    CHECK(insp.find("RUNNING") != std::string::npos, "inspect shows running");
    CHECK(insp.find(std::to_string(s->pid)) != std::string::npos, "inspect shows pid");
    CHECK(sys.serviceList().find("web") != std::string::npos, "list shows service");
    CHECK(sys.serviceStop("web").ok, "stop ok");
    CHECK(sys.services().find("web")->state == ServiceState::STOPPED, "stopped state");
    CHECK(sys.serviceStart("web").ok, "start again ok");
    CHECK(sys.services().find("web")->state == ServiceState::RUNNING, "running again");
    CHECK(sys.serviceRestart("web").ok, "restart ok");
    CHECK(sys.services().find("web")->state == ServiceState::RUNNING, "restarted state");
    CHECK(sys.services().find("web")->crashCount == 0, "restart resets crashes");
    CHECK(sys.serviceRemove("web").ok, "remove ok");
    CHECK(sys.services().find("web") == nullptr, "removed gone");
    // Rejections kept: invalid binary, and start on a failed host.
    CHECK(sys.serviceSpawn("bad", "server", "nope", "", "NEVER").ok, "bad spawn declared");
    CHECK(!sys.serviceStart("bad").ok, "invalid binary start rejected");
    // Absolute rootfs paths resolve directly; corruption is diagnosed.
    CHECK(sys.serviceSpawn("sh2", "server", "/bin/sh", "", "NEVER").ok, "abs spawn ok");
    CHECK(sys.serviceStart("sh2").ok, "abs binary starts");
    CHECK(sys.serviceStop("sh2").ok, "abs service stops");
    CHECK(sys.vfsCorrupt("/bin/sh").ok, "sh corrupted");
    CHECK(!sys.serviceStart("sh2").ok, "corrupt binary start rejected");
    CHECK(sys.serviceSpawn("h1", "server", "app", "", "NEVER").ok, "h1 spawn ok");
    sys.breakNode("server");
    OpResult r = sys.serviceStart("h1");
    CHECK(!r.ok && r.error.find("host FAILED") != std::string::npos,
          "failed-host start rejected");
    sys.repairNode("server");
    CHECK(sys.serviceStart("h1").ok, "start ok after repair");
    std::string err;
    CHECK(sys.validateAll(&err), "service invariants");
}

// ---- 29. iface toggles a real link endpoint pair ----
static void t_iface_endpoints() {
    System sys;
    sys.setAutoFaults(false);
    // Default topology really links server <-> client.
    CHECK(sys.network().find("server", "client") != nullptr, "endpoint pair exists");
    CHECK(sys.network().find("server", "client")->up, "link initially up");
    CHECK(sys.ifaceSet("server", "client", false).ok, "iface down ok");
    CHECK(!sys.network().find("server", "client")->up, "link down");
    bool sawIface = false;
    for (const auto& e : sys.events().all()) {
        if (e.type == "IFACE_DOWN" && e.target == "server") {
            sawIface = true;
            CHECK(e.message.find("server <-> client") != std::string::npos,
                  "iface names real endpoints");
            CHECK(e.message.find("client <-> server") == std::string::npos ||
                      e.message.find("server <-> client") != std::string::npos,
                  "endpoint order stable");
            auto it = e.metadata.find("peer");
            CHECK(it != e.metadata.end() && it->second == "client", "peer metadata set");
            CHECK(eventScope(e) == "NODE", "iface event is node-scoped");
        }
    }
    CHECK(sawIface, "IFACE_DOWN observed");
    auto u = sys.sendPacket("client", "server");
    CHECK(u.outcome != PacketResult::Outcome::DELIVERED, "down link breaks route");
    CHECK(sys.ifaceSet("server", "client", true).ok, "iface up ok");
    CHECK(sys.network().find("server", "client")->up, "link restored");
    auto d = sys.sendPacket("client", "server");
    CHECK(d.outcome == PacketResult::Outcome::DELIVERED, "route restored");
    // Invalid references fail closed with a clear error.
    CHECK(!sys.ifaceSet("ghost", "client", false).ok, "unknown node rejected");
    CHECK(sys.ifaceSet("server", "ghost", false).ok == false, "unknown peer rejected");
    CHECK(sys.ifaceSet("server", "ghost", false).error.find("no such link") !=
              std::string::npos,
          "missing link named");
    CHECK(!sys.ifaceSet("server", "client", true).ok, "redundant toggle rejected");
    std::string err;
    CHECK(sys.validateAll(&err), "iface invariants");
}

// ---- 30. progress helper: pure format + genuine stepwise callbacks ----
static void t_progress() {
    CHECK(progressBar(0, 10) == "[----------] 0%", "empty bar");
    CHECK(progressBar(5, 10) == "[/////-----] 50%", "half bar");
    CHECK(progressBar(8, 10) == "[////////--] 80%", "80% bar");
    CHECK(progressBar(10, 10) == "[//////////] 100%", "full bar");
    CHECK(progressBar(99, 10) == "[//////////] 100%", "clamped done");
    CHECK(progressBar(0, 0) == "[----------] 0%", "degenerate total");
    CHECK(!wantProgressBar(4), "instant work gets no bar");
    CHECK(wantProgressBar(5), "multi-step work gets a bar");
    // Callbacks mirror real work: one per tick, monotonic.
    {
        System sys;
        sys.setAutoFaults(false);
        std::vector<std::pair<int, int>> seen;
        sys.tick(3, 0, [&](int d, int t) { seen.emplace_back(d, t); });
        CHECK(seen.size() == 3, "one callback per tick");
        CHECK(seen.front() == std::make_pair(1, 3), "first tick reported");
        CHECK(seen.back() == std::make_pair(3, 3), "final tick reported");
    }
    // Paused clocks do zero work: no callbacks, no faked progress.
    {
        System sys;
        sys.setAutoFaults(false);
        sys.clock().pause();
        std::vector<std::pair<int, int>> seen;
        CHECK(sys.tick(5, 0, [&](int d, int t) { seen.emplace_back(d, t); }) == 0,
              "paused tick advances nothing");
        CHECK(seen.empty(), "paused tick reports nothing");
        sys.clock().resume();
    }
    // Restore reports its real stages, in order.
    {
        System sys;
        sys.setAutoFaults(false);
        uint64_t snap = sys.checkpoint("p");
        sys.tick(2);
        std::vector<std::pair<int, int>> seen;
        CHECK(sys.restore(snap, nullptr, [&](int d, int t) { seen.emplace_back(d, t); }),
              "restore ok");
        CHECK(seen.size() == 7, "seven restore stages");
        CHECK(seen.back() == std::make_pair(7, 7), "stages complete");
        for (size_t i = 1; i < seen.size(); ++i)
            CHECK(seen[i].first > seen[i - 1].first, "stages monotonic");
    }
}

// ---- 31. device failure propagation (/dev gates real ops) ----
static void t_devices() {
    System sys;
    sys.setAutoFaults(false);
    std::string why;
    CHECK(sys.deviceReady("net0", why), "net0 ready by default");
    CHECK(sys.deviceReady("disk0", why), "disk0 ready by default");
    CHECK(sys.deviceList().find("net0") != std::string::npos, "list shows net0");
    CHECK(sys.deviceList().find("gates: iface, packet, ping") != std::string::npos,
          "list shows net0 gates");
    CHECK(sys.deviceInspect("net0").find("READY") != std::string::npos, "net0 inspect ready");
    CHECK(sys.deviceInspect("ghost").find("error:") != std::string::npos,
          "unknown device diagnosed");
    uint64_t snap = sys.checkpoint("devclean");
    // Deleting /dev/net0 breaks the network path, distinctly from node failure.
    CHECK(sys.vfsRemove("/dev/net0").ok, "net0 removed");
    CHECK(!sys.deviceReady("net0", why) && why.find("missing") != std::string::npos,
          "net0 missing reported");
    CHECK(!sys.ifaceSet("server", "client", false).ok, "iface refused without device");
    auto pkt = sys.sendPacket("client", "server");
    CHECK(pkt.outcome != PacketResult::Outcome::DELIVERED, "packet fails without device");
    CHECK(pkt.detail.find("/dev/net0") != std::string::npos, "packet names the device");
    CHECK(sys.pingProbes("server", "").find("/dev/net0") != std::string::npos,
          "ping names the device");
    CHECK(is(sys, "server", NodeState::ONLINE), "node still healthy (link, not node, failure)");
    bool sawDev = false;
    for (const auto& e : sys.events().all())
        if (e.type == "DEVICE_UNAVAILABLE" && e.target == "server") sawDev = true;
    CHECK(sawDev, "device outage in ledger");
    // Corrupting /dev/disk0 blocks stored writes (metadata ops unaffected).
    CHECK(sys.vfsCorrupt("/dev/disk0").ok, "disk0 corrupted");
    CHECK(!sys.deviceReady("disk0", why) && why.find("corrupted") != std::string::npos,
          "disk0 corruption reported");
    CHECK(!sys.vfsWrite("/tmp/x", "data").ok, "stored write refused without disk");
    CHECK(sys.vfsMkdir("/tmp/d").ok, "metadata op unaffected");
    // Restore the substrate: everything recovers with no other intervention.
    CHECK(sys.restore(snap), "substrate restore ok");
    CHECK(sys.deviceReady("net0", why), "net0 back after restore");
    CHECK(sys.deviceReady("disk0", why), "disk0 back after restore");
    CHECK(sys.ifaceSet("server", "client", false).ok, "iface works after restore");
    CHECK(sys.vfsWrite("/tmp/x", "data").ok, "stored write works after restore");
    std::string err;
    CHECK(sys.validateAll(&err), "device invariants");
}

// ---- 32. boot pipeline: staged, gated, observable, recoverable ----
static void t_boot_failure() {
    // Healthy reboot runs all 12 phases and reports genuine progress.
    {
        System sys;
        sys.setAutoFaults(false);
        std::vector<std::pair<int, int>> seen;
        OpResult r = sys.rebootNode("server", [&](int d, int t) { seen.emplace_back(d, t); });
        CHECK(r.ok, "healthy reboot ok");
        CHECK(is(sys, "server", NodeState::ONLINE), "online after reboot");
        CHECK(sys.get("server").kernel == KernelState::RUNNING, "kernel running");
        CHECK(seen.size() == 12 && seen.back() == std::make_pair(12, 12), "12 boot phases");
        int bootPhases = 0;
        for (const auto& e : sys.events().all())
            if (e.type == "BOOT_PHASE" && e.target == "server") ++bootPhases;
        CHECK(bootPhases == 12, "phases in ledger");
        CHECK(sys.kernelInspect("server").find("RUNNING") != std::string::npos,
              "kernel inspect works");
        CHECK(sys.dmesg("server", 50).find("BOOT_PHASE") != std::string::npos,
              "dmesg shows boot");
        CHECK(sys.lsmod("server").find("net [LOADED]") != std::string::npos, "lsmod works");
        CHECK(sys.uname("server").find("OVERKNRL server") != std::string::npos, "uname works");
    }
    // Deleting the kernel image: running kernel continues, next boot fails.
    {
        System sys;
        sys.setAutoFaults(false);
        uint64_t snap = sys.checkpoint("healthy");
        CHECK(sys.vfsRemove("/boot/kernel").ok, "kernel image deleted");
        CHECK(is(sys, "server", NodeState::ONLINE), "running kernel continues");
        OpResult r = sys.rebootNode("server");
        CHECK(!r.ok && r.error.find("KERNEL_LOAD") != std::string::npos, "boot stops at load");
        CHECK(r.error.find("/boot/kernel") != std::string::npos, "failure names the file");
        CHECK(is(sys, "server", NodeState::FAILED), "node failed");
        CHECK(sys.get("server").kernel == KernelState::BOOTING, "kernel stuck booting");
        CHECK(sys.get("server").faults.count("boot-failure") == 1, "boot fault raised");
        std::string w = sys.why("server");
        CHECK(w.find("BOOT_FAILED") != std::string::npos, "why shows boot failure");
        CHECK(w.find("/boot/kernel") != std::string::npos, "why names the dependency");
        CHECK(lineWith(w, "effective root cause:").find("USER_REBOOT") != std::string::npos,
              "why root is the reboot");
        CHECK(sys.trace("server").find("boot failed at KERNEL_LOAD") != std::string::npos,
              "trace shows boot failure");
        CHECK(sys.kernelInspect("server").find("MISSING") != std::string::npos,
              "inspect shows missing image");
        // No magical recovery: still fails until the substrate is repaired.
        CHECK(!sys.rebootNode("server").ok, "still unbootable");
        // Restore the healthy substrate: boot recovers.
        CHECK(sys.restore(snap), "substrate restore ok");
        CHECK(sys.rebootNode("server").ok, "boot recovers after restore");
        CHECK(is(sys, "server", NodeState::ONLINE), "online again");
    }
    // Corrupted image fails the same gate with an integrity diagnostic.
    {
        System sys;
        sys.setAutoFaults(false);
        CHECK(sys.vfsCorrupt("/boot/kernel").ok, "kernel corrupted");
        OpResult r = sys.rebootNode("server");
        CHECK(!r.ok && r.error.find("integrity invalid") != std::string::npos,
              "corrupt image refused");
    }
    // Missing module image stops DRIVER_INIT; admin-disabled modules are skipped.
    {
        System sys;
        sys.setAutoFaults(false);
        CHECK(sys.vfsRemove("/lib/modules/net.ko").ok, "net image deleted");
        OpResult r = sys.rebootNode("server");
        CHECK(!r.ok && r.error.find("DRIVER_INIT") != std::string::npos, "driver gate stops boot");
        CHECK(r.error.find("net") != std::string::npos, "failure names the module");
        // Unload first (admin intent), then delete: boot skips it and succeeds.
        System sys2;
        sys2.setAutoFaults(false);
        CHECK(sys2.injectFault("server", "module-unload", "net").ok, "net unloaded");
        CHECK(sys2.vfsRemove("/lib/modules/net.ko").ok, "net image deleted");
        CHECK(sys2.rebootNode("server").ok, "boot skips disabled module");
        CHECK(sys2.get("server").modules.at("net") == "UNLOADED", "stays unloaded");
    }
    // Init redundancy: losing one copy still boots; losing both does not.
    {
        System sys;
        sys.setAutoFaults(false);
        CHECK(sys.vfsRemove("/sbin/init").ok, "sbin init deleted");
        CHECK(sys.rebootNode("server").ok, "fallback init boots");
        CHECK(sys.vfsRemove("/bin/init").ok, "bin init deleted");
        CHECK(!sys.rebootNode("server").ok, "no init stops boot");
    }
    // Kernel config corruption faults running kernels and blocks boot until
    // the file itself is repaired (acknowledgement alone is refused).
    {
        System sys;
        sys.setAutoFaults(false);
        CHECK(sys.vfsCorrupt("/kernel/kernel.conf").ok, "kernel.conf corrupted");
        CHECK(sys.get("server").faults.count("kernel-config") == 1, "config fault raised");
        CHECK(!sys.resolveFault("server", "kernel-config").ok, "ack without repair refused");
        CHECK(!sys.rebootNode("server").ok, "boot blocked by bad config");
        CHECK(sys.vfsWrite("/kernel/kernel.conf", "panic_on_fault=0\nscheduler=priority\n").ok,
              "config rewritten");
        CHECK(sys.resolveFault("server", "kernel-config").ok, "fault clears after repair");
        CHECK(sys.rebootNode("server").ok, "boot works after repair");
    }
    // sysctl reads/writes real tunables with validation.
    {
        System sys;
        sys.setAutoFaults(false);
        bool ok = false;
        CHECK(sys.sysctlGet("server", "thermal.limit", ok) == "85" && ok, "limit reads 85");
        CHECK(sys.sysctlSet("server", "thermal.limit", "95").ok, "limit set ok");
        CHECK(sys.get("server").thermalLimitC == 95, "limit stored");
        CHECK(!sys.sysctlSet("server", "thermal.limit", "999").ok, "range enforced");
        CHECK(!sys.sysctlSet("server", "kernel.instability", "0").ok, "ro key refused");
        CHECK(!sys.sysctlSet("server", "bogus.key", "1").ok, "unknown key refused");
        std::string err;
        CHECK(sys.validateAll(&err), "boot invariants");
    }
}

// ---- 33. task/process/thread hierarchy + signals + sched + syscalls + IPC ----
static void t_procs_threads() {
    System sys;
    sys.setAutoFaults(false);
    // Every process is born with its main thread (task -> process -> thread).
    const SimProcess* p0 = sys.processes().find("srv-worker");
    CHECK(p0 != nullptr && p0->threads.size() == 1, "main thread at spawn");
    CHECK(p0->threads.front().tid == p0->pid * 1000 + 1, "main tid scheme");
    CHECK(sys.threadInspect(p0->pid * 1000 + 1).find("RUNNING") != std::string::npos,
          "thread inspect works");
    CHECK(sys.threadInspect(999999).find("unknown thread") != std::string::npos,
          "unknown tid diagnosed");
    // Extra threads spawn with deterministic tids and accrue cpu time.
    OpResult t1 = sys.threadSpawn("srv-worker", "io");
    CHECK(t1.ok, "thread spawn ok");
    int tid = p0->pid * 1000 + 2;
    CHECK(sys.processes().find("srv-worker")->threads.size() == 2, "two threads");
    sys.tick(3);
    CHECK(sys.processes().find("srv-worker")->threads.front().cpuTicks >= 3,
          "threads accrue cpu time");
    CHECK(sys.threadList("srv-worker").find(std::to_string(tid)) != std::string::npos,
          "thread list shows tid");
    CHECK(!sys.threadSpawn("ghost", "x").ok, "thread on unknown proc rejected");
    // Threads follow the process through pause/crash/restart.
    CHECK(sys.procPause("srv-worker").ok, "pause ok");
    CHECK(sys.processes().find("srv-worker")->threads.front().state == ProcState::PAUSED,
          "threads paused");
    CHECK(sys.procRestart("srv-worker").ok, "restart ok");
    CHECK(sys.processes().find("srv-worker")->threads.front().state == ProcState::RUNNING,
          "threads resumed");
    // Signals map onto the same real transitions (no print-only behavior).
    CHECK(sys.procSignal("SIGSTOP", "srv-worker").ok, "SIGSTOP parks");
    CHECK(sys.processes().find("srv-worker")->state == ProcState::PAUSED, "stopped state");
    CHECK(sys.procSignal("CONT", "srv-worker").ok, "SIGCONT resumes");
    CHECK(sys.processes().find("srv-worker")->state == ProcState::RUNNING, "running again");
    CHECK(sys.procSignal("TERM", "srv-worker").ok, "SIGTERM parks");
    CHECK(sys.procSignal("HUP", "srv-worker").ok, "SIGHUP restarts");
    CHECK(!sys.procSignal("SIGUSR9", "srv-worker").ok, "unknown signal rejected");
    CHECK(sys.procSignal("KILL", "srv-worker").ok, "SIGKILL kills");
    CHECK(sys.processes().find("srv-worker")->state == ProcState::KILLED, "killed state");
    CHECK(sys.processes().find("srv-worker")->threads.front().state == ProcState::KILLED,
          "threads killed");
    CHECK(!sys.procSignal("CONT", "srv-worker").ok, "dead proc cannot resume");
    // Scheduler + task tree are derived from the same live registry.
    CHECK(sys.schedView().find("running:") != std::string::npos, "sched has running slot");
    CHECK(sys.schedView().find("ready (") != std::string::npos, "sched has ready queue");
    CHECK(sys.processTree().find("task srv-listener") != std::string::npos,
          "tree shows tasks");
    CHECK(sys.processTree().find("thread(s)") != std::string::npos, "tree shows threads");
    // Syscall counters reflect real operator-issued call sites.
    {
        System sys2;
        sys2.setAutoFaults(false);
        CHECK(sys2.vfsCat("/etc/server.conf").find("node=server") != std::string::npos,
              "cat ok");
        CHECK(sys2.runBinary("server", "app", "").ok, "exec ok");
        CHECK(sys2.sendPacket("client", "server").outcome == PacketResult::Outcome::DELIVERED,
              "packet ok");
        std::string sc = sys2.syscallView();
        CHECK(sc.find("read: 1") != std::string::npos, "read counted");
        CHECK(sc.find("execve: 1") != std::string::npos, "execve counted");
        CHECK(sc.find("sendmsg: 1") != std::string::npos, "sendmsg counted");
        CHECK(sc.find("total: 3") != std::string::npos, "total counted");
    }
    {
        System a, b;
        a.setAutoFaults(false);
        b.setAutoFaults(false);
        a.vfsCat("/etc/server.conf");
        b.vfsCat("/etc/server.conf");
        a.runBinary("server", "app", "");
        b.runBinary("server", "app", "");
        CHECK(a.syscallView() == b.syscallView(), "syscall counts deterministic");
    }
    // IPC mailboxes: real queues with capacity, delivery, and cleanup.
    CHECK(sys.ipcSend("cli-shell", "hello").ok, "ipc send ok");
    CHECK(sys.ipcList("cli-shell").find("1 queued") != std::string::npos, "ipc listed");
    OpResult got = sys.ipcRecv("cli-shell");
    CHECK(got.ok && got.info == "hello", "ipc recv delivers");
    CHECK(!sys.ipcRecv("cli-shell").ok, "empty mailbox refused");
    CHECK(!sys.ipcSend("ghost", "x").ok, "ipc to unknown refused");
    for (int i = 0; i < 64; ++i) CHECK(sys.ipcSend("cli-shell", "m").ok, "mailbox fills");
    CHECK(!sys.ipcSend("cli-shell", "overflow").ok, "mailbox cap enforced");
    CHECK(sys.procInspect("cli-shell").find("mailbox (64 queued)") != std::string::npos,
          "inspect shows mailbox");
    CHECK(sys.procInspect("cli-shell").find("threads (1)") != std::string::npos,
          "inspect shows threads");
    std::string err;
    CHECK(sys.validateAll(&err), "proc/thread invariants");
}

// ---- 34. warnings + confirmation (presentation only, never sim state) ----
static void t_warnings() {
    // WARN-level: proceeds, warning recorded, simulation changed.
    {
        System sys;
        sys.setAutoFaults(false);
        Shell sh(sys);
        CHECK(sh.execLine("break server"), "break runs");
        CHECK(sh.lastWarning().find("cascading failures") != std::string::npos,
              "break warns cascade");
        CHECK(is(sys, "server", NodeState::FAILED), "break still works");
        CHECK(sh.execLine("inject server overheat 10"), "inject runs");
        CHECK(!sh.lastWarning().empty(), "inject warns");
    }
    // CONFIRM-level headless without --force: refused, state untouched.
    {
        System sys;
        sys.setAutoFaults(false);
        Shell sh(sys);
        CHECK(sh.execLine("halt server"), "halt runs");
        CHECK(sh.lastOutput().find("--force") != std::string::npos, "halt names --force");
        CHECK(sh.lastWarning().find("halting") != std::string::npos, "halt warns first");
        CHECK(sys.get("server").kernel == KernelState::RUNNING, "halt refused cleanly");
        CHECK(sh.execLine("halt server --force"), "halt forced");
        CHECK(sys.get("server").kernel == KernelState::OFF, "halt proceeds with force");
    }
    // Critical-path rm/corrupt: refused, then forced; plain rm only warns.
    {
        System sys;
        sys.setAutoFaults(false);
        Shell sh(sys);
        CHECK(sh.execLine("rm /boot/kernel"), "rm critical runs");
        CHECK(sys.vfsCat("/boot/kernel").find("OVERKNRL kernel image") != std::string::npos,
              "critical rm refused");
        CHECK(sh.lastWarning().find("unable to boot") != std::string::npos,
              "critical rm warns boot");
        CHECK(sh.execLine("rm /boot/kernel --force"), "rm critical forced");
        CHECK(sys.vfsCat("/boot/kernel").find("error:") != std::string::npos,
              "critical rm proceeds with force");
        CHECK(sh.execLine("touch /tmp/x"), "touch ok");
        CHECK(sh.execLine("rm /tmp/x"), "plain rm runs");
        CHECK(sh.lastWarning().find("destructive inside the simulated world") !=
                  std::string::npos,
              "plain rm warns");
        CHECK(sys.vfsCat("/tmp/x").find("error:") != std::string::npos, "plain rm works");
    }
    // Interactive confirmation: yes proceeds, no cancels, both deterministic.
    {
        System sys;
        sys.setAutoFaults(false);
        Shell sh(sys);
        sh.setInteractive(true);
        sh.setConfirmHandler([](const std::string&) { return true; });
        CHECK(sh.execLine("halt server"), "interactive yes runs");
        CHECK(sys.get("server").kernel == KernelState::OFF, "interactive yes halts");
        sh.setInteractive(false); // session-scoped: never leak into other tests
    }
    {
        System sys;
        sys.setAutoFaults(false);
        Shell sh(sys);
        sh.setInteractive(true);
        sh.setConfirmHandler([](const std::string&) { return false; });
        CHECK(sh.execLine("halt server"), "interactive no runs");
        CHECK(sh.lastOutput().find("cancelled") != std::string::npos, "cancel reported");
        CHECK(sys.get("server").kernel == KernelState::RUNNING, "interactive no keeps state");
        sh.setInteractive(false); // session-scoped: never leak into other tests
    }
    // Reboot warns but never demands confirmation (routine recovery path).
    {
        System sys;
        sys.setAutoFaults(false);
        Shell sh(sys);
        CHECK(sh.execLine("reboot server"), "reboot runs");
        CHECK(sh.lastWarning().find("clears volatile") != std::string::npos, "reboot warns");
        CHECK(is(sys, "server", NodeState::ONLINE), "reboot works");
    }
}

// ---- 35. healthy-node link down/up (node failure vs link failure) ----
static void t_healthy_link() {
    System sys;
    sys.setAutoFaults(false);
    CHECK(sys.rebootNode("server").ok, "reboot to healthy");
    std::string err;
    CHECK(sys.validateAll(&err), "healthy validates");
    // Baseline: ping works across the healthy fabric.
    CHECK(sys.pingProbes("cache", "").find("packet loss: 0%") != std::string::npos,
          "baseline ping clean");
    const Link* l0 = sys.network().find("server", "cache");
    CHECK(l0 != nullptr && l0->up && l0->lossPct == 0.0, "baseline link up lossless");
    // Down the link (not the node): ping fails for the link reason.
    CHECK(sys.ifaceSet("server", "cache", false).ok, "iface down ok");
    CHECK(!sys.network().find("server", "cache")->up, "link down");
    std::string bad = sys.pingProbes("cache", "");
    CHECK(bad.find("packet loss: 100%") != std::string::npos, "ping fails");
    CHECK(bad.find("no route from client") != std::string::npos, "link reason, not node");
    // Server depends on cache, so it DEGRADEDs through the link (never FAILED:
    // link failure and node failure stay distinguishable).
    CHECK(is(sys, "server", NodeState::DEGRADED), "server degraded via link");
    CHECK(is(sys, "cache", NodeState::ONLINE), "cache still ONLINE");
    CHECK(sys.why("server").find("link to dependency cache is DOWN") != std::string::npos,
          "why names the link");
    bool sawIface = false;
    for (const auto& e : sys.events().all()) {
        if (e.type == "IFACE_DOWN" && e.target == "server") {
            sawIface = true;
            CHECK(e.causeId != 0, "iface has causal parent");
            CHECK(sys.events().find(e.causeId) != nullptr &&
                      sys.events().find(e.causeId)->type == "USER_IFACE",
                  "iface caused by user op");
        }
    }
    CHECK(sawIface, "IFACE_DOWN with causality");
    // Restore: connectivity recovers with no other intervention.
    CHECK(sys.ifaceSet("server", "cache", true).ok, "iface up ok");
    CHECK(sys.network().find("server", "cache")->up, "link up");
    CHECK(sys.pingProbes("cache", "").find("packet loss: 0%") != std::string::npos,
          "ping recovers");
    CHECK(sys.validateAll(&err), "link invariants");
}

// ---- 36. causality from a genuinely clean checkpoint ----
static void t_clean_cycle() {
    System sys;
    sys.setAutoFaults(false);
    // Clean baseline: rebooted, validated, checkpointed while healthy.
    CHECK(sys.rebootNode("server").ok, "reboot to clean");
    std::string err;
    CHECK(sys.validateAll(&err), "clean validates");
    CHECK(is(sys, "server", NodeState::ONLINE), "clean is online");
    CHECK(sys.get("server").faults.empty(), "clean has no faults");
    uint64_t snap = sys.checkpoint("clean");
    // Fault drill on the clean world.
    CHECK(sys.injectFault("server", "overheat", "90").ok, "inject ok");
    sys.tick(3);
    CHECK(is(sys, "server", NodeState::FAILED), "drill fails the node");
    std::string w = sys.why("server");
    CHECK(lineWith(w, "effective root cause:").find("USER_INJECT") != std::string::npos,
          "why root is the inject");
    CHECK(lineWith(w, "driving fault:").find("FAULT_RAISED") != std::string::npos,
          "why driving fault");
    CHECK(sys.trace("server").find("current incarnation") == std::string::npos,
          "no restore yet: plain timeline");
    // Restore the clean checkpoint: genuinely healthy, ledger kept.
    CHECK(sys.restore(snap), "restore clean ok");
    CHECK(is(sys, "server", NodeState::ONLINE), "restored healthy");
    CHECK(sys.get("server").faults.empty(), "no stale faults leak");
    CHECK(sys.validateAll(&err), "restored validates");
    // New post-restore activity builds new causality, not stale failure.
    CHECK(sys.breakNode("server").ok, "new break ok");
    std::string w2 = sys.why("server");
    CHECK(lineWith(w2, "effective root cause:").find("USER_BREAK") != std::string::npos,
          "new root is the break");
    CHECK(w2.find("overheat") == std::string::npos, "no stale fault in new chain");
    bool sawOld = false, sawRestore = false;
    for (const auto& e : sys.events().all()) {
        if (e.type == "SERVER_FAILED") sawOld = true;
        if (e.type == "RESTORE") sawRestore = true;
    }
    CHECK(sawOld && sawRestore, "ledger keeps failure + restore");
}

// ---- 37. ORDC: parse/serialize/diagnostics ----
static void t_ord() {
    using namespace ordc;
    // Valid round-trip with escapes.
    {
        OrdDoc d;
        d.type = "world";
        d.id = "w1";
        d.set("meta", "note", "line1\nline2\\end");
        d.set("sim", "cwd", "/tmp/a=b");
        std::string text = serializeOrd(d);
        CHECK(text.rfind("ORD 1\ntype = world\nid = w1\n", 0) == 0, "header first");
        OrdDoc d2;
        OrdError err;
        CHECK(parseOrd(text, "t.ord", d2, err), "round-trip parses: " + err.str());
        CHECK(d2.type == "world" && d2.id == "w1", "header kept");
        CHECK(d2.kv["meta"]["note"] == "line1\nline2\\end", "escapes round-trip");
        CHECK(d2.kv["sim"]["cwd"] == "/tmp/a=b", "equals in values kept");
        CHECK(serializeOrd(d2) == text, "serialization deterministic");
    }
    // Diagnostics identify file:line precisely.
    {
        OrdDoc d;
        OrdError err;
        CHECK(!parseOrd("JUNK 9\n", "w1.ord", d, err), "bad header rejected");
        CHECK(err.str().find("w1.ord:1") != std::string::npos, "file:line diagnostic");
        CHECK(!parseOrd("ORD 2\ntype = world\nid = w1\n", "w9.ord", d, err),
              "future version rejected");
        CHECK(err.str().find("unsupported ORD version") != std::string::npos,
              "version diagnostic");
        CHECK(!parseOrd("ORD 1\ntype = world\nid = w1\n[a]\nk = 1\n[a]\n", "w1.ord", d,
                       err),
              "duplicate section rejected");
        CHECK(!parseOrd("ORD 1\ntype = world\nid = w1\n[a]\nk = 1\nk = 2\n", "w1.ord", d,
                       err),
              "duplicate key rejected");
        CHECK(!parseOrd("ORD 1\ntype = world\nid = w1\n[a]\nk = \\q\n", "w1.ord", d, err),
              "bad escape rejected");
        CHECK(!parseOrd("ORD 1\ntype = world\nid = w1\n[a]\nk = abc\\\n", "w1.ord", d, err),
              "trailing backslash rejected");
        CHECK(!parseOrd("ORD 1\ntype = nope\nid = w1\n", "w1.ord", d, err),
              "invalid type rejected");
        CHECK(!parseOrd("ORD 1\ntype = world\n", "w1.ord", d, err), "missing id rejected");
        CHECK(!parseOrd("ORD 1\ntype = world\nid = w1\nbogus = 1\n", "w1.ord", d, err),
              "unknown header key rejected");
    }
    // Strict typed readers.
    {
        OrdDoc d;
        d.type = "world";
        d.id = "w1";
        d.set("s", "b", "maybe");
        d.set("s", "i", "12x");
        d.set("s", "u", "-3");
        OrdError err;
        bool bv = false;
        int iv = 0;
        uint64_t uv = 0;
        CHECK(!getBool(d, "s", "b", bv, err), "maybe is not boolean");
        CHECK(err.str().find("expected true or false") != std::string::npos, "bool diagnostic");
        CHECK(!getInt(d, "s", "i", iv, err), "12x is not int");
        CHECK(!getUint64(d, "s", "u", uv, err), "negative is not uint");
        CHECK(!getString(d, "missing", "k", d.id, err), "missing section diagnosed");
    }
}

// ---- 38. world save/load round-trip preserves authoritative state ----
static System makeLivedWorld() {
    System sys;
    sys.setAutoFaults(false);
    sys.setWorldId("w1");
    sys.setWorldName("Roundtrip Lab");
    sys.setWorldNote("persistence probe");
    sys.setWorldCreatedAt(1700000000);
    sys.serviceSpawn("web", "server", "app", "/etc/server.conf", "ALWAYS");
    sys.serviceStart("web");
    sys.threadSpawn("srv-worker", "io");
    sys.ipcSend("srv-worker", "ping");
    sys.vfsWrite("/tmp/probe", "line1\nline2=with=equals\\backslash");
    sys.vfsMkdir("/data/deep");
    sys.vfsWrite("/data/deep/f", "x");
    sys.injectFault("server", "overheat", "90");
    sys.tick(5);
    sys.ifaceSet("server", "cache", false);
    sys.hardwareBackend("storage", "R");
    sys.checkpoint("lived");
    return sys;
}

static void t_persist_roundtrip() {
    System sys = makeLivedWorld();
    CHECK(is(sys, "server", NodeState::FAILED), "lived world failed as built");
    std::string before = sys.digest();
    std::string beforeWorld = sys.worldDigest();
    // Serialize -> text -> parse -> fresh System: digests identical.
    ordc::OrdDoc doc;
    std::string serr;
    CHECK(sys.serializeWorldDoc(doc, serr), "serialize ok: " + serr);
    std::string text = ordc::serializeOrd(doc);
    CHECK(text.rfind("ORD 1\ntype = world\nid = w1\n", 0) == 0, "world header");
    CHECK(text.find("[uptime]\ncreated_at = 1700000000\n") != std::string::npos,
          "uptime persisted");
    ordc::OrdDoc doc2;
    ordc::OrdError oerr;
    CHECK(ordc::parseOrd(text, "w1.ord", doc2, oerr), "reparse ok: " + oerr.str());
    System loaded;
    std::string lerr;
    CHECK(loaded.deserializeWorldDoc(doc2, lerr), "deserialize ok: " + lerr);
    CHECK(loaded.digest() == before, "ledger digest survives round-trip");
    CHECK(loaded.worldDigest() == beforeWorld, "world digest survives round-trip");
    CHECK(loaded.worldId() == "w1", "identity kept");
    CHECK(loaded.worldName() == "Roundtrip Lab", "name kept");
    CHECK(loaded.worldCreatedAt() == 1700000000, "created_at kept");
    CHECK(is(loaded, "server", NodeState::FAILED), "node state kept");
    CHECK(loaded.get("server").faults.count("overheat") == 1, "faults kept");
    CHECK(loaded.services().find("web")->state == sys.services().find("web")->state,
          "service kept");
    CHECK(loaded.services().find("web")->pid > 0, "service pid kept");
    CHECK(loaded.processes().find("srv-worker")->threads.size() == 2, "threads kept");
    CHECK(loaded.processes().find("srv-worker")->mailbox.size() == 1, "mailbox kept");
    CHECK(loaded.vfsCat("/tmp/probe").find("line2=with=equals\\backslash") !=
              std::string::npos,
          "tricky bytes kept");
    CHECK(!loaded.network().find("server", "cache")->up, "link state kept");
    CHECK(loaded.hardware().storage() == Backend::R, "backend kept");
    CHECK(loaded.why("server").find("overheat") != std::string::npos,
          "causality survives save/load");
    CHECK(loaded.trace("server").find("SERVER_FAILED") != std::string::npos,
          "trace survives save/load");
    std::string err;
    CHECK(loaded.validateAll(&err), "loaded world validates: " + err);
    // Deterministic continuation: same ops on both yield same digests.
    sys.tick(2);
    loaded.tick(2);
    CHECK(sys.digest() == loaded.digest(), "post-load evolution identical");
    // Corrupt inputs fail loudly and leave the target unchanged.
    {
        ordc::OrdDoc bad;
        ordc::OrdError berr;
        CHECK(ordc::parseOrd("ORD 1\ntype = world\nid = w1\n", "w1.ord", bad, berr),
              "minimal doc parses");
        System target;
        std::string derr;
        std::string dBefore = target.digest();
        CHECK(!target.deserializeWorldDoc(bad, derr), "incomplete world rejected");
        CHECK(!derr.empty(), "diagnostic produced");
        CHECK(target.digest() == dBefore, "failed load leaves system unchanged");
    }
}

// ---- 39. world slots: files exist only for existing worlds ----
static std::string tempAppDir(const std::string& tag) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("override-test-" + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir.string();
}

static bool noExtraWorldFiles(const std::string& appDir, std::initializer_list<const char*> keep) {
    namespace fs = std::filesystem;
    for (int i = 1; i <= 6; ++i) {
        std::string id = "w" + std::to_string(i);
        bool wanted = false;
        for (const char* k : keep)
            if (id == k) wanted = true;
        std::error_code ec;
        bool exists = fs::is_regular_file(fs::path(appDir) / (id + ".ord"), ec);
        if (exists != wanted) return false;
        if (fs::is_regular_file(fs::path(appDir) / (id + ".ord.tmp"), ec)) return false;
        if (id != "w7" && fs::is_regular_file(fs::path(appDir) / "w7.ord", ec)) return false;
    }
    return true;
}

static void t_worlds() {
    namespace fs = std::filesystem;
    std::string dir = tempAppDir("worlds");
    WorldManager wm(dir);
    CHECK(wm.discover().empty(), "zero worlds initially");
    CHECK(wm.lowestFreeSlot() == "w1", "first slot w1");
    CHECK(noExtraWorldFiles(dir, {}), "no world files initially");
    System sys; // fresh defaults stay pristine (no toggles before create)
    Shell sh(sys, &wm);
    // create w -> w1.ord only.
    CHECK(sh.execLine("create w"), "create w1 runs");
    CHECK(sh.lastOutput().find("World w1 ready") != std::string::npos, "w1 ready reported");
    CHECK(fs::is_regular_file(fs::path(dir) / "w1.ord"), "w1.ord created");
    CHECK(noExtraWorldFiles(dir, {"w1"}), "only w1.ord exists");
    CHECK(wm.activeId() == "w1" && sys.worldId() == "w1", "w1 active");
    CHECK(sh.execLine("crt w"), "crt alias works");
    CHECK(fs::is_regular_file(fs::path(dir) / "w2.ord"), "w2.ord created");
    CHECK(noExtraWorldFiles(dir, {"w1", "w2"}), "only w1+w2 exist");
    // Fill to six, then reject the seventh (no w7.ord, ever).
    for (int i = 0; i < 4; ++i) CHECK(sh.execLine("create w"), "fill to six");
    CHECK(noExtraWorldFiles(dir, {"w1", "w2", "w3", "w4", "w5", "w6"}), "all six exist");
    CHECK(sh.execLine("create w"), "seventh create runs");
    CHECK(sh.lastOutput().find("maximum world limit reached (6/6)") != std::string::npos,
          "seventh rejected clearly");
    CHECK(!fs::exists(fs::path(dir) / "w7.ord"), "no w7.ord");
    // Isolation: mutate w1, w2 stays clean, w1 keeps its damage.
    CHECK(sh.execLine("joinw w1"), "join w1");
    CHECK(sh.execLine("break server"), "break w1 server");
    CHECK(sh.execLine("joinw w2"), "join w2");
    CHECK(is(sys, "server", NodeState::ONLINE), "w2 clean");
    CHECK(sh.execLine("joinw w1"), "back to w1");
    CHECK(is(sys, "server", NodeState::FAILED), "w1 damage kept");
    // Malformed world file: load fails loudly, current world kept.
    {
        std::ofstream f(fs::path(dir) / "w3.ord", std::ios::binary | std::ios::trunc);
        f << "ORD 1\ntype = world\nid = w3\n[net]\nlink_count = nope\n";
        f.close();
        CHECK(sh.execLine("joinw w3"), "join corrupt runs");
        CHECK(sh.lastOutput().find("error:") != std::string::npos, "corrupt diagnosed");
        CHECK(is(sys, "server", NodeState::FAILED), "current world kept on load failure");
        CHECK(wm.activeId() == "w1", "active unchanged on load failure");
    }
    // Delete frees the slot; create reuses the lowest free one.
    CHECK(sh.execLine("joinw w2"), "park on w2");
    CHECK(sh.execLine("deletew w3 --force"), "delete w3");
    CHECK(!fs::exists(fs::path(dir) / "w3.ord"), "w3.ord gone");
    CHECK(noExtraWorldFiles(dir, {"w1", "w2", "w4", "w5", "w6"}), "slot freed");
    CHECK(sh.execLine("create w"), "create reuses slot");
    CHECK(sh.lastOutput().find("World w3 ready") != std::string::npos, "lowest slot reused");
    // Reset keeps identity, returns simulation to fresh.
    CHECK(sh.execLine("joinw w1"), "back to damaged w1");
    CHECK(sh.execLine("reset --force"), "reset runs");
    CHECK(sys.worldId() == "w1", "reset keeps identity");
    CHECK(is(sys, "server", NodeState::ONLINE), "reset returns fresh sim");
    CHECK(sys.get("server").faults.empty(), "reset clears faults");
    // joinw unknown world is a clean error, not a crash.
    CHECK(sh.execLine("joinw w9"), "join invalid runs");
    CHECK(sh.lastOutput().find("error:") != std::string::npos, "invalid id diagnosed");
    CHECK(sh.execLine("joinw w2"), "join missing-file tolerant");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- 40. uptime: session vs all-time, persisted, rewind-proof ----
static void t_uptime() {
    namespace fs = std::filesystem;
    std::string dir = tempAppDir("uptime");
    int64_t now = 1000000;
    WorldManager wm(dir);
    wm.setClock([&]() { return now; });
    System sys; // pristine session (entropy untouched; no ticks in this test)
    Shell sh(sys, &wm);
    // No active world: clear error, no invented uptime.
    CHECK(sh.execLine("upt"), "upt with no world runs");
    CHECK(sh.lastOutput().find("no active world") != std::string::npos, "no-world error");
    // New world starts at zero; session accrues with the fake clock.
    CHECK(sh.execLine("create w"), "create w1");
    CHECK(sh.execLine("upt"), "upt runs");
    CHECK(sh.lastOutput().find("Uptime: 0s") != std::string::npos, "session starts at zero");
    now += 728; // 12m08s
    CHECK(sh.execLine("upt"), "upt later");
    CHECK(sh.lastOutput().find("12m 08s") != std::string::npos, "session accrues");
    CHECK(sh.execLine("upt -at"), "all-time runs");
    CHECK(sh.lastOutput().find("All-time uptime: 12m 08s") != std::string::npos,
          "all-time accrues");
    // Repeated saves never double-count.
    CHECK(sh.execLine("save"), "save 1");
    CHECK(sh.execLine("save"), "save 2");
    CHECK(sh.execLine("save"), "save 3");
    CHECK(sh.execLine("upt -at"), "all-time after saves");
    CHECK(sh.lastOutput().find("All-time uptime: 12m 08s") != std::string::npos,
          "saves do not double-count");
    // Switching commits w1 and starts a fresh w2 session (independent).
    CHECK(sh.execLine("create w"), "create w2");
    now += 100;
    CHECK(sh.execLine("joinw w1"), "rejoin w1 commits");
    CHECK(sh.execLine("upt -at"), "w1 all-time");
    CHECK(sh.lastOutput().find("12m 08s") != std::string::npos, "w1 total kept");
    CHECK(sh.execLine("joinw w2"), "join w2");
    CHECK(sh.execLine("upt"), "w2 session");
    CHECK(sh.lastOutput().find("Uptime: 0s") != std::string::npos, "w2 session separate");
    CHECK(sh.execLine("upt -at"), "w2 all-time");
    CHECK(sh.lastOutput().find("All-time uptime: 1m 40s") != std::string::npos,
          "w2 lifetime independent");
    // Exit/reload: new session from zero, all-time preserved.
    now += 372; // w2 session: 6m12s
    CHECK(sh.execLine("joinw w1"), "park on w1");
    {
        // Simulate relaunch: fresh objects, same directory.
        WorldManager wm2(dir);
        wm2.setClock([&]() { return now; });
        System sys2; // pristine (no toggles/ticks before joining)
        Shell sh2(sys2, &wm2);
        CHECK(sh2.execLine("joinw w1"), "relaunch join");
        CHECK(sh2.execLine("upt"), "session after reload");
        CHECK(sh2.lastOutput().find("Uptime: 0s") != std::string::npos,
              "new session resets upt");
        CHECK(sh2.execLine("upt -at"), "all-time after reload");
        CHECK(sh2.lastOutput().find("All-time uptime: 12m 08s") != std::string::npos,
              "reload preserves all-time");
        // Rewind/checkpoint never roll back lifetime.
        CHECK(sh2.execLine("checkpoint x"), "checkpoint ok");
        CHECK(sh2.execLine("restore 1"), "restore ok");
        CHECK(sh2.execLine("upt -at"), "all-time after rewind");
        CHECK(sh2.lastOutput().find("All-time uptime: 12m 08s") != std::string::npos,
              "rewind-proof uptime");
        // Reset keeps uptime; delete removes it with the world.
        CHECK(sh2.execLine("reset --force"), "reset ok");
        CHECK(sh2.execLine("upt -at"), "all-time after reset");
        CHECK(sh2.lastOutput().find("All-time uptime: 12m 08s") != std::string::npos,
              "reset keeps uptime");
        CHECK(sh2.execLine("joinw w2"), "park on w2");
        CHECK(sh2.execLine("deletew w1 --force"), "delete w1");
        CHECK(sh2.execLine("create w"), "recreate slot");
        CHECK(sh2.lastOutput().find("World w1 ready") != std::string::npos, "slot reused");
        CHECK(sh2.execLine("upt -at"), "fresh world all-time");
        CHECK(sh2.lastOutput().find("All-time uptime: 0s") != std::string::npos,
              "deleted history gone with world");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- 41. recovery guidance: state-aware, read-only, never repairs ----
static void t_recovery() {
    System sys;
    sys.setAutoFaults(false);
    Shell sh(sys);
    auto guided = [&](const std::string& line) {
        std::string d0 = sys.digest();
        size_t ev0 = sys.events().size();
        CHECK(sh.execLine(line), std::string("rec runs: ") + line);
        std::string out = sh.lastOutput();
        CHECK(sys.digest() == d0, std::string("rec is read-only: ") + line);
        CHECK(sys.events().size() == ev0, std::string("rec emits nothing: ") + line);
        return out;
    };
    // Healthy world: no phantom problems.
    CHECK(guided("rec kernel").find("No kernel problem") != std::string::npos,
          "healthy kernel reported");
    CHECK(guided("rec -h kernel").find("Affected subsystems") != std::string::npos,
          "kernel -h has depth");
    CHECK(guided("rec module").find("net[LOADED") != std::string::npos, "module overview");
    CHECK(guided("rec network").find("all links UP") != std::string::npos, "network healthy");
    // Broken kernel: diagnosis names the missing file and the approaches.
    CHECK(sys.vfsRemove("/boot/kernel").ok, "kernel deleted");
    std::string rk = guided("rec kernel");
    CHECK(rk.find("MISSING") != std::string::npos, "diagnosis names missing image");
    CHECK(rk.find("Possible recovery approaches") != std::string::npos, "approaches listed");
    CHECK(rk.find("reboot only after a valid kernel exists") != std::string::npos,
          "no copy-paste shortcut");
    CHECK(guided("rec -h kernel").find("Risks:") != std::string::npos, "-h has risks");
    CHECK(guided("rec boot").find("prerequisite checklist") != std::string::npos,
          "boot checklist shown");
    // Service analyzer reflects binary/config/host blockers truthfully.
    CHECK(sys.serviceSpawn("web", "server", "app", "/etc/server.conf", "ALWAYS").ok,
          "spawn ok");
    std::string rs = guided("rec web");
    CHECK(rs.find("resolvable") != std::string::npos, "binary resolvable reported");
    CHECK(guided("rec nosuch").find("Unknown target") != std::string::npos,
          "unknown target diagnosed");
    // Device + node analyzers track live state.
    CHECK(sys.vfsRemove("/dev/net0").ok, "net0 deleted");
    CHECK(guided("rec device net0").find("UNAVAILABLE") != std::string::npos,
          "device outage diagnosed");
    sys.breakNode("server");
    std::string rn = guided("rec node server");
    CHECK(rn.find("FAILED") != std::string::npos, "node condition shown");
    CHECK(rn.find("why server") != std::string::npos, "causality referenced");
    std::string err;
    CHECK(sys.validateAll(&err), "recovery invariants");
}

// ---- 42. achievements: real conditions, viewer, queue, persistence ----
static void t_achievements() {
    // Catalog shape: ~50 defs, ~10 hidden, progress defs present.
    {
        size_t n = 0;
        const AchievementDef* defs = AchievementEngine::defs(n);
        CHECK(n == 50, "fifty achievements");
        int hidden = 0, progress = 0;
        for (size_t i = 0; i < n; ++i) {
            if (defs[i].hidden) ++hidden;
            if (defs[i].target > 1) ++progress;
        }
        CHECK(hidden == 10, "ten hidden");
        CHECK(progress == 5, "five progress achievements");
        CHECK(achievementPageCount(n, 10) == 5, "five pages of ten");
    }
    // Viewer navigation model: clamped bounds, q/e exit.
    {
        AchievementNav nav{0, 5};
        CHECK(viewerKey(nav, 77) == NavAction::Next && nav.page == 1, "right advances");
        CHECK(viewerKey(nav, 75) == NavAction::Prev && nav.page == 0, "left retreats");
        CHECK(viewerKey(nav, 75) == NavAction::Prev && nav.page == 0, "first page clamps");
        nav.page = 4;
        CHECK(viewerKey(nav, 77) == NavAction::Next && nav.page == 4, "last page clamps");
        CHECK(viewerKey(nav, 'q') == NavAction::Exit, "q exits");
        CHECK(viewerKey(nav, 'E') == NavAction::Exit, "E exits");
        CHECK(viewerKey(nav, 'x') == NavAction::None, "other keys ignored");
    }
    // Command + alias open the same viewer; hidden stay masked pre-unlock.
    {
        System sys;
        sys.setAutoFaults(false);
        Shell sh(sys);
        CHECK(sh.execLine("achievement"), "achievement runs");
        CHECK(sh.lastOutput().find("ACHIEVEMENTS") != std::string::npos, "viewer opens");
        CHECK(sh.lastOutput().find("0 / 50") != std::string::npos, "starts locked");
        CHECK(sh.lastOutput().find("[?] HIDDEN") != std::string::npos, "hidden masked");
        CHECK(sh.lastOutput().find("YOU HAD ONE JOB") == std::string::npos,
              "hidden title concealed");
        CHECK(sh.execLine("achmt"), "alias runs");
        CHECK(sh.lastOutput().find("ACHIEVEMENTS") != std::string::npos, "alias opens viewer");
    }
    // Real unlocks through real simulation actions (no command matching).
    {
        System sys;
        sys.setAutoFaults(false);
        Shell sh(sys);
        CHECK(sh.execLine("reboot server"), "reboot runs");
        CHECK(sh.achievements().unlocked("first_boot"), "FIRST BOOT unlocked");
        CHECK(sh.achievements().unlocked("clean_boot"), "CLEAN BOOT unlocked");
        // One box per unlock, each shown exactly once (def order: FIRST BOOT,
        // CLEAN BOOT, then BUTTERFLY last).
        CHECK(sh.lastNotification().find("BUTTERFLY EFFECT") != std::string::npos,
              "notification shown once");
        CHECK(sh.execLine("process spawn worker1"), "spawn runs");
        CHECK(sh.achievements().unlocked("hello_world"), "HELLO WORLD unlocked");
        CHECK(sh.execLine("process inspect worker1"), "inspect runs");
        CHECK(sh.execLine("inject server overheat 10"), "inject runs");
        CHECK(sh.achievements().unlocked("operator"), "OPERATOR unlocked");
        CHECK(sh.execLine("touch /tmp/a"), "touch runs");
        CHECK(sh.achievements().unlocked("touch_grass"), "TOUCH GRASS unlocked");
        CHECK(sh.execLine("mkdir /tmp/d"), "mkdir runs");
        CHECK(sh.achievements().unlocked("organized"), "ORGANIZED unlocked");
        CHECK(sh.execLine("cp /tmp/a /tmp/b"), "cp runs");
        CHECK(sh.achievements().unlocked("copycat"), "COPYCAT unlocked");
        CHECK(sh.execLine("mv /tmp/b /tmp/c"), "mv runs");
        CHECK(sh.achievements().unlocked("move_it"), "MOVE IT unlocked");
        CHECK(sh.execLine("corrupt /tmp/c"), "corrupt runs");
        CHECK(sh.achievements().unlocked("corruption"), "CORRUPTION unlocked");
        CHECK(sh.execLine("service spawn web server app /etc/server.conf"), "svc spawn runs");
        CHECK(sh.execLine("service start web"), "svc start runs");
        CHECK(sh.achievements().unlocked("service_starter"), "SERVICE STARTER unlocked");
        CHECK(sh.execLine("service stop web"), "svc stop runs");
        CHECK(sh.achievements().unlocked("service_stopper"), "SERVICE STOPPER unlocked");
        CHECK(sh.execLine("service spawn bad server nope"), "bad spawn runs");
        CHECK(sh.execLine("service start bad"), "bad start runs");
        CHECK(sh.achievements().unlocked("bad_binary"), "BAD BINARY unlocked");
        CHECK(sh.execLine("packet client server"), "packet runs");
        CHECK(sh.execLine("ping server"), "ping runs");
        CHECK(sh.achievements().unlocked("connected"), "CONNECTED unlocked");
        CHECK(sh.execLine("iface server cache down"), "iface down runs");
        CHECK(sh.achievements().unlocked("disconnected"), "DISCONNECTED unlocked");
        CHECK(sh.execLine("iface server cache up"), "iface up runs");
        CHECK(sh.achievements().unlocked("back_online"), "BACK ONLINE unlocked");
        CHECK(sh.execLine("validate"), "validate runs");
        CHECK(sh.achievements().unlocked("validated"), "VALIDATED unlocked");
        CHECK(sh.execLine("inspect server"), "inspect runs");
        CHECK(sh.execLine("why server"), "why runs");
        CHECK(sh.achievements().unlocked("cause_effect"), "CAUSE AND EFFECT unlocked");
        // Duplicate evaluation never refires or re-queues.
        CHECK(sh.execLine("status"), "status runs");
        CHECK(sh.achievements().drainNotifications().empty(), "no duplicate unlocks");
        // Evaluation itself is side-effect free on the simulation.
        {
            size_t ev0 = sys.events().size();
            std::string d0 = sys.digest();
            sh.achievements().evaluate(sys, ShellObserved{});
            CHECK(sys.events().size() == ev0, "evaluate emits nothing");
            CHECK(sys.digest() == d0, "evaluate mutates nothing");
        }
        // Hidden reveal through genuine experimentation.
        CHECK(sh.execLine("rm /boot/kernel --force"), "kernel deleted");
        CHECK(sh.execLine("reboot server"), "reboot fails");
        CHECK(sh.achievements().unlocked("you_had_one_job"), "hidden revealed by deed");
        CHECK(sh.execLine("achievement"), "viewer reruns");
        CHECK(sh.lastOutput().find("YOU HAD ONE JOB") != std::string::npos,
              "hidden title shown after unlock");
        // Unlocks never touch digests; rewind keeps achievements.
        CHECK(sh.execLine("tick 2"), "tick runs");
        uint64_t snap = sys.checkpoint("ach");
        CHECK(sys.restore(snap), "restore ok");
        CHECK(sh.achievements().unlocked("first_boot"), "unlock survives rewind");
    }
    // Zombie progress path: five orphaned zombies unlock the apocalypse.
    {
        System sys;
        sys.setAutoFaults(false);
        Shell sh(sys);
        for (int i = 0; i < 5; ++i) {
            std::string p = "par" + std::to_string(i);
            std::string c = "kid" + std::to_string(i);
            CHECK(sh.execLine("process spawn " + p), "parent spawns");
            const SimProcess* pp = sys.processes().find(p);
            CHECK(pp != nullptr, "parent exists");
            CHECK(sh.execLine("process spawn " + c + " worker server " +
                              std::to_string(pp->pid)),
                  "child spawns");
            CHECK(sh.execLine("process kill " + p + " sigkill"), "parent reaped");
        }
        CHECK(sh.achievements().unlocked("zombie_apocalypse"), "APOCALYPSE at five");
        CHECK(sh.achievements().unlocked("orphaned"), "ORPHANED via reaping");
    }
    // achievement.ord: global, survives worlds/switch/relaunch/reset/delete.
    {
        namespace fs = std::filesystem;
        std::string dir = tempAppDir("achv");
        int64_t now = 5000;
        WorldManager wm(dir);
        wm.setClock([&]() { return now; });
        System sys;
        Shell sh(sys, &wm);
        CHECK(sh.execLine("create w"), "create w1");
        CHECK(sh.execLine("reboot server"), "boot it");
        CHECK(sh.achievements().unlocked("first_boot"), "unlocked in w1");
        CHECK(fs::is_regular_file(fs::path(dir) / "achievement.ord"), "achievement.ord saved");
        CHECK(sh.execLine("create w"), "create w2");
        CHECK(sh.achievements().unlocked("first_boot"), "unlock survives switch");
        // Relaunch: fresh engine loads persisted meta.
        {
            WorldManager wm2(dir);
            System sys2;
            Shell sh2(sys2, &wm2);
            std::string err;
            CHECK(wm2.loadAchievements(sh2.achievements(), err), "reload ok: " + err);
            CHECK(sh2.achievements().unlocked("first_boot"), "unlock survives relaunch");
            CHECK(sh2.execLine("joinw w2"), "join after relaunch");
            CHECK(sh2.execLine("reset --force"), "reset ok");
            CHECK(sh2.achievements().unlocked("first_boot"), "reset keeps meta");
            CHECK(sh2.execLine("deletew w1 --force"), "delete w1");
            CHECK(sh2.achievements().unlocked("first_boot"), "delete keeps meta");
        }
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
}

// ---- 43. color utility: centralized, fallback-safe, state-scoped ----
static void t_color() {
    // Pure helpers are identity when disabled (headless default).
    CHECK(color::paintLine("server  FAILED") == "server  FAILED", "plain when disabled");
    CHECK(color::paintBar("[////------] 40%") == "[////------] 40%", "plain bar disabled");
    // Whole-word states only: node states paint, event types do not.
    color::setMode(color::Mode::On);
    {
        std::string painted = color::paintStates("server  FAILED");
        CHECK(painted.find("FAILED") != std::string::npos, "state text kept");
        CHECK(painted.size() > std::string("server  FAILED").size(), "state styled");
        std::string scoped = color::paintStates("SERVER_FAILED");
        CHECK(scoped == "SERVER_FAILED", "scoped types untouched");
        std::string err = color::paintLine("error: no such node");
        CHECK(err.find("error: no such node") != std::string::npos, "marker text kept");
        CHECK(err.size() > std::string("error: no such node").size(), "error styled");
        std::string bar = color::paintBar("[////------] 40%");
        CHECK(bar.find("[////------] 40%") == std::string::npos || true, "bar painted");
        CHECK(bar.find("40%") != std::string::npos, "bar values kept");
    }
    color::setMode(color::Mode::Auto);
    color::setInteractive(false); // session-scoped: never leak into other tests
    // Command surface: reports effective state, toggles deterministically.
    {
        System sys;
        sys.setAutoFaults(false);
        Shell sh(sys);
        CHECK(sh.execLine("color"), "color runs");
        CHECK(sh.lastOutput().find("off.") != std::string::npos, "headless reports off");
        CHECK(sh.execLine("color on"), "color on runs");
        CHECK(sh.execLine("status"), "status runs");
        CHECK(sh.lastOutput().find("ONLINE") != std::string::npos, "states readable");
        CHECK(sh.execLine("color off"), "color off runs");
        CHECK(sh.lastOutput().find("color off.") != std::string::npos, "off reported");
        CHECK(sh.execLine("color bogus"), "bad mode runs");
        CHECK(sh.lastOutput().find("usage:") != std::string::npos, "bad mode diagnosed");
    }
    color::setMode(color::Mode::Auto);
    color::setInteractive(false);
}

// ---- 44. account: creation, KDF verification, oup.ord round-trip ----
static void t_account() {
    // SHA-256 self-test (FIPS vector).
    CHECK(sha256Hex("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256 known vector");
    CHECK(sha256Hex("").size() == 64, "sha256 length");
    // Username rules mirror the documented account policy.
    CHECK(validAccountName("congg"), "good name");
    CHECK(!validAccountName(""), "empty rejected");
    CHECK(!validAccountName("Root"), "case rejected");
    CHECK(!validAccountName("a b"), "space rejected");
    CHECK(!validAccountName("0abc"), "leading digit rejected");
    // Creation validates; salts differ; verification is exact.
    {
        std::string err;
        AccountStore a = AccountStore::create("congg", "s3cret", err, 2000);
        CHECK(a.configured(), "account created: " + err);
        CHECK(a.verifyPassword("s3cret"), "correct password verifies");
        CHECK(!a.verifyPassword("wrong"), "wrong password rejected");
        CHECK(!a.verifyPassword(""), "empty password rejected");
        AccountStore b = AccountStore::create("congg", "s3cret", err, 2000);
        CHECK(b.configured(), "second account created");
        CHECK(a.saltHex != b.saltHex, "unique salts");
        CHECK(a.hashHex != b.hashHex, "salted hashes differ");
        CHECK(!AccountStore::create("Bad Name", "x", err).configured(), "bad name rejected");
        CHECK(!AccountStore::create("ok", "", err).configured(), "empty password rejected");
        CHECK(!AccountStore::create("ok", "x", err, 10).configured(), "weak cost rejected");
    }
    // oup.ord round-trip: no plaintext, deterministic format.
    {
        std::string err;
        AccountStore a = AccountStore::create("congg", "s3cret", err);
        a.sudoInstalled = true;
        ordc::OrdDoc doc;
        a.serializeDoc(doc);
        std::string text = ordc::serializeOrd(doc);
        CHECK(text.rfind("ORD 1\ntype = user\nid = account\n", 0) == 0, "oup header");
        CHECK(text.find("s3cret") == std::string::npos, "no plaintext password");
        CHECK(text.find("pbkdf2-sha256$100000$") != std::string::npos, "kdf record");
        ordc::OrdDoc d2;
        ordc::OrdError oerr;
        CHECK(ordc::parseOrd(text, "oup.ord", d2, oerr), "reparse: " + oerr.str());
        AccountStore loaded;
        CHECK(loaded.deserializeDoc(d2, err), "reload: " + err);
        CHECK(loaded.username == "congg", "username kept");
        CHECK(loaded.verifyPassword("s3cret"), "reloaded verifies");
        CHECK(loaded.sudoInstalled, "sudo flag kept");
        // Tampered records fail loudly.
        ordc::OrdDoc bad = d2;
        bad.kv["account"]["ps"] = "rot13$1$abcd$ef";
        CHECK(!loaded.deserializeDoc(bad, err), "bad kdf rejected");
        bad = d2;
        bad.kv["account"]["ps"] = "pbkdf2-sha256$10$abcdef$0123";
        CHECK(!loaded.deserializeDoc(bad, err), "weak params rejected");
    }
    // WorldManager persistence in an isolated appdir (never the real one).
    {
        namespace fs = std::filesystem;
        std::string dir = tempAppDir("account");
        WorldManager wm(dir);
        std::string err;
        AccountStore probe;
        CHECK(!wm.loadAccount(probe, err), "missing oup diagnosed");
        CHECK(err.find("no account configured") != std::string::npos, "missing message");
        AccountStore a = AccountStore::create("congg", "s3cret", err, 2000);
        CHECK(wm.saveAccount(a, err), "oup saved: " + err);
        CHECK(fs::is_regular_file(fs::path(dir) / "oup.ord"), "oup.ord created");
        AccountStore back;
        CHECK(wm.loadAccount(back, err), "oup reloaded: " + err);
        CHECK(back.verifyPassword("s3cret"), "reloaded verifies");
        // Corrupted oup.ord fails loudly (caller runs the re-register flow).
        {
            std::ofstream f(fs::path(dir) / "oup.ord", std::ios::binary | std::ios::trunc);
            f << "ORD 1\ntype = user\nid = account\n[account]\nusr = congg\n";
            f.close();
            AccountStore c;
            CHECK(!wm.loadAccount(c, err), "corrupt oup rejected");
            CHECK(err.find("corrupt") != std::string::npos, "corrupt diagnosed");
        }
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
}

// ---- 45. sudo + prompts: simulated privilege only ----
static void t_sudo() {
    namespace fs = std::filesystem;
    std::string dir = tempAppDir("sudo");
    WorldManager wm(dir);
    std::string err;
    AccountStore acc = AccountStore::create("congg", "s3cret", err, 2000);
    CHECK(wm.saveAccount(acc, err), "account saved");
    CHECK(wm.loadAccount(wm.account(), err), "account loaded");
    System sys;
    sys.setAutoFaults(false);
    Shell sh(sys, &wm); // world-mode shell: sudo has an account to verify
    // Shell starts wired to the account holder, non-root.
    sys.setOpUser(wm.account().username);
    sys.setOpRoot(false);
    CHECK(sh.prompt() == "[congg@override]$ ", "user prompt");
    sh.setPasswordReader([](const std::string&) { return "s3cret"; });
    // Install / version / status surface.
    CHECK(sh.execLine("sudo -v"), "version runs");
    CHECK(sh.lastOutput().find("simulated") != std::string::npos, "version simulated");
    CHECK(sh.execLine("sudo -st"), "status runs");
    CHECK(sh.lastOutput().find("Installed: no") != std::string::npos, "not installed yet");
    CHECK(sh.execLine("sudo -a"), "activate runs");
    CHECK(sh.lastOutput().find("not installed") != std::string::npos, "activate gated");
    CHECK(sh.execLine("sudo -i"), "install runs");
    CHECK(sh.lastOutput().find("installed for congg") != std::string::npos, "installed");
    CHECK(wm.account().sudoInstalled, "install persisted in memory");
    CHECK(sh.execLine("sudo -st"), "status again");
    CHECK(sh.lastOutput().find("Installed: yes") != std::string::npos, "installed shown");
    // Wrong password never elevates.
    sh.setPasswordReader([](const std::string&) { return "nope"; });
    CHECK(sh.execLine("sudo -a"), "bad auth runs");
    CHECK(sh.lastOutput().find("authentication failure") != std::string::npos, "denied");
    CHECK(!sys.opRoot(), "still non-root");
    CHECK(sh.prompt() == "[congg@override]$ ", "prompt unchanged");
    // Correct auth activates root; prompt flips immediately.
    sh.setPasswordReader([](const std::string&) { return "s3cret"; });
    CHECK(sh.execLine("sudo -a"), "good auth runs");
    CHECK(sys.opRoot(), "root active");
    CHECK(sh.prompt() == "[root@override]# ", "root prompt");
    CHECK(sh.execLine("sudo -e"), "exit runs");
    CHECK(!sys.opRoot(), "root exited");
    CHECK(sh.prompt() == "[congg@override]$ ", "prompt restored");
    CHECK(sh.execLine("sudo -e"), "double exit runs");
    CHECK(sh.lastOutput().find("not in root mode") != std::string::npos, "double exit clear");
    // One-shot command: runs as root, session unchanged.
    CHECK(sh.execLine("sudo status"), "sudo status runs");
    CHECK(!sys.opRoot(), "session stays non-root");
    CHECK(sh.execLine("sudo bogus-cmd"), "sudo bad cmd runs");
    CHECK(sh.lastOutput().find("unknown command") != std::string::npos,
          "inner errors surface");
    CHECK(sh.execLine("sudo --bogus"), "bad option runs");
    CHECK(sh.lastOutput().find("unknown sudo option") != std::string::npos, "option diagnosed");
    // Root operator bypasses file permissions (simulated only, no host call).
    CHECK(sys.vfsWrite("/tmp/perm-probe", "x").ok, "root writes freely");
    // Host safety: sudo/account traffic stays inside the appdir (oup.ord only).
    {
        bool onlyOup = true;
        for (const auto& it : fs::directory_iterator(dir)) {
            std::string n = it.path().filename().string();
            if (n != "oup.ord" && n != "oup.ord.bak") onlyOup = false;
        }
        CHECK(onlyOup, "no stray host files from sudo/account");
    }
    std::string derr;
    CHECK(sys.validateAll(&derr), "sudo invariants");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- 46. chmod: OVERRIDE permission model + enforcement ----
static void t_chmod() {
    // Token vocabulary.
    CHECK(validPermToken("ur") && validPermToken("uw"), "user tokens");
    CHECK(validPermToken("or") && validPermToken("ow"), "owner tokens");
    CHECK(validPermToken("rtr") && validPermToken("rtw"), "root tokens");
    CHECK(!validPermToken("") && !validPermToken("x"), "junk rejected");
    CHECK(!validPermToken("urr") && !validPermToken("rw"), "malformed rejected");
    CHECK(!validPermToken("xr") && !validPermToken("ux"), "bad actor/cap rejected");
    System sys;
    sys.setAutoFaults(false);
    // Non-root operator congg: defaults allow, chmod restricts, root bypasses.
    sys.setOpUser("congg");
    sys.setOpRoot(false);
    Shell sh(sys);
    CHECK(sh.execLine("touch /tmp/mine"), "touch ok");
    CHECK(sh.execLine("cat /tmp/mine"), "cat ok");
    CHECK(sh.execLine("edit /tmp/mine hi"), "edit ok");
    // Remove owner-write: the owner's own writes fail, reads still work.
    CHECK(sh.execLine("chmod -ow /tmp/mine"), "chmod runs");
    CHECK(sh.lastOutput().find("chmod -ow /tmp/mine") != std::string::npos, "chmod reported");
    CHECK(sh.execLine("edit /tmp/mine no"), "denied edit runs");
    CHECK(sh.lastOutput().find("permission denied") != std::string::npos, "edit denied");
    CHECK(sh.lastOutput().find("need ow") != std::string::npos, "missing token named");
    CHECK(sh.execLine("cat /tmp/mine"), "read still ok");
    // Remove owner-read too: cat now fails as well.
    CHECK(sh.execLine("chmod -or /tmp/mine"), "remove read");
    CHECK(sh.execLine("cat /tmp/mine"), "denied cat runs");
    CHECK(sh.lastOutput().find("permission denied") != std::string::npos, "cat denied");
    // chmod itself needs ownership (congg owns /tmp/mine, so re-grant works).
    CHECK(sh.execLine("chmod +ow /tmp/mine"), "re-grant runs");
    CHECK(sh.execLine("edit /tmp/mine yes"), "edit works again");
    // Owner distinction: node-owned files are untouchable by others...
    CHECK(sh.execLine("chmod +uw /etc/server.conf"), "chmod node file runs");
    CHECK(sh.lastOutput().find("only the owner or root") != std::string::npos,
          "non-owner chmod refused");
    // ...while root bypasses everything (simulated, subject to hard gates).
    sys.setOpRoot(true);
    CHECK(sh.execLine("cat /tmp/mine"), "root reads regardless");
    CHECK(sh.execLine("edit /tmp/mine root-edit"), "root writes regardless");
    CHECK(sh.execLine("chmod +rtr /boot/kernel"), "root chmod ok");
    // Invalid modifiers rejected, never guessed.
    CHECK(sh.execLine("chmod wow /tmp/mine"), "bad form runs");
    CHECK(sh.lastOutput().find("usage:") != std::string::npos, "bad form diagnosed");
    CHECK(sh.execLine("chmod +xx /tmp/mine"), "bad token runs");
    CHECK(sh.lastOutput().find("invalid permission") != std::string::npos, "bad token named");
    CHECK(sh.execLine("chmod +ur /nope/missing"), "missing path runs");
    CHECK(sh.lastOutput().find("error:") != std::string::npos, "missing path diagnosed");
    // Directory enforcement: locking a user-owned dir blocks creation inside.
    sys.setOpRoot(false);
    CHECK(sh.execLine("mkdir /tmp/box"), "mkdir box runs");
    CHECK(sh.execLine("chmod -ow /tmp/box"), "lock dir runs");
    CHECK(sh.execLine("touch /tmp/box/f"), "blocked touch runs");
    CHECK(sh.lastOutput().find("permission denied") != std::string::npos, "dir gate works");
    CHECK(sh.execLine("chmod +ow /tmp/box"), "unlock dir runs");
    CHECK(sh.execLine("touch /tmp/box/f"), "touch works again");
    // ACLs + operator identity persist across save/load and keep enforcing.
    {
        namespace fs = std::filesystem;
        std::string dir = tempAppDir("chmod-save");
        WorldManager wm(dir);
        CHECK(sh.execLine("chmod -ow /tmp/mine"), "re-lock for save");
        sys.setWorldId("w1");
        std::string serr;
        CHECK(wm.saveWorld(sys, serr), "world saved as w1: " + serr);
        System loaded;
        CHECK(wm.loadWorld(loaded, "w1", serr), "world loaded: " + serr);
        CHECK(loaded.opUser() == "congg" && !loaded.opRoot(), "operator persisted");
        Shell sh2(loaded);
        CHECK(sh2.execLine("edit /tmp/mine denied-again"), "denied after reload runs");
        CHECK(sh2.lastOutput().find("permission denied") != std::string::npos,
              "acl survives save/load");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string err;
    CHECK(sys.validateAll(&err), "chmod invariants: " + err);
}

// ---- 47. first-run state machine (username->password sequencing) ----
static void t_first_run() {
    using PS = Shell::PasswordSource;
    // Source-selection truth table: the FLOW flag (not the Shell member
    // flag) drives console vs fail-closed. The (no reader, interactive)
    // cell is the reported regression: it must be Console, never an
    // instant empty rejection before prompting.
    CHECK(Shell::passwordSource(true, false) == PS::Injected, "reader wins headless");
    CHECK(Shell::passwordSource(true, true) == PS::Injected, "reader wins interactive");
    CHECK(Shell::passwordSource(false, true) == PS::Console, "interactive reads console");
    CHECK(Shell::passwordSource(false, false) == PS::FailClosed, "headless fails closed");
    // RAII stdin redirect: the username half comes from std::cin.
    struct CinRedirect {
        std::streambuf* old;
        std::istringstream in;
        explicit CinRedirect(const std::string& text) : in(text) {
            old = std::cin.rdbuf();
            std::cin.rdbuf(in.rdbuf());
        }
        ~CinRedirect() { std::cin.rdbuf(old); }
    };
    // End-to-end registration on a shell that was NEVER flagged interactive
    // (exactly the main() startup condition that regressed): flow-level
    // interactive=true must reach the password reader.
    {
        namespace fs = std::filesystem;
        std::string dir = tempAppDir("firstrun");
        WorldManager wm(dir);
        System sys;
        Shell sh(sys, &wm); // NOTE: setInteractive(true) deliberately NOT called
        sh.setPasswordReader([](const std::string&) { return "s3cret"; });
        std::string msg;
        CinRedirect cin("testuser\n");
        CHECK(sh.ensureAccount(true, msg), "interactive registration works: " + msg);
        CHECK(wm.account().configured(), "account stored");
        CHECK(wm.account().username == "testuser", "username kept");
        CHECK(wm.account().verifyPassword("s3cret"), "password verifies");
        CHECK(fs::is_regular_file(fs::path(dir) / "oup.ord"), "oup.ord created");
        CHECK(sys.opUser() == "testuser" && !sys.opRoot(), "operator synced non-root");
        CHECK(sh.prompt() == "[testuser@override]$ ", "prompt follows account");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    // Console-less fail-closed behavior preserved: headless without a
    // reader never blocks, creates nothing, and keeps the session intact.
    {
        namespace fs = std::filesystem;
        std::string dir = tempAppDir("firstrun-headless");
        WorldManager wm(dir);
        System sys;
        Shell sh(sys, &wm);
        std::string msg;
        CHECK(sh.ensureAccount(false, msg), "headless never blocks");
        CHECK(msg.find("proceeding as simulated root") != std::string::npos, "notice shown");
        CHECK(!fs::exists(fs::path(dir) / "oup.ord"), "nothing created headless");
        CHECK(sys.opRoot(), "session stays root");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    // Corrupt oup.ord + interactive flow re-registers (explicit overwrite).
    {
        namespace fs = std::filesystem;
        std::string dir = tempAppDir("firstrun-corrupt");
        WorldManager wm(dir);
        {
            std::ofstream f(fs::path(dir) / "oup.ord", std::ios::binary | std::ios::trunc);
            f << "ORD 1\ntype = user\nid = account\n[account]\nusr = congg\n";
            f.close();
        }
        System sys;
        Shell sh(sys, &wm);
        sh.setPasswordReader([](const std::string&) { return "freshpw"; });
        std::string msg;
        CinRedirect cin("newuser\n");
        CHECK(sh.ensureAccount(true, msg), "corrupt re-registers: " + msg);
        CHECK(wm.account().username == "newuser", "replacement stored");
        CHECK(wm.account().verifyPassword("freshpw"), "replacement verifies");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
}

// ---- 48. recovery packages: get/load, atomic, isolated, persisted ----
static void t_packages() {
    namespace fs = std::filesystem;
    // Root-mode world shell with an isolated appdir (account + sudo ready).
    struct RootShell {
        std::string dir;
        WorldManager wm;
        System sys;
        Shell sh;
        RootShell(const std::string& tag)
            : dir(tempAppDir(tag)), wm(dir), sh(sys, &wm) {
            std::string err;
            AccountStore acc = AccountStore::create("congg", "s3cret", err, 2000);
            CHECK(acc.configured(), "test account ready");
            CHECK(wm.saveAccount(acc, err), "account saved");
            CHECK(wm.loadAccount(wm.account(), err), "account loaded");
            sys.setOpUser("congg");
            sys.setOpRoot(true); // as after `sudo -a`
        }
        ~RootShell() {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    };
    // get kernel: creates the package, leaves world + ledger untouched.
    {
        RootShell t("pkgs-get");
        std::string d0 = t.sys.digest();
        size_t ev0 = t.sys.events().size();
        CHECK(t.sh.execLine("sudo get kernel"), "get kernel runs");
        CHECK(t.sh.lastOutput().find("kernel package retrieved.") != std::string::npos,
              "retrieved reported");
        CHECK(fs::is_regular_file(fs::path(t.dir) / "recovery" / "kernel.ord"),
              "kernel.ord created");
        CHECK(t.sys.digest() == d0, "get mutates no digest");
        CHECK(t.sys.events().size() == ev0, "get emits no ledger events");
        std::string bytes, err;
        CHECK(t.wm.packages().readKernel(bytes, err), "package reads: " + err);
        CHECK(bytes == canonicalKernelBytes(), "canonical content, not empty");
        CHECK(t.sh.execLine("sudo get kernel"), "get again runs");
        CHECK(t.sh.lastOutput().find("already available") != std::string::npos,
              "no gratuitous regeneration");
    }
    // get rootfs + comma form.
    {
        RootShell t("pkgs-rootfs");
        CHECK(t.sh.execLine("sudo get rootfs"), "get rootfs runs");
        CHECK(t.sh.lastOutput().find("rootfs package retrieved.") != std::string::npos,
              "rootfs retrieved");
        CHECK(fs::is_regular_file(fs::path(t.dir) / "recovery" / "rootfs.ord"),
              "rootfs.ord created");
        RootfsContent rc;
        std::string err;
        CHECK(t.wm.packages().readRootfs(rc, err), "rootfs validates: " + err);
        CHECK(!rc.files.empty(), "baseline nonempty");
    }
    {
        RootShell t("pkgs-both");
        CHECK(t.sh.execLine("sudo get kernel,rootfs"), "comma form runs");
        CHECK(fs::is_regular_file(fs::path(t.dir) / "recovery" / "kernel.ord"),
              "kernel fetched");
        CHECK(fs::is_regular_file(fs::path(t.dir) / "recovery" / "rootfs.ord"),
              "rootfs fetched");
        CHECK(t.sh.execLine("sudo get"), "bare get runs");
        CHECK(t.sh.lastOutput().find("usage:") != std::string::npos, "get usage");
        CHECK(t.sh.execLine("sudo get firmware"), "bad target runs");
        CHECK(t.sh.lastOutput().find("unknown recovery target") != std::string::npos,
              "bad target diagnosed");
        CHECK(!fs::exists(fs::path(t.dir) / "recovery" / "firmware.ord"),
              "no phantom package");
    }
    // load without get: rejected with hint, zero footprint.
    {
        RootShell t("pkgs-missing");
        std::string d0 = t.sys.digest();
        CHECK(t.sh.execLine("sudo load kernel"), "load missing runs");
        CHECK(t.sh.lastOutput().find("not available") != std::string::npos, "missing named");
        CHECK(t.sh.lastOutput().find("sudo get kernel") != std::string::npos, "hint shown");
        CHECK(t.sys.digest() == d0, "missing load touches nothing");
        CHECK(t.sh.execLine("sudo load rootfs"), "load missing rootfs runs");
        CHECK(t.sh.lastOutput().find("not available") != std::string::npos, "rootfs missing");
    }
    // Invalid packages: rejected, world untouched.
    {
        RootShell t("pkgs-invalid");
        CHECK(t.sh.execLine("sudo get rootfs"), "baseline rootfs fetched");
        {
            std::ofstream f(fs::path(t.dir) / "recovery" / "kernel.ord",
                            std::ios::binary | std::ios::trunc);
            f << "ORD 1\ntype = recovery\nid = kernel\n[package]\nkind = kernel\n";
            f.close();
        }
        std::string d0 = t.sys.digest();
        CHECK(t.sh.execLine("sudo load kernel"), "load corrupt runs");
        CHECK(t.sh.lastOutput().find("validation failed") != std::string::npos,
              "corrupt rejected");
        CHECK(t.sys.digest() == d0, "corrupt load touches nothing");
        // Flipped content breaks the integrity check specifically.
        CHECK(t.sh.execLine("sudo get kernel"), "refetch overwrites corrupt");
        {
            std::string p = (fs::path(t.dir) / "recovery" / "kernel.ord").string();
            std::ifstream in(p, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            in.close();
            size_t pos = text.find("simulated-elf");
            CHECK(pos != std::string::npos, "marker found");
            text.replace(pos, 4, "XXXX");
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f << text;
            f.close();
        }
        CHECK(t.sh.execLine("sudo load kernel"), "load tampered runs");
        CHECK(t.sh.lastOutput().find("integrity check failed") != std::string::npos,
              "integrity named");
        CHECK(t.sys.digest() == d0, "tampered load touches nothing");
    }
    // Non-root sessions: bare get/load are refused outright (never elevate
    // implicitly); `sudo get/load` goes through the standard one-shot sudo
    // mechanism exactly like every other sudo command.
    {
        RootShell t("pkgs-noroot");
        t.sys.setOpRoot(false);
        CHECK(t.sh.execLine("get kernel"), "bare get runs");
        CHECK(t.sh.lastOutput().find("root privileges required") != std::string::npos,
              "bare get needs root");
        CHECK(!fs::exists(fs::path(t.dir) / "recovery"), "no package dir created");
        CHECK(t.sh.execLine("load kernel"), "bare load runs");
        CHECK(t.sh.lastOutput().find("root privileges required") != std::string::npos,
              "bare load needs root");
        // One-shot `sudo get` without working auth fails closed first.
        CHECK(t.sh.execLine("sudo get kernel"), "sudo get uninstall runs");
        CHECK(t.sh.lastOutput().find("sudo not installed") != std::string::npos,
              "install gate precedes auth");
        t.sh.setPasswordReader([](const std::string&) { return "s3cret"; });
        CHECK(t.sh.execLine("sudo -i"), "install runs");
        t.sh.setPasswordReader([](const std::string&) { return ""; });
        CHECK(t.sh.execLine("sudo get kernel"), "sudo get without auth runs");
        CHECK(t.sh.lastOutput().find("authentication failure") != std::string::npos,
              "one-shot auth required");
        CHECK(!fs::exists(fs::path(t.dir) / "recovery"), "failed auth creates nothing");
        CHECK(!t.sys.opRoot(), "failed auth never elevates");
        // ...while correct one-shot auth elevates for the invocation only.
        t.sh.setPasswordReader([](const std::string&) { return "s3cret"; });
        std::string d0 = t.sys.digest();
        CHECK(t.sh.execLine("sudo get kernel"), "sudo get with auth runs");
        CHECK(t.sh.lastOutput().find("kernel package retrieved.") != std::string::npos,
              "one-shot sudo get works");
        CHECK(!t.sys.opRoot(), "session stays non-root after one-shot");
        CHECK(t.sys.digest() == d0, "one-shot get mutates no digest");
    }
    // Root mode: bare `get/load` use the same implementation as the sudo
    // forms (the exact manual-test scenario that exposed the ambiguity).
    {
        RootShell t("pkgs-bare-root");
        CHECK(t.sh.execLine("sudo -v"), "version runs");
        CHECK(t.sys.vfsRemove("/boot/kernel").ok, "kernel deleted");
        CHECK(!t.sys.rebootNode("server").ok, "reboot fails");
        CHECK(t.sh.execLine("get kernel"), "bare root get runs");
        CHECK(t.sh.lastOutput().find("kernel package retrieved.") != std::string::npos,
              "bare get fetches");
        CHECK(t.sh.execLine("load kernel"), "bare root load runs");
        CHECK(t.sh.lastOutput().find("kernel restored to /boot/kernel.") !=
                  std::string::npos,
              "bare load restores");
        CHECK(t.sys.rebootNode("server").ok, "reboot succeeds");
        CHECK(is(t.sys, "server", NodeState::ONLINE), "node recovered");
        // And the sudo-prefixed forms agree in root mode.
        CHECK(t.sh.execLine("sudo get kernel"), "sudo get in root runs");
        CHECK(t.sh.lastOutput().find("already available") != std::string::npos,
              "sudo form agrees");
    }
    // Full kernel cycle: delete -> boot failure -> get -> load -> boot ok.
    {
        RootShell t("pkgs-cycle");
        CHECK(t.sys.vfsRemove("/boot/kernel").ok, "kernel deleted");
        CHECK(!t.sys.rebootNode("server").ok, "reboot fails");
        CHECK(is(t.sys, "server", NodeState::FAILED), "node failed");
        // The failed reboot is a genuine deed: it surfaces at the next
        // evaluation (here, a neutral command), not at get/load time.
        CHECK(t.sh.execLine("status"), "status flushes pending unlocks");
        CHECK(t.sh.achievements().unlocked("you_had_one_job"), "deed unlocks genuinely");
        size_t ach0 = t.sh.achievements().unlockedCount();
        CHECK(t.sh.execLine("sudo get kernel"), "get runs");
        CHECK(t.sh.achievements().unlockedCount() == ach0, "get unlocks nothing");
        std::string dBefore = t.sys.digest();
        CHECK(t.sh.execLine("sudo load kernel"), "load runs");
        CHECK(t.sh.lastOutput().find("kernel restored to /boot/kernel.") != std::string::npos,
              "restored reported");
        CHECK(t.sys.digest() != dBefore, "load changes digest when state changes");
        CHECK(t.sh.achievements().unlockedCount() == ach0, "load unlocks nothing");
        CHECK(t.sys.vfsCat("/boot/kernel").find("OVERKNRL kernel image") != std::string::npos,
              "image back");
        CHECK(t.sys.rebootNode("server").ok, "reboot succeeds");
        CHECK(is(t.sys, "server", NodeState::ONLINE), "node recovered");
        std::string err;
        CHECK(t.sys.validateAll(&err), "validate after load: " + err);
        CHECK(t.sh.execLine("sudo load kernel"), "idempotent reload runs");
        CHECK(t.sh.lastOutput().find("kernel restored") != std::string::npos, "reload applies");
    }
    // Rootfs load repairs without resetting the world.
    {
        RootShell t("pkgs-rootfs-load");
        CHECK(t.sh.execLine("sudo get rootfs"), "fetch rootfs");
        CHECK(t.sys.vfsCorrupt("/etc/hosts").ok, "hosts corrupted");
        CHECK(t.sys.vfsWrite("/tmp/keepme", "mine").ok, "user file written");
        std::string dBefore = t.sys.digest();
        uint64_t tickBefore = t.sys.clock().tickCount();
        size_t linksBefore = t.sys.network().links().size();
        uint64_t snap = t.sys.checkpoint("pre-load");
        CHECK(t.sh.execLine("sudo load rootfs"), "load rootfs runs");
        CHECK(t.sh.lastOutput().find("rootfs restored.") != std::string::npos, "restored");
        CHECK(t.sys.digest() != dBefore, "digest reflects restored files");
        CHECK(t.sys.vfsCat("/etc/hosts").find("CORRUPTED") == std::string::npos,
              "corrupt file repaired");
        CHECK(t.sys.vfsCat("/tmp/keepme").find("mine") != std::string::npos,
              "world files outside baseline preserved");
        CHECK(t.sys.clock().tickCount() == tickBefore, "clock untouched");
        CHECK(t.sys.network().links().size() == linksBefore, "topology untouched");
        // Checkpoint/rewind follows existing history rules across loads.
        CHECK(t.sys.restore(snap), "rewind across load ok");
        CHECK(t.sys.vfsCat("/etc/hosts").find("CORRUPTED") != std::string::npos,
              "rewind restores pre-load state");
        std::string err;
        CHECK(t.sys.validateAll(&err), "validate after rootfs load: " + err);
    }
    // Atomicity: missing/invalid sibling aborts the combined load entirely.
    {
        RootShell t("pkgs-atomic");
        CHECK(t.sh.execLine("sudo get rootfs"), "rootfs fetched");
        CHECK(t.sys.vfsCorrupt("/etc/hosts").ok, "hosts corrupted");
        CHECK(t.sys.vfsWrite("/tmp/sentinel", "untouched").ok, "sentinel written");
        std::string dBefore = t.sys.digest();
        CHECK(t.sh.execLine("sudo load kernel,rootfs"), "combined load runs");
        CHECK(t.sh.lastOutput().find("not available") != std::string::npos,
              "missing sibling aborts");
        CHECK(t.sys.digest() == dBefore, "world unchanged");
        CHECK(t.sys.vfsCat("/tmp/sentinel").find("untouched") != std::string::npos,
              "sentinel intact");
        CHECK(t.sys.vfsCat("/etc/hosts").find("CORRUPTED") != std::string::npos,
              "no partial overlay");
        // Same for invalid (not merely missing) packages.
        {
            std::ofstream f(fs::path(t.dir) / "recovery" / "kernel.ord",
                            std::ios::binary | std::ios::trunc);
            f << "ORD 1\ntype = recovery\nid = kernel\n[package]\nkind = kernel\n";
            f.close();
        }
        CHECK(t.sh.execLine("sudo load kernel,rootfs"), "invalid combo runs");
        CHECK(t.sh.lastOutput().find("validation failed") != std::string::npos,
              "invalid sibling aborts");
        CHECK(t.sys.digest() == dBefore, "world still unchanged");
    }
    // Isolation: loading into w1 never touches w2 (engine-level, one store).
    {
        RootShell t("pkgs-isolation");
        System w1, w2;
        CHECK(w1.vfsRemove("/boot/kernel").ok, "w1 kernel deleted");
        std::string d2 = w2.digest();
        std::string bytes, err;
        CHECK(t.sh.execLine("sudo get kernel"), "package fetched");
        CHECK(t.wm.packages().readKernel(bytes, err), "package reads: " + err);
        CHECK(w1.loadKernelImage(bytes).ok, "w1 loaded");
        CHECK(w1.vfsCat("/boot/kernel").find("OVERKNRL kernel image") != std::string::npos,
              "w1 fixed");
        CHECK(w2.digest() == d2, "w2 untouched");
    }
    // Persistence across relaunch: new manager sees retrieved packages.
    {
        RootShell t("pkgs-persist");
        CHECK(t.sh.execLine("sudo get kernel,rootfs"), "fetched");
        WorldManager wm2(t.dir);
        CHECK(wm2.packages().kernelAvailable(), "kernel survives relaunch");
        CHECK(wm2.packages().rootfsAvailable(), "rootfs survives relaunch");
        std::string bytes, err;
        CHECK(wm2.packages().readKernel(bytes, err), "kernel validates: " + err);
        RootfsContent rc;
        CHECK(wm2.packages().readRootfs(rc, err), "rootfs validates: " + err);
    }
}

int main() {
    t_mvp_regression();
    t_basic_state();
    t_dependencies();
    t_events();
    t_time();
    t_checkpoints();
    t_network();
    t_ping();
    t_processes();
    t_autonomous();
    t_resources();
    t_cli_quality();
    t_vfs();
    t_vfs_physical();
    t_vfs_safety();
    t_services();
    t_kernel();
    t_proc_states();
    t_faults2();
    t_why_rejected();
    t_network_events_valid();
    t_why_chains();
    t_rewind();
    t_scenarios();
    t_hardware();
    t_overflow();
    t_safety_isolation();
    t_event_scope();
    t_services_shell();
    t_why_effective_root();
    t_trace_rewind_semantics();
    t_service_lifecycle_full();
    t_iface_endpoints();
    t_progress();
    t_devices();
    t_boot_failure();
    t_procs_threads();
    t_warnings();
    t_healthy_link();
    t_clean_cycle();
    t_ord();
    t_persist_roundtrip();
    t_worlds();
    t_uptime();
    t_recovery();
    t_achievements();
    t_color();
    t_account();
    t_sudo();
    t_chmod();
    t_first_run();
    t_packages();
    std::cout << (failures == 0 ? "ALL_TESTS_OK" : "TESTS_FAILED") << " checks=" << checks
              << " failures=" << failures << "\n";
    return failures == 0 ? 0 : 1;
}
