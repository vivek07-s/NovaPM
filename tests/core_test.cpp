// Tests for config, container detection, database history and (on Linux) the real /proc back end.
#include "config.hpp"
#include "database.hpp"
#include "process_collector.hpp"
#include "signal_manager.hpp"
#include "system_monitor.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#ifdef __linux__
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

static int g_pass = 0, g_fail = 0;
#define CHECK(name, cond) do { if (cond) { ++g_pass; std::printf("  PASS  %s\n", name); } \
                               else { ++g_fail; std::printf("  FAIL  %s   (%s:%d)\n", name, __FILE__, __LINE__); } } while (0)
namespace fs = std::filesystem;

static void testConfig() {
    std::puts("C1  config defaults, round trip, validation");
    AppConfig d;
    CHECK("default alert/stop thresholds are 60 / 80", d.guard.warnPct == 60.0 && d.guard.stopPct == 80.0);
    const std::string path = (fs::temp_directory_path() / "novapm_cfg_test" / "sub" / "n.ini").string();
    fs::remove_all(fs::temp_directory_path() / "novapm_cfg_test");

    AppConfig missing; std::string warn;
    CHECK("missing file loads defaults", loadConfig(path, missing, &warn) && missing.guard.warnPct == 60.0 && warn.empty());

    AppConfig c; c.guard.warnPct = 55; c.guard.stopPct = 90; c.guard.stopSustain = 5; c.guard.autoStop = false;
    c.guard.normalizeCpu = true; c.refreshMs = 500; c.dbEverySec = 9; c.dbPath = "x.db";
    std::string err;
    CHECK("save creates missing directories", saveConfig(path, c, &err));
    AppConfig r; loadConfig(path, r, &warn);
    CHECK("round trip keeps thresholds", r.guard.warnPct == 55 && r.guard.stopPct == 90 && r.guard.stopSustain == 5);
    CHECK("round trip keeps flags and paths", !r.guard.autoStop && r.guard.normalizeCpu && r.refreshMs == 500 && r.dbEverySec == 9 && r.dbPath == "x.db");

    std::ofstream(path) << "# comment\nwarn_pct = 70\nstop_pct = 40\nbogus = 1\nauto_stop = maybe\nnot a line\nstop_sustain = 0\n";
    AppConfig bad; loadConfig(path, bad, &warn);
    CHECK("unknown key / bad value / bad line are reported", warn.find("bogus") != std::string::npos && warn.find("maybe") != std::string::npos && warn.find("expected key") != std::string::npos);
    CHECK("stop is forced above alert (70 / 40 -> 70 / 71)", bad.guard.warnPct == 70 && bad.guard.stopPct == 71);
    CHECK("sustain is clamped to >= 1", bad.guard.stopSustain == 1);

    AppConfig s; std::string e2;
    CHECK("set bool 'off'", setConfigValue(s, "auto_stop", "off", e2) && !s.guard.autoStop);
    CHECK("set rejects unknown key", !setConfigValue(s, "nope", "1", e2));
    CHECK("set rejects non-number", !setConfigValue(s, "warn_pct", "abc", e2));
    fs::remove_all(fs::temp_directory_path() / "novapm_cfg_test");
}

static void testContainers() {
    std::puts("C2  container detection from cgroup text");
    const std::string id = "3f2a9c1d7b44e0a5c8d1f6b2a9e4c7d03f2a9c1d7b44e0a5c8d1f6b2a9e4c7d0";
    CHECK("id fixture is 64 hex", id.size() == 64);
    CHECK("docker cgroup v1", containerFromCgroup("12:cpu,cpuacct:/docker/" + id + "\n") == "docker:3f2a9c1d7b44");
    CHECK("docker cgroup v2 scope", containerFromCgroup("0::/system.slice/docker-" + id + ".scope\n") == "docker:3f2a9c1d7b44");
    CHECK("kubernetes containerd", containerFromCgroup("0::/kubepods.slice/kubepods-pod1.slice/cri-containerd-" + id + ".scope") == "containerd:3f2a9c1d7b44");
    CHECK("cri-o", containerFromCgroup("0::/kubepods/crio-" + id + ".scope") == "crio:3f2a9c1d7b44");
    CHECK("podman", containerFromCgroup("0::/user.slice/libpod-" + id + ".scope") == "podman:3f2a9c1d7b44");
    CHECK("host process has no container", containerFromCgroup("0::/user.slice/user-1000.slice/session-3.scope\n").empty());
    CHECK("empty text", containerFromCgroup("").empty());
    CHECK("63 hex chars is not an id", containerFromCgroup("/docker/" + id.substr(1)).empty());
}

