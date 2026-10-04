// System (ORD persistence: serialize/deserialize the authoritative world).
// Snapshots (checkpoints), command history, and observational syscall
// counters are runtime-only and are NOT persisted. Everything else is:
// identity, meta, uptime totals, clock, RNG, sim config, hardware (V/R),
// VFS, nodes (+faults), links, processes (+threads, mailboxes), services,
// and the full event ledger (causality survives save/load).
#include "override/system.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <vector>

namespace override {

void System::commitUptimeSession(int64_t now) {
    if (uptimeSessionStart_ == 0) return; // no active session: nothing to fold
    if (now > uptimeSessionStart_) uptimeTotalSecs_ += (uint64_t)(now - uptimeSessionStart_);
    uptimeSessionStart_ = now; // re-baseline: repeated saves never double-count
}

void System::endUptimeSession(int64_t now) {
    commitUptimeSession(now);
    uptimeSessionStart_ = 0;
}

int64_t System::sessionUptimeSecs(int64_t now) const {
    if (uptimeSessionStart_ == 0 || now <= uptimeSessionStart_) return 0;
    return now - uptimeSessionStart_;
}

uint64_t System::allTimeUptimeSecs(int64_t now) const {
    return uptimeTotalSecs_ + (uint64_t)sessionUptimeSecs(now);
}

namespace {
std::string b2s(bool b) { return b ? "true" : "false"; }

std::string dbl(double v) {
    std::ostringstream o;
    o << std::setprecision(17) << v;
    return o.str();
}

bool validKey(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '.' || c == '/' || c == '-';
        if (!ok) return false;
    }
    return true;
}
} // namespace