static void testDatabase() {
    std::puts("C3  database: history, containers, alerts, read-only SQL");
    const std::string path = (fs::temp_directory_path() / "novapm_db_test.db").string();
    for (auto sfx : {"", "-wal", "-shm"}) fs::remove(path + sfx);
    {
        Database db(path);
        CHECK("opens", db.ok());
        SysStats sys; sys.mem_total_kb = 1000;
        for (int i = 0; i < 3; ++i) {
            ProcInfo p; p.pid = 42; p.name = "demo"; p.user = "u"; p.cpu = 10.0 * (i + 1); p.rss_kb = 2048 * (i + 1); p.threads = 2; p.state = 'R'; p.container = "docker:abc";
            ProcInfo q; q.pid = 43; q.name = "other"; q.cpu = 1;
            db.insertSnapshot({p, q}, sys);
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));   // distinct second-resolution timestamps
        }
        auto h = db.processHistory(42, 60);
        CHECK("history has 3 samples for pid 42 only", h.size() == 3);
        CHECK("history is oldest-first with right values", h.size() == 3 && h[0].cpu == 10.0 && h[2].cpu == 30.0 && h[2].rss_kb == 6144 && h[0].ts < h[2].ts);
        CHECK("history of unknown pid is empty", db.processHistory(999, 60).empty());
        CHECK("history window excludes old samples", db.processHistory(42, 0).size() <= 1);
        auto m = db.query("SELECT container FROM process_meta WHERE pid=42");
        CHECK("container stored in process_meta", m.error.empty() && m.rows.size() == 1 && m.rows[0][0] == "docker:abc");

        db.logAlert(42, "demo", "CPU", "warn", 63.0, 60, "alert raised");
        db.logAlert(42, "demo", "CPU", "stop", 91.0, 80, "suspended");
        auto a = db.query("SELECT action, COUNT(*) FROM alerts GROUP BY action ORDER BY action");
        CHECK("alerts table is queryable", a.rows.size() == 2 && a.rows[0][0] == "stop" && a.rows[1][0] == "warn");
        CHECK("DELETE rejected", !db.query("DELETE FROM alerts").error.empty());
        CHECK("INSERT rejected", !db.query("INSERT INTO events(ts) VALUES(1)").error.empty());
        CHECK("DROP rejected", !db.query("DROP TABLE alerts").error.empty());
        CHECK("alerts survive the rejected writes", db.query("SELECT * FROM alerts").rows.size() == 2);
    }
    {   // reopening an existing file keeps data (upgrade path: container column already present)
        Database db(path);
        CHECK("reopen keeps history", db.ok() && db.processHistory(42, 60).size() == 3);
    }
    for (auto sfx : {"", "-wal", "-shm"}) fs::remove(path + sfx);
}

#ifdef __linux__
static const ProcInfo* find(const std::vector<ProcInfo>& v, int pid) { for (auto& p : v) if (p.pid == pid) return &p; return nullptr; }

static void testLinuxBackend() {
    std::puts("C4  Linux /proc back end (real processes)");
    ProcessCollector pc;
    pc.collect();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto v = pc.collect();
    const ProcInfo* me = find(v, (int)getpid());
    CHECK("collector sees its own process", me != nullptr);
    CHECK("own name, user, memory, threads are filled", me && !me->name.empty() && !me->user.empty() && me->rss_kb > 0 && me->threads >= 1);
    CHECK("own state is running/sleeping", me && (me->state == 'R' || me->state == 'S'));
    CHECK("init (PID 1) is listed", find(v, 1) != nullptr);

    ProcDetails d;
    CHECK("details(self) succeeds", ProcessCollector::details((int)getpid(), d));
    CHECK("details: exe, cmdline, start time, fd count", !d.exe.empty() && d.exe[0] == '/' && !d.cmdline.empty() && !d.started.empty() && d.handles > 0);
    CHECK("details: nice value text", d.priority.rfind("nice ", 0) == 0);
    CHECK("details(nonexistent pid) fails", !ProcessCollector::details(2147483000, d));

    SystemMonitor sm; sm.sample(0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    SysStats s = sm.sample((int)v.size(), 0);
    CHECK("system: RAM, cores, uptime", s.mem_total_kb > 0 && s.mem_used_kb > 0 && s.mem_used_kb <= s.mem_total_kb && s.cores >= 1 && s.uptime > 0);
    CHECK("system: CPU percent in range", s.cpu >= 0 && s.cpu <= 100.0);

    // real signals on a real child process
    pid_t child = fork();
    if (child == 0) { execlp("sleep", "sleep", "30", (char*)nullptr); _exit(127); }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK("refuses PID 1", !SignalManager::send(1, NPM_SIGKILL).ok);
    CHECK("refuses itself", !SignalManager::send((int)getpid(), NPM_SIGSTOP).ok);
    CHECK("SIGSTOP accepted", SignalManager::send(child, NPM_SIGSTOP).ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto st = pc.collect(); const ProcInfo* c = find(st, child);
    CHECK("collector reports state T for the stopped child", c && c->state == 'T');
    CHECK("SIGCONT accepted", SignalManager::send(child, NPM_SIGCONT).ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    st = pc.collect(); c = find(st, child);
    CHECK("child is running again", c && c->state != 'T');
    CHECK("SIGKILL accepted", SignalManager::send(child, NPM_SIGKILL).ok);
    int status = 0; waitpid(child, &status, 0);
    SignalResult gone = SignalManager::send(child, NPM_SIGTERM);
    CHECK("signalling an exited process reports 'already exited'", !gone.ok && gone.msg.find("already exited") != std::string::npos);
}
#endif

int main() {
    testConfig();
    testContainers();
    testDatabase();
#ifdef __linux__
    testLinuxBackend();
#endif
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