bool System::serializeWorldDoc(ordc::OrdDoc& doc, std::string& err) const {
    doc = ordc::OrdDoc{};
    doc.type = "world";
    doc.id = worldId_;
    auto S = [&](const std::string& sec, const std::string& k, const std::string& v) {
        doc.set(sec, k, v);
    };
    S("meta", "name", worldName_);
    S("meta", "note", worldNote_);
    S("meta", "created_tick", std::to_string(worldCreatedTick_));
    S("meta", "saved_tick", std::to_string(clock_.tickCount()));
    S("uptime", "created_at", std::to_string(worldCreatedAt_));
    S("uptime", "total_active_seconds", std::to_string(uptimeTotalSecs_));
    S("clock", "tick", std::to_string(clock_.tickCount()));
    S("clock", "paused", b2s(clock_.paused()));
    S("random", "seed", std::to_string(seed_));
    S("random", "state", std::to_string(rng_));
    S("sim", "autofaults", b2s(autoFaults_));
    S("sim", "autorate", std::to_string(autoRate_));
    S("sim", "mode", mode_);
    S("sim", "nextpid", std::to_string(pm_.nextPid()));
    S("sim", "cwd", cwd_);
    S("operator", "op_user", opUser_);
    S("operator", "op_root", opRoot_ ? "true" : "false");
    // VFS: directory list (with owner/acl) + one section per file.
    // Hardware (V/R model, fully).
    {
        const HardwareProfile& p = hw_.profile();
        S("hardware", "cpu_model", p.cpuModel);
        S("hardware", "cpu_cores", std::to_string(p.cpuCores));
        S("hardware", "ram_mb", std::to_string(p.ramMb));
        S("hardware", "storage_mb", std::to_string(p.storageMb));
        S("hardware", "backend_cpu", toString(hw_.cpu()));
        S("hardware", "backend_ram", toString(hw_.ram()));
        S("hardware", "backend_storage", toString(hw_.storage()));
        S("hardware", "backend_net", toString(hw_.net()));
        S("hardware", "policy", toString(hw_.policy()));
        S("hardware", "cap_ram", std::to_string(hw_.backing().ramMbCap));
        S("hardware", "cap_storage", std::to_string(hw_.backing().storageMbCap));
        int bi = 0;
        for (const auto& [k, bytes] : hw_.backing().blobs) {
            std::string pre = "blob." + std::to_string(bi++);
            S("hardware", pre + ".key", k);
            auto rit = hw_.backing().blobResource.find(k);
            S("hardware", pre + ".res", rit == hw_.backing().blobResource.end() ? "" : rit->second);
            S("hardware", pre + ".bytes", bytes);
        }
        S("hardware", "blob_count", std::to_string(bi));
    }
    // VFS: directory list + one section per file.
    {
        std::vector<std::string> dirs{"/"}, files;
        std::vector<VEntry> entries;
        std::string err;
        // Breadth-first over list() (sorted entries).
        std::vector<std::string> stack{"/"};
        while (!stack.empty()) {
            std::string d = stack.back();
            stack.pop_back();
            entries.clear();
            if (!vfs_.list(d, entries, err)) continue; // validated tree lists cleanly
            for (const auto& e : entries) {
                std::string full = (d == "/") ? ("/" + e.name) : (d + "/" + e.name);
                if (e.isDir) {
                    std::string dd = full.substr(0, full.size() - 1); // list() appends '/'
                    dirs.push_back(dd);
                    stack.push_back(dd);
                } else {
                    files.push_back(full);
                }
            }
        }
        std::sort(dirs.begin(), dirs.end());
        std::sort(files.begin(), files.end());
        S("vfs", "dir_count", std::to_string(dirs.size()));
        for (size_t i = 0; i < dirs.size(); ++i) {
            std::string pre = "dir." + std::to_string(i);
            S("vfs", pre, dirs[i]);
            const VDir* dd = vfs_.dir(dirs[i]);
            S("vfs", pre + ".owner", dd ? dd->owner : "");
            int ai = 0;
            if (dd) {
                for (const auto& t : dd->acl) S("vfs", pre + ".acl." + std::to_string(ai++), t);
            }
            S("vfs", pre + ".acl_count", std::to_string(ai));
        }
        S("vfs", "file_count", std::to_string(files.size()));
        for (size_t i = 0; i < files.size(); ++i) {
            std::string sec = "file." + std::to_string(i);
            const VFile* f = vfs_.file(files[i]);
            if (!f) continue; // cannot happen on a validated tree
            S(sec, "path", files[i]);
            S(sec, "owner", f->owner);
            S(sec, "exec", f->executable ? "1" : "0");
            S(sec, "corrupt", f->corrupted ? "1" : "0");
            S(sec, "mtime", std::to_string(f->mtime));
            S(sec, "content", f->content);
            int ai = 0;
            for (const auto& t : f->acl) S(sec, "acl." + std::to_string(ai++), t);
            S(sec, "acl_count", std::to_string(ai));
        }
    }
    // Nodes (+faults, modules, binaries, deps, metadata, beliefs).
    for (const auto& [name, n] : nodes_) {
        std::string sec = "node." + name;
        S(sec, "type", n.type);
        S(sec, "state", toString(n.state));
        S(sec, "health", std::to_string(n.health));
        S(sec, "latency", std::to_string(n.latencyMs));
        S(sec, "base_latency", std::to_string(n.baseLatencyMs));
        S(sec, "load", std::to_string(n.load));
        S(sec, "memory_mb", std::to_string(n.memoryMb));
        S(sec, "memory_used", std::to_string(n.memoryUsedMb));
        S(sec, "connections", std::to_string(n.connections));
        S(sec, "max_connections", std::to_string(n.maxConnections));
        S(sec, "temp", std::to_string(n.tempC));
        S(sec, "ambient", std::to_string(n.ambientC));
        S(sec, "thermal_limit", std::to_string(n.thermalLimitC));
        S(sec, "critical", std::to_string(n.criticalC));
        S(sec, "throttled", b2s(n.throttled));
        S(sec, "freq", std::to_string(n.freqMHz));
        S(sec, "base_freq", std::to_string(n.baseFreqMHz));
        S(sec, "max_freq", std::to_string(n.maxFreqMHz));
        S(sec, "clock", toString(n.clockState));
        S(sec, "mem_corrupt", b2s(n.memCorrupt));
        S(sec, "leak", std::to_string(n.leakMbPerTick));
        S(sec, "swap", std::to_string(n.swapMb));
        S(sec, "swap_used", std::to_string(n.swapUsedMb));
        S(sec, "oom_kills", std::to_string(n.oomKills));
        S(sec, "storage_mb", std::to_string(n.storageMb));
        S(sec, "storage_used", std::to_string(n.storageUsedMb));
        S(sec, "io", std::to_string(n.ioLoad));
        for (const auto& [m, st] : n.filesystems) S(sec, "fs." + m, toString(st));
        S(sec, "kernel", toString(n.kernel));
        S(sec, "instability", std::to_string(n.instability));
        for (const auto& [m, st] : n.modules) S(sec, "module." + m, st);
        int bi = 0;
        for (const auto& b : n.binaries) S(sec, "bin." + std::to_string(bi++), b);
        S(sec, "bin_count", std::to_string(bi));
        for (size_t i = 0; i < n.dependencies.size(); ++i)
            S(sec, "dep." + std::to_string(i), n.dependencies[i]);
        S(sec, "dep_count", std::to_string(n.dependencies.size()));
        for (const auto& [k, v] : n.metadata) S(sec, "meta." + k, v);
        for (const auto& [t, props] : n.beliefs)
            for (const auto& [k, v] : props) S(sec, "belief." + t + "." + k, v);
        S(sec, "proc", n.proc);
        S(sec, "priv", toString(n.priv));
        S(sec, "config_corrupt", b2s(n.configCorrupt));
        for (const auto& [fname, f] : n.faults) {
            std::string pre = "fault." + fname;
            S(sec, pre + ".kind", toString(f.kind));
            S(sec, pre + ".sev", toString(f.severity));
            S(sec, pre + ".since", std::to_string(f.sinceTick));
            S(sec, pre + ".event", std::to_string(f.eventId));
            S(sec, pre + ".detail", f.detail);
        }
    }
    // Links (sorted).
    {
        auto links = net_.links();
        std::sort(links.begin(), links.end(), [](const Link& a, const Link& b) {
            if (a.a != b.a) return a.a < b.a;
            return a.b < b.b;
        });
        S("net", "link_count", std::to_string(links.size()));
        for (size_t i = 0; i < links.size(); ++i) {
            std::string pre = "link." + std::to_string(i);
            S("net", pre + ".a", links[i].a);
            S("net", pre + ".b", links[i].b);
            S("net", pre + ".up", b2s(links[i].up));
            S("net", pre + ".lat", std::to_string(links[i].latencyMs));
            S("net", pre + ".loss", dbl(links[i].lossPct));
            S("net", pre + ".load", dbl(links[i].loadPct));
        }
    }
    // Processes (+threads, mailboxes).
    for (const auto& [pid, p] : pm_.all()) {
        std::string sec = "proc." + std::to_string(pid);
        S(sec, "name", p.name);
        S(sec, "ptype", p.type);
        S(sec, "state", toString(p.state));
        S(sec, "cpu", std::to_string(p.cpu));
        S(sec, "mem", std::to_string(p.memMb));
        S(sec, "priority", std::to_string(p.priority));
        S(sec, "parent", std::to_string(p.parentPid));
        S(sec, "host", p.host);
        S(sec, "runtime", std::to_string(p.runtime));
        S(sec, "thread_seq", std::to_string(p.threadSeq));
        S(sec, "thread_count", std::to_string(p.threads.size()));
        for (size_t i = 0; i < p.threads.size(); ++i) {
            const auto& t = p.threads[i];
            S(sec, "thread." + std::to_string(i),
              std::to_string(t.tid) + "|" + t.name + "|" + toString(t.state) + "|" +
                  std::to_string(t.cpuTicks));
        }
        S(sec, "mbox_count", std::to_string(p.mailbox.size()));
        for (size_t i = 0; i < p.mailbox.size(); ++i)
            S(sec, "mbox." + std::to_string(i), p.mailbox[i]);
    }
    // Services.
    for (const auto& [name, s] : svc_.all()) {
        std::string sec = "service." + name;
        S(sec, "node", s.node);
        S(sec, "binary", s.binary);
        S(sec, "config", s.config);
        S(sec, "policy", toString(s.policy));
        S(sec, "state", toString(s.state));
        S(sec, "pid", std::to_string(s.pid));
        S(sec, "crashes", std::to_string(s.crashCount));
        S(sec, "change", std::to_string(s.lastChange));
    }
    // Event ledger (ids are 1..N contiguous; causality rides along).
    {
        const auto& all = events_.all();
        S("ledger", "event_count", std::to_string(all.size()));
        for (const auto& e : all) {
            std::string sec = "event." + std::to_string(e.id);
            S(sec, "id", std::to_string(e.id));
            S(sec, "tick", std::to_string(e.tick));
            S(sec, "etype", e.type);
            S(sec, "source", e.source);
            S(sec, "target", e.target);
            S(sec, "cause", std::to_string(e.causeId));
            S(sec, "severity", e.severity);
            S(sec, "message", e.message);
            for (const auto& [k, v] : e.metadata) S(sec, "meta." + k, v);
        }
    }
    // Every generated section/key must survive a strict re-parse; user
    // controlled fragments (belief props, metadata keys) can otherwise
    // smuggle '=' or newlines into keys. Fail loudly instead of corrupting.
    for (const auto& s : doc.sections) {
        if (!validKey(s)) {
            err = "unrepresentable section in world state: '" + s + "'";
            return false;
        }
    }
    for (const auto& [s, keys] : doc.kv) {
        for (const auto& [k, v] : keys) {
            (void)v;
            if (!validKey(k)) {
                err = "unrepresentable key in world state: [" + s + "] '" + k + "'";
                return false;
            }
        }
    }
    return true;
}

bool System::deserializeWorldDoc(const ordc::OrdDoc& doc, std::string& err) {
    using ordc::getBool;
    using ordc::getDouble;
    using ordc::getInt;
    using ordc::getInt64;
    using ordc::getString;
    using ordc::getUint64;
    using ordc::requireSection;
    if (doc.type != "world") {
        err = "not a world document (type = " + doc.type + ")";
        return false;
    }
    ordc::OrdError oerr;
    oerr.file = "world:" + doc.id;
    auto fail = [&](const std::string& msg) {
        err = oerr.str();
        if (err.find(msg) == std::string::npos) err += " " + msg;
        return false;
    };
    // Build into a scratch world first: *this is untouched on failure.
    System t;
    try {
        std::string s;
        int64_t i64 = 0;
        uint64_t u64 = 0;
        int iv = 0;
        bool bv = false;
        double dv = 0.0;
        if (!getString(doc, "meta", "name", s, oerr)) return fail("");
        t.worldName_ = s;
        if (!getString(doc, "meta", "note", s, oerr)) return fail("");
        t.worldNote_ = s;
        if (!getUint64(doc, "meta", "created_tick", u64, oerr)) return fail("");
        t.worldCreatedTick_ = u64;
        if (!getUint64(doc, "meta", "saved_tick", u64, oerr)) return fail("");
        t.worldSavedTick_ = u64;
        if (!getInt64(doc, "uptime", "created_at", i64, oerr)) return fail("");
        t.worldCreatedAt_ = i64;
        if (!getUint64(doc, "uptime", "total_active_seconds", u64, oerr)) return fail("");
        t.uptimeTotalSecs_ = u64;
        t.worldId_ = doc.id;
        t.uptimeSessionStart_ = 0; // sessions never persist
        if (!getUint64(doc, "clock", "tick", u64, oerr)) return fail("");
        t.clock_.setTick(u64);
        if (!getBool(doc, "clock", "paused", bv, oerr)) return fail("");
        if (bv) t.clock_.pause();
        else t.clock_.resume();
        if (!getUint64(doc, "random", "seed", u64, oerr)) return fail("");
        t.seed_ = u64;
        if (!getUint64(doc, "random", "state", u64, oerr)) return fail("");
        t.rng_ = u64;
        if (!getBool(doc, "sim", "autofaults", bv, oerr)) return fail("");
        t.autoFaults_ = bv;
        if (!getInt(doc, "sim", "autorate", iv, oerr)) return fail("");
        if (iv < 0 || iv > 100) {
            oerr.section = "sim";
            oerr.key = "autorate";
            oerr.message = "autorate out of range";
            return fail("");
        }
        t.autoRate_ = iv;
        if (!getString(doc, "sim", "mode", s, oerr)) return fail("");
        t.mode_ = s;
        if (!getInt(doc, "sim", "nextpid", iv, oerr)) return fail("");
        t.pm_.setNextPid(iv);
        if (!getString(doc, "sim", "cwd", s, oerr)) return fail("");
        t.cwd_ = s; // validated against restored tree below
        if (doc.kv.count("operator")) {
            if (!getString(doc, "operator", "op_user", s, oerr)) return fail("");
            t.opUser_ = s;
            if (!getBool(doc, "operator", "op_root", bv, oerr)) return fail("");
            t.opRoot_ = bv;
        }
        // Hardware.
        {
            if (!requireSection(doc, "hardware", oerr)) return fail("");
            HardwareProfile p;
            if (!getString(doc, "hardware", "cpu_model", p.cpuModel, oerr)) return fail("");
            if (!getInt(doc, "hardware", "cpu_cores", p.cpuCores, oerr)) return fail("");
            if (!getInt(doc, "hardware", "ram_mb", p.ramMb, oerr)) return fail("");
            if (!getInt(doc, "hardware", "storage_mb", p.storageMb, oerr)) return fail("");
            std::string derr;
            if (!t.hw_.setProfile(p, derr)) {
                oerr.section = "hardware";
                oerr.message = "bad profile: " + derr;
                return fail("");
            }
            const char* res[][2] = {{"cpu", "backend_cpu"},
                                    {"ram", "backend_ram"},
                                    {"storage", "backend_storage"},
                                    {"net", "backend_net"}};
            for (auto [rn, kn] : res) {
                if (!getString(doc, "hardware", kn, s, oerr)) return fail("");
                Backend b;
                try {
                    b = backendFromString(s);
                } catch (const std::exception&) {
                    oerr.section = "hardware";
                    oerr.key = kn;
                    oerr.message = "unknown backend";
                    oerr.expected = "V|R";
                    oerr.actual = s;
                    return fail("");
                }
                if (!t.hw_.setBackend(rn, b, t.clock_.tickCount(), derr)) {
                    oerr.section = "hardware";
                    oerr.message = "bad backend: " + derr;
                    return fail("");
                }
            }
            if (!getString(doc, "hardware", "policy", s, oerr)) return fail("");
            try {
                t.hw_.setPolicy(overflowPolicyFromString(s), t.clock_.tickCount());
            } catch (const std::exception&) {
                oerr.section = "hardware";
                oerr.key = "policy";
                oerr.message = "unknown overflow policy";
                return fail("");
            }
            int capRam = 0, capStor = 0;
            if (!getInt(doc, "hardware", "cap_ram", capRam, oerr)) return fail("");
            if (!getInt(doc, "hardware", "cap_storage", capStor, oerr)) return fail("");
            if (!t.hw_.setBackingCaps(capRam, capStor, derr)) {
                oerr.section = "hardware";
                oerr.message = "bad backing caps: " + derr;
                return fail("");
            }
            if (!getInt(doc, "hardware", "blob_count", iv, oerr)) return fail("");
            for (int i = 0; i < iv; ++i) {
                std::string pre = "blob." + std::to_string(i);
                std::string key, res, bytes;
                if (!getString(doc, "hardware", pre + ".key", key, oerr)) return fail("");
                if (!getString(doc, "hardware", pre + ".res", res, oerr)) return fail("");
                if (!getString(doc, "hardware", pre + ".bytes", bytes, oerr)) return fail("");
                std::string serr;
                if (!t.hw_.storeOverflow(res.empty() ? "storage" : res, key, bytes, serr)) {
                    oerr.section = "hardware";
                    oerr.message = "bad blob: " + serr;
                    return fail("");
                }
            }
        }
        // VFS.
        {
            if (!requireSection(doc, "vfs", oerr)) return fail("");
            t.vfs_.clear();
            if (!getInt(doc, "vfs", "dir_count", iv, oerr)) return fail("");
            for (int i = 0; i < iv; ++i) {
                std::string pre = "dir." + std::to_string(i);
                std::string dpath;
                if (!getString(doc, "vfs", pre, dpath, oerr)) return fail("");
                if (dpath != "/") {
                    std::string downer;
                    if (!getString(doc, "vfs", pre + ".owner", downer, oerr)) return fail("");
                    int acount = 0;
                    if (!getInt(doc, "vfs", pre + ".acl_count", acount, oerr)) return fail("");
                    std::set<std::string> dacl;
                    for (int j = 0; j < acount; ++j) {
                        std::string tok;
                        if (!getString(doc, "vfs", pre + ".acl." + std::to_string(j), tok,
                                       oerr))
                            return fail("");
                        if (!validPermToken(tok)) {
                            oerr.section = "vfs";
                            oerr.message = "bad acl token";
                            return fail("");
                        }
                        dacl.insert(tok);
                    }
                    std::string derr;
                    if (!t.vfs_.mkdir(dpath, derr, downer, dacl)) {
                        oerr.section = "vfs";
                        oerr.message = "bad dir: " + derr;
                        return fail("");
                    }
                }
            }
            if (!getInt(doc, "vfs", "file_count", iv, oerr)) return fail("");
            for (int i = 0; i < iv; ++i) {
                std::string sec = "file." + std::to_string(i);
                if (!requireSection(doc, sec, oerr)) return fail("");
                std::string path, owner, mtimeS, content;
                int exec = 0, corrupt = 0;
                uint64_t mtime = 0;
                if (!getString(doc, sec, "path", path, oerr)) return fail("");
                if (!getString(doc, sec, "owner", owner, oerr)) return fail("");
                if (!getInt(doc, sec, "exec", exec, oerr) || (exec != 0 && exec != 1))
                    return fail("");
                if (!getInt(doc, sec, "corrupt", corrupt, oerr) ||
                    (corrupt != 0 && corrupt != 1))
                    return fail("");
                if (!getUint64(doc, sec, "mtime", mtime, oerr)) return fail("");
                if (!getString(doc, sec, "content", content, oerr)) return fail("");
                int acount = 0;
                if (!getInt(doc, sec, "acl_count", acount, oerr)) return fail("");
                std::vector<std::string> atoks;
                for (int j = 0; j < acount; ++j) {
                    std::string tok;
                    if (!getString(doc, sec, "acl." + std::to_string(j), tok, oerr))
                        return fail("");
                    if (!validPermToken(tok)) {
                        oerr.section = sec;
                        oerr.message = "bad acl token";
                        return fail("");
                    }
                    atoks.push_back(tok);
                }
                std::string derr;
                if (!t.vfs_.write(path, content, owner, mtime, derr, exec == 1)) {
                    oerr.section = sec;
                    oerr.message = "bad file: " + derr;
                    return fail("");
                }
                // Fresh writes carry defaults; restore the recorded ACL exactly.
                {
                    const VFile* wf = t.vfs_.file(path);
                    std::set<std::string> cur = wf ? wf->acl : std::set<std::string>{};
                    for (const auto& t0 : cur) {
                        if (std::find(atoks.begin(), atoks.end(), t0) == atoks.end())
                            t.vfs_.chmod(path, false, t0, derr);
                    }
                    for (const auto& t0 : atoks)
                        if (!cur.count(t0)) t.vfs_.chmod(path, true, t0, derr);
                }
                if (corrupt == 1 && !t.vfs_.corrupt(path, derr)) {
                    oerr.section = sec;
                    oerr.message = "bad file: " + derr;
                    return fail("");
                }
                (void)mtimeS;
            }
            if (!t.vfs_.isDir(t.cwd_)) {
                oerr.section = "sim";
                oerr.key = "cwd";
                oerr.message = "cwd not a directory in restored tree";
                return fail("");
            }
        }
        // Nodes.
        {
            t.nodes_.clear();
            for (const auto& sec : doc.sections) {
                if (sec.rfind("node.", 0) != 0) continue;
                std::string name = sec.substr(5);
                if (!isValidNodeName(name)) {
                    oerr.section = sec;
                    oerr.message = "bad node name";
                    return fail("");
                }
                Node n;
                n.name = name;
                auto req = [&](const char* k, std::string& out) {
                    return getString(doc, sec, k, out, oerr);
                };
                std::string vs;
                if (!req("type", vs)) return fail("");
                n.type = vs;
                if (!req("state", vs)) return fail("");
                try {
                    n.state = stateFromString(vs);
                } catch (const std::exception&) {
                    oerr.section = sec;
                    oerr.key = "state";
                    oerr.message = "unknown node state";
                    return fail("");
                }
                if (!getInt(doc, sec, "health", n.health, oerr)) return fail("");
                if (!getInt(doc, sec, "latency", n.latencyMs, oerr)) return fail("");
                if (!getInt(doc, sec, "base_latency", n.baseLatencyMs, oerr)) return fail("");
                if (!getInt(doc, sec, "load", n.load, oerr)) return fail("");
                if (!getInt(doc, sec, "memory_mb", n.memoryMb, oerr)) return fail("");
                if (!getInt(doc, sec, "memory_used", n.memoryUsedMb, oerr)) return fail("");
                if (!getInt(doc, sec, "connections", n.connections, oerr)) return fail("");
                if (!getInt(doc, sec, "max_connections", n.maxConnections, oerr))
                    return fail("");
                if (!getInt(doc, sec, "temp", n.tempC, oerr)) return fail("");
                if (!getInt(doc, sec, "ambient", n.ambientC, oerr)) return fail("");
                if (!getInt(doc, sec, "thermal_limit", n.thermalLimitC, oerr)) return fail("");
                if (!getInt(doc, sec, "critical", n.criticalC, oerr)) return fail("");
                if (!getBool(doc, sec, "throttled", n.throttled, oerr)) return fail("");
                if (!getInt(doc, sec, "freq", n.freqMHz, oerr)) return fail("");
                if (!getInt(doc, sec, "base_freq", n.baseFreqMHz, oerr)) return fail("");
                if (!getInt(doc, sec, "max_freq", n.maxFreqMHz, oerr)) return fail("");
                if (!req("clock", vs)) return fail("");
                try {
                    n.clockState = clockStateFromString(vs);
                } catch (const std::exception&) {
                    oerr.section = sec;
                    oerr.key = "clock";
                    oerr.message = "unknown clock state";
                    return fail("");
                }
                if (!getBool(doc, sec, "mem_corrupt", n.memCorrupt, oerr)) return fail("");
                if (!getInt(doc, sec, "leak", n.leakMbPerTick, oerr)) return fail("");
                if (!getInt(doc, sec, "swap", n.swapMb, oerr)) return fail("");
                if (!getInt(doc, sec, "swap_used", n.swapUsedMb, oerr)) return fail("");
                if (!getInt(doc, sec, "oom_kills", n.oomKills, oerr)) return fail("");
                if (!getInt(doc, sec, "storage_mb", n.storageMb, oerr)) return fail("");
                if (!getInt(doc, sec, "storage_used", n.storageUsedMb, oerr)) return fail("");
                if (!getInt(doc, sec, "io", n.ioLoad, oerr)) return fail("");
                {
                    auto it = doc.kv.find(sec);
                    for (const auto& [k, v] : it->second) {
                        if (k.rfind("fs.", 0) == 0) {
                            try {
                                n.filesystems[k.substr(3)] = fsStateFromString(v);
                            } catch (const std::exception&) {
                                oerr.section = sec;
                                oerr.key = k;
                                oerr.message = "unknown fs state";
                                return fail("");
                            }
                        } else if (k.rfind("module.", 0) == 0) {
                            n.modules[k.substr(7)] = v;
                        } else if (k.rfind("meta.", 0) == 0) {
                            n.metadata[k.substr(5)] = v;
                        } else if (k.rfind("belief.", 0) == 0) {
                            std::string rest = k.substr(7);
                            auto dot = rest.find('.');
                            if (dot == std::string::npos) {
                                oerr.section = sec;
                                oerr.key = k;
                                oerr.message = "bad belief key";
                                return fail("");
                            }
                            n.beliefs[rest.substr(0, dot)][rest.substr(dot + 1)] = v;
                        } else if (k.rfind("fault.", 0) == 0) {
                            // fault.<name>.<field>: grouped below.
                        }
                    }
                }
                if (!req("kernel", vs)) return fail("");
                try {
                    n.kernel = kernelStateFromString(vs);
                } catch (const std::exception&) {
                    oerr.section = sec;
                    oerr.key = "kernel";
                    oerr.message = "unknown kernel state";
                    return fail("");
                }
                if (!getInt(doc, sec, "instability", n.instability, oerr)) return fail("");
                int binCount = 0;
                if (!getInt(doc, sec, "bin_count", binCount, oerr)) return fail("");
                for (int i = 0; i < binCount; ++i) {
                    if (!getString(doc, sec, "bin." + std::to_string(i), vs, oerr))
                        return fail("");
                    n.binaries.insert(vs);
                }
                int depCount = 0;
                if (!getInt(doc, sec, "dep_count", depCount, oerr)) return fail("");
                for (int i = 0; i < depCount; ++i) {
                    if (!getString(doc, sec, "dep." + std::to_string(i), vs, oerr))
                        return fail("");
                    n.dependencies.push_back(vs);
                }
                if (!req("proc", vs)) return fail("");
                n.proc = vs;
                if (!req("priv", vs)) return fail("");
                try {
                    n.priv = privStateFromString(vs);
                } catch (const std::exception&) {
                    oerr.section = sec;
                    oerr.key = "priv";
                    oerr.message = "unknown privilege state";
                    return fail("");
                }
                if (!getBool(doc, sec, "config_corrupt", n.configCorrupt, oerr)) return fail("");
                // Faults: collect distinct names first (deterministic).
                {
                    auto it = doc.kv.find(sec);
                    std::vector<std::string> fnames;
                    for (const auto& [k, v] : it->second) {
                        (void)v;
                        if (k.rfind("fault.", 0) != 0) continue;
                        std::string rest = k.substr(6);
                        auto dot = rest.find('.');
                        if (dot == std::string::npos) continue;
                        std::string fn = rest.substr(0, dot);
                        if (std::find(fnames.begin(), fnames.end(), fn) == fnames.end())
                            fnames.push_back(fn);
                    }
                    for (const auto& fn : fnames) {
                        ActiveFault af;
                        af.name = fn;
                        std::string pre = "fault." + fn;
                        if (!getString(doc, sec, pre + ".kind", vs, oerr)) return fail("");
                        try {
                            af.kind = faultKindFromString(vs);
                        } catch (const std::exception&) {
                            oerr.section = sec;
                            oerr.key = pre + ".kind";
                            oerr.message = "unknown fault kind";
                            return fail("");
                        }
                        if (!getString(doc, sec, pre + ".sev", vs, oerr)) return fail("");
                        try {
                            af.severity = severityFromString(vs);
                        } catch (const std::exception&) {
                            oerr.section = sec;
                            oerr.key = pre + ".sev";
                            oerr.message = "unknown severity";
                            return fail("");
                        }
                        if (!getUint64(doc, sec, pre + ".since", af.sinceTick, oerr))
                            return fail("");
                        if (!getUint64(doc, sec, pre + ".event", af.eventId, oerr))
                            return fail("");
                        if (!getString(doc, sec, pre + ".detail", af.detail, oerr))
                            return fail("");
                        n.faults[fn] = af;
                    }
                }
                t.nodes_[name] = n;
            }
            if (t.nodes_.empty()) {
                oerr.message = "no [node.*] sections";
                return fail("");
            }
        }
        // Links.
        {
            if (!requireSection(doc, "net", oerr)) return fail("");
            if (!getInt(doc, "net", "link_count", iv, oerr)) return fail("");
            t.net_.clear();
            for (int i = 0; i < iv; ++i) {
                std::string pre = "link." + std::to_string(i);
                std::string a, b;
                bool up = false;
                int lat = 0;
                if (!getString(doc, "net", pre + ".a", a, oerr)) return fail("");
                if (!getString(doc, "net", pre + ".b", b, oerr)) return fail("");
                if (!getBool(doc, "net", pre + ".up", up, oerr)) return fail("");
                if (!getInt(doc, "net", pre + ".lat", lat, oerr)) return fail("");
                if (!getDouble(doc, "net", pre + ".loss", dv, oerr)) return fail("");
                double loss = dv;
                if (!getDouble(doc, "net", pre + ".load", dv, oerr)) return fail("");
                double load = dv;
                t.net_.ensureLink(a, b, lat);
                t.net_.setUp(a, b, up);
                t.net_.setLoss(a, b, loss);
                t.net_.setLatency(a, b, lat);
                if (Link* m = t.net_.find(a, b)) m->loadPct = load;
            }
        }
        // Processes.
        {
            std::map<int, SimProcess> procs;
            for (const auto& sec : doc.sections) {
                if (sec.rfind("proc.", 0) != 0) continue;
                int pid = 0;
                try {
                    pid = std::stoi(sec.substr(5));
                } catch (...) {
                    oerr.section = sec;
                    oerr.message = "bad proc section";
                    return fail("");
                }
                SimProcess p;
                p.pid = pid;
                if (!getString(doc, sec, "name", p.name, oerr)) return fail("");
                if (!getString(doc, sec, "ptype", p.type, oerr)) return fail("");
                if (!getString(doc, sec, "state", s, oerr)) return fail("");
                try {
                    p.state = procStateFromString(s);
                } catch (const std::exception&) {
                    oerr.section = sec;
                    oerr.key = "state";
                    oerr.message = "unknown proc state";
                    return fail("");
                }
                if (!getInt(doc, sec, "cpu", p.cpu, oerr)) return fail("");
                if (!getInt(doc, sec, "mem", p.memMb, oerr)) return fail("");
                if (!getInt(doc, sec, "priority", p.priority, oerr)) return fail("");
                if (!getInt(doc, sec, "parent", p.parentPid, oerr)) return fail("");
                if (!getString(doc, sec, "host", p.host, oerr)) return fail("");
                if (!getUint64(doc, sec, "runtime", p.runtime, oerr)) return fail("");
                if (!getInt(doc, sec, "thread_seq", p.threadSeq, oerr)) return fail("");
                int tcount = 0, mcount = 0;
                if (!getInt(doc, sec, "thread_count", tcount, oerr)) return fail("");
                for (int i = 0; i < tcount; ++i) {
                    if (!getString(doc, sec, "thread." + std::to_string(i), s, oerr))
                        return fail("");
                    // tid|name|state|cpuTicks (names cannot contain '|').
                    size_t p1 = s.find('|'), p2 = s.find('|', p1 + 1), p3 = s.find('|', p2 + 1);
                    if (p1 == std::string::npos || p2 == std::string::npos ||
                        p3 == std::string::npos) {
                        oerr.section = sec;
                        oerr.message = "bad thread record";
                        return fail("");
                    }
                    SimThread th;
                    try {
                        th.tid = std::stoi(s.substr(0, p1));
                        th.cpuTicks = (uint64_t)std::stoull(s.substr(p3 + 1));
                    } catch (...) {
                        oerr.section = sec;
                        oerr.message = "bad thread record";
                        return fail("");
                    }
                    th.name = s.substr(p1 + 1, p2 - p1 - 1);
                    try {
                        th.state = procStateFromString(s.substr(p2 + 1, p3 - p2 - 1));
                    } catch (const std::exception&) {
                        oerr.section = sec;
                        oerr.message = "bad thread record";
                        return fail("");
                    }
                    p.threads.push_back(th);
                }
                if (!getInt(doc, sec, "mbox_count", mcount, oerr)) return fail("");
                for (int i = 0; i < mcount; ++i) {
                    if (!getString(doc, sec, "mbox." + std::to_string(i), s, oerr))
                        return fail("");
                    p.mailbox.push_back(s);
                }
                procs[pid] = p;
            }
            int nextPid = 101;
            {
                auto it = doc.kv.find("sim");
                if (it == doc.kv.end()) {
                    oerr.section = "sim";
                    oerr.message = "missing required section";
                    return fail("");
                }
                // nextpid already read into t above; reuse via sim section.
                std::string nps;
                if (!getString(doc, "sim", "nextpid", nps, oerr)) return fail("");
                try {
                    nextPid = std::stoi(nps);
                } catch (...) {
                    oerr.section = "sim";
                    oerr.key = "nextpid";
                    oerr.message = "invalid integer";
                    return fail("");
                }
            }
            t.pm_.restoreSnapshot(procs, nextPid);
        }
        // Services.
        {
            std::map<std::string, Service> svcs;
            for (const auto& sec : doc.sections) {
                if (sec.rfind("service.", 0) != 0) continue;
                std::string name = sec.substr(8);
                Service sv;
                sv.name = name;
                if (!getString(doc, sec, "node", sv.node, oerr)) return fail("");
                if (!getString(doc, sec, "binary", sv.binary, oerr)) return fail("");
                if (!getString(doc, sec, "config", sv.config, oerr)) return fail("");
                if (!getString(doc, sec, "policy", s, oerr)) return fail("");
                try {
                    sv.policy = servicePolicyFromString(s);
                } catch (const std::exception&) {
                    oerr.section = sec;
                    oerr.key = "policy";
                    oerr.message = "unknown service policy";
                    return fail("");
                }
                if (!getString(doc, sec, "state", s, oerr)) return fail("");
                try {
                    sv.state = serviceStateFromString(s);
                } catch (const std::exception&) {
                    oerr.section = sec;
                    oerr.key = "state";
                    oerr.message = "unknown service state";
                    return fail("");
                }
                if (!getInt(doc, sec, "pid", sv.pid, oerr)) return fail("");
                if (!getInt(doc, sec, "crashes", sv.crashCount, oerr)) return fail("");
                if (!getUint64(doc, sec, "change", sv.lastChange, oerr)) return fail("");
                svcs[name] = sv;
            }
            t.svc_.restoreSnapshot(svcs);
        }
        // Event ledger (ids must arrive 1..N in order).
        {
            if (!requireSection(doc, "ledger", oerr)) return fail("");
            if (!getInt(doc, "ledger", "event_count", iv, oerr)) return fail("");
            t.events_.clear();
            for (int i = 1; i <= iv; ++i) {
                std::string sec = "event." + std::to_string(i);
                if (!requireSection(doc, sec, oerr)) return fail("");
                int id = 0;
                uint64_t tick = 0, cause = 0;
                std::string type, source, target, severity, message;
                if (!getInt(doc, sec, "id", id, oerr)) return fail("");
                if (id != i) {
                    oerr.section = sec;
                    oerr.key = "id";
                    oerr.message = "event ids must be 1..N in order";
                    return fail("");
                }
                if (!getUint64(doc, sec, "tick", tick, oerr)) return fail("");
                if (!getString(doc, sec, "etype", type, oerr)) return fail("");
                if (!getString(doc, sec, "source", source, oerr)) return fail("");
                if (!getString(doc, sec, "target", target, oerr)) return fail("");
                if (!getUint64(doc, sec, "cause", cause, oerr)) return fail("");
                if (!getString(doc, sec, "severity", severity, oerr)) return fail("");
                if (!getString(doc, sec, "message", message, oerr)) return fail("");
                std::map<std::string, std::string> meta;
                auto it = doc.kv.find(sec);
                if (it != doc.kv.end()) {
                    for (const auto& [k, v] : it->second) {
                        if (k.rfind("meta.", 0) == 0) meta[k.substr(5)] = v;
                    }
                }
                t.events_.emit(tick, type, source, target, cause, message, meta, severity);
            }
        }
        std::string dia;
        if (!t.validateAll(&dia)) {
            err = oerr.file + ":0: restored world violates invariants: " + dia;
            return false;
        }
    } catch (const std::exception& e) {
        err = oerr.file + ":0: load failed: " + e.what();
        return false;
    }
    *this = std::move(t);
    syscallCounts_.clear(); // observational only; a loaded world starts counting fresh
    return true;
}

} // namespace override
