#include "cli.hpp"
#include "config.hpp"
#include "database.hpp"
#include "monitor_core.hpp"
#include "process_collector.hpp"
#include "signal_manager.hpp"
#include "system_monitor.hpp"
#include "threshold_guard.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
std::atomic<bool> g_stop{false};
void onSigint(int) { g_stop = true; }

const char* kCommands[] = {"top", "watch", "history", "alerts", "query", "signal", "containers", "config", "help", "--help", "-h", "version", "--version"};

// "--key value" / "--flag" parser. Positional arguments are collected in order.
struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::string> opt;
    bool has(const std::string& k) const { return opt.count(k) > 0; }
    std::string get(const std::string& k, const std::string& def = "") const { auto i = opt.find(k); return i == opt.end() ? def : i->second; }
    double num(const std::string& k, double def) const { return has(k) ? std::atof(opt.at(k).c_str()) : def; }
};
const char* kFlagsWithoutValue[] = {"no-stop", "quiet", "mem"};
Args parseArgs(int argc, char** argv, int from) {
    Args a;
    for (int i = from; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            std::string k = s.substr(2);
            bool flag = false;
            for (auto f : kFlagsWithoutValue) if (k == f) flag = true;
            if (flag || i + 1 >= argc) a.opt[k] = "1"; else a.opt[k] = argv[++i];
        } else a.pos.push_back(s);
    }
    return a;
}

void usage() {
    std::puts(
"NovaPM command-line interface\n"
"\n"
"  novapm-cli top [--n 15] [--mem] [--interval 1000]     live-sampled process list (sorted by CPU, or --mem)\n"
"  novapm-cli watch [options]                             headless monitor with the threshold guard\n"
"        --warn 60 --stop 80 --sustain 3 --no-stop        alert threshold, stop threshold, samples, alert-only mode\n"
"        --interval 1000 --duration 0 --quiet             sample interval (ms), run time in s (0 = until Ctrl+C)\n"
"  novapm-cli history PID [--minutes 60]                  stored CPU / memory samples of one process\n"
"  novapm-cli alerts [--limit 20]                         recent alerts and automatic stops\n"
"  novapm-cli query \"SELECT ...\"                          read-only SQL against the database\n"
"  novapm-cli signal PID term|stop|cont|kill              send a signal (logged in the events table)\n"
"  novapm-cli containers                                  processes grouped by container (Linux cgroups)\n"
"  novapm-cli config [set KEY VALUE]                      show / change the settings file\n"
"\n"
"Global options:  --db FILE   database (default from config, novapm.db)    --config FILE   settings file\n"
"Exit codes: 0 ok, 1 runtime error, 2 usage error.");
}

struct Ctx { AppConfig cfg; std::string cfgPath; };
bool loadCtx(const Args& a, Ctx& c) {
    c.cfgPath = a.get("config", defaultConfigPath());
    std::string warn;
    loadConfig(c.cfgPath, c.cfg, &warn);
    if (!warn.empty()) std::fprintf(stderr, "config warnings (%s):\n%s", c.cfgPath.c_str(), warn.c_str());
    if (a.has("db")) c.cfg.dbPath = a.get("db");
    return true;
}

std::string fmtTime(long long ts) {
    char b[32]; std::time_t t = (std::time_t)ts; std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::strftime(b, sizeof b, "%H:%M:%S", &tm);
    return b;
}

void printTable(const QueryResult& r) {
    std::vector<size_t> w(r.cols.size());
    for (size_t i = 0; i < r.cols.size(); ++i) w[i] = r.cols[i].size();
    for (auto& row : r.rows) for (size_t i = 0; i < row.size(); ++i) w[i] = std::min<size_t>(std::max(w[i], row[i].size()), 48);
    for (size_t i = 0; i < r.cols.size(); ++i) std::printf("%-*s  ", (int)w[i], r.cols[i].c_str());
    std::puts("");
    for (size_t i = 0; i < r.cols.size(); ++i) std::printf("%s  ", std::string(w[i], '-').c_str());
    std::puts("");
    for (auto& row : r.rows) {
        for (size_t i = 0; i < row.size(); ++i) std::printf("%-*.*s  ", (int)w[i], (int)w[i], row[i].c_str());
        std::puts("");
    }
    std::printf("(%zu row%s)\n", r.rows.size(), r.rows.size() == 1 ? "" : "s");
}

// ---------------------------------------------------------------- commands
int cmdTop(const Args& a) {
    ProcessCollector pc; SystemMonitor sm;
    pc.collect();                                                   // first sample primes the CPU counters
    sm.sample(0, 0);
    const int interval = (int)a.num("interval", 1000);
    for (int slept = 0; slept < interval && !g_stop; slept += 50) std::this_thread::sleep_for(50ms);
    auto procs = pc.collect();
    int threads = 0; for (auto& p : procs) threads += p.threads;
    SysStats s = sm.sample((int)procs.size(), threads);
    const bool byMem = a.has("mem");
    std::sort(procs.begin(), procs.end(), [&](const ProcInfo& x, const ProcInfo& y) {
        return byMem ? x.rss_kb > y.rss_kb : x.cpu > y.cpu; });
    std::printf("CPU %.1f%%  RAM %.0f/%.0f MB  Swap %.0f/%.0f MB  Procs %d  Threads %d  Uptime %dh %dm\n",
        s.cpu, s.mem_used_kb / 1024.0, s.mem_total_kb / 1024.0, s.swap_used_kb / 1024.0, s.swap_total_kb / 1024.0,
        s.procs, s.threads, (int)s.uptime / 3600, ((int)s.uptime % 3600) / 60);
    std::printf("%7s  %-22s %-12s %s %7s %9s %4s  %s\n", "PID", "NAME", "USER", "S", "CPU%", "MEM(MB)", "THR", "CONTAINER");
    const int n = (int)a.num("n", 15);
    for (int i = 0; i < (int)procs.size() && i < n; ++i) {
        const auto& p = procs[i];
        std::printf("%7d  %-22.22s %-12.12s %c %7.1f %9.1f %4d  %s\n", p.pid, p.name.c_str(), p.user.c_str(), p.state, p.cpu, p.rss_kb / 1024.0, p.threads, p.container.c_str());
    }
    return 0;
}

int cmdWatch(const Args& a, Ctx& c) {
    GuardConfig& g = c.cfg.guard;
    if (a.has("warn")) g.warnPct = a.num("warn", g.warnPct);
    if (a.has("stop")) g.stopPct = a.num("stop", g.stopPct);
    if (a.has("sustain")) g.stopSustain = (int)a.num("sustain", g.stopSustain);
    if (a.has("no-stop")) g.autoStop = false;
    if (a.has("interval")) c.cfg.refreshMs = (int)a.num("interval", c.cfg.refreshMs);
    sanitizeConfig(c.cfg);
    const double duration = a.num("duration", 0);
    const bool quiet = a.has("quiet");

    Database db(c.cfg.dbPath);
    if (!db.ok()) { std::fprintf(stderr, "cannot open database '%s': %s\n", c.cfg.dbPath.c_str(), db.error().c_str()); return 1; }
    std::signal(SIGINT, onSigint);
    std::signal(SIGTERM, onSigint);

    std::printf("NovaPM watch: alert >= %.0f%%, %s >= %.0f%% for %d samples, every %d ms, db=%s\n",
        g.warnPct, g.autoStop ? "stop (suspend)" : "no auto-stop; would-stop", g.stopPct, g.stopSustain, c.cfg.refreshMs, c.cfg.dbPath.c_str());
    std::fflush(stdout);

    ProcessCollector pc; SystemMonitor sm; ThresholdGuard guard;
    std::map<int, std::string> prev; bool first = true;
    auto start = std::chrono::steady_clock::now();
    auto lastDb = start - 1h;
    int alerts = 0, stops = 0;

    while (!g_stop) {
        auto procs = pc.collect();
        int threads = 0; for (auto& p : procs) threads += p.threads;
        SysStats sys = sm.sample((int)procs.size(), threads);

        std::map<int, std::string> cur;
        for (auto& p : procs) cur[p.pid] = p.name;
        if (!first && c.cfg.dbEnabled) logLifecycle(prev, cur, db);
        prev = std::move(cur); first = false;

        auto now = std::chrono::steady_clock::now();
        for (const GuardEvent& ev : guard.evaluate(procs, sys, g, now)) {
            AlertRec r = applyGuardAction(ev, db);
            ++alerts; if (r.level == "AUTO-STOP") ++stops;
            if (!quiet) { std::printf("[%s] %-11s %s\n", r.time.c_str(), r.level.c_str(), r.msg.c_str()); std::fflush(stdout); }
        }
        if (c.cfg.dbEnabled && now - lastDb >= std::chrono::seconds(c.cfg.dbEverySec)) { db.insertSnapshot(procs, sys); lastDb = now; }

        if (duration > 0 && std::chrono::duration<double>(now - start).count() >= duration) break;
        for (int slept = 0; slept < c.cfg.refreshMs && !g_stop; slept += 50) std::this_thread::sleep_for(50ms);
    }
    std::printf("NovaPM watch finished: %d alert event(s), %d automatic stop(s)\n", alerts, stops);
    return 0;
}

int cmdHistory(const Args& a, Ctx& c) {
    if (a.pos.size() < 2) { std::fputs("usage: novapm-cli history PID [--minutes 60]\n", stderr); return 2; }
    const int pid = std::atoi(a.pos[1].c_str());
    Database db(c.cfg.dbPath);
    if (!db.ok()) { std::fprintf(stderr, "cannot open database: %s\n", db.error().c_str()); return 1; }
    auto pts = db.processHistory(pid, (int)a.num("minutes", 60) * 60);
    if (pts.empty()) { std::printf("No stored samples for PID %d in that period.\n", pid); return 0; }
    std::printf("%-9s %7s %10s  graph (CPU %%, each # = 5)\n", "TIME", "CPU%", "MEM(MB)");
    for (auto& p : pts) {
        std::string bar((size_t)std::min(20.0, p.cpu / 5.0), '#');
        std::printf("%-9s %7.1f %10.1f  %s\n", fmtTime(p.ts).c_str(), p.cpu, p.rss_kb / 1024.0, bar.c_str());
    }
    double mx = 0, sum = 0; for (auto& p : pts) { mx = std::max(mx, p.cpu); sum += p.cpu; }
    std::printf("%zu samples, average CPU %.1f%%, peak CPU %.1f%%\n", pts.size(), sum / pts.size(), mx);
    return 0;
}

int cmdAlerts(const Args& a, Ctx& c) {
    Database db(c.cfg.dbPath);
    if (!db.ok()) { std::fprintf(stderr, "cannot open database: %s\n", db.error().c_str()); return 1; }
    int limit = (int)a.num("limit", 20);
    auto r = db.query("SELECT datetime(ts,'unixepoch','localtime') AS time, pid, name, metric, action, ROUND(value,1) AS value_pct, threshold, result "
                      "FROM alerts ORDER BY ts DESC, id DESC LIMIT " + std::to_string(limit));
    if (!r.error.empty()) { std::fprintf(stderr, "%s\n", r.error.c_str()); return 1; }
    printTable(r);
    return 0;
}

int cmdQuery(const Args& a, Ctx& c) {
    if (a.pos.size() < 2) { std::fputs("usage: novapm-cli query \"SELECT ...\"\n", stderr); return 2; }
    Database db(c.cfg.dbPath);
    if (!db.ok()) { std::fprintf(stderr, "cannot open database: %s\n", db.error().c_str()); return 1; }
    auto r = db.query(a.pos[1], 1000);
    if (!r.error.empty()) { std::fprintf(stderr, "error: %s\n", r.error.c_str()); return 1; }
    printTable(r);
    return 0;
}

int cmdSignal(const Args& a, Ctx& c) {
    if (a.pos.size() < 3) { std::fputs("usage: novapm-cli signal PID term|stop|cont|kill\n", stderr); return 2; }
    const int pid = std::atoi(a.pos[1].c_str());
    const std::string w = a.pos[2];
    int sig = w == "term" ? NPM_SIGTERM : w == "stop" ? NPM_SIGSTOP : w == "cont" ? NPM_SIGCONT : w == "kill" ? NPM_SIGKILL : 0;
    if (!sig || pid <= 0) { std::fputs("usage: novapm-cli signal PID term|stop|cont|kill\n", stderr); return 2; }
    SignalResult r = SignalManager::send(pid, sig);
    Database db(c.cfg.dbPath);
    if (db.ok()) db.logEvent(pid, "", "signal", std::string(SignalManager::name(sig)) + (r.ok ? " ok (cli)" : " failed: " + r.msg));
    std::printf("%s\n", r.msg.c_str());
    return r.ok ? 0 : 1;
}

int cmdContainers() {
    ProcessCollector pc; SystemMonitor sm;
    pc.collect();
    std::this_thread::sleep_for(500ms);
    auto procs = pc.collect();
    struct G { int n = 0; double cpu = 0; long rss = 0; };
    std::map<std::string, G> groups;
    for (auto& p : procs) { auto& g = groups[p.container.empty() ? "(host)" : p.container]; ++g.n; g.cpu += p.cpu; g.rss += p.rss_kb; }
    std::printf("%-30s %6s %8s %10s\n", "CONTAINER", "PROCS", "CPU%", "MEM(MB)");
    for (auto& [k, g] : groups) std::printf("%-30s %6d %8.1f %10.1f\n", k.c_str(), g.n, g.cpu, g.rss / 1024.0);
#ifdef _WIN32
    std::puts("(container detection reads Linux cgroups; on Windows every process is listed as host)");
#endif
    return 0;
}

int cmdConfig(const Args& a, Ctx& c) {
    if (a.pos.size() >= 2 && a.pos[1] == "set") {
        if (a.pos.size() < 4) { std::fputs("usage: novapm-cli config set KEY VALUE\n", stderr); return 2; }
        std::string err;
        if (!setConfigValue(c.cfg, a.pos[2], a.pos[3], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
        sanitizeConfig(c.cfg);
        if (!saveConfig(c.cfgPath, c.cfg, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        std::printf("saved %s\n", c.cfgPath.c_str());
    }
    const GuardConfig& g = c.cfg.guard;
    std::printf("settings file: %s\n", c.cfgPath.c_str());
    std::printf("  enabled=%d auto_stop=%d warn_pct=%.0f stop_pct=%.0f stop_sustain=%d alert_cooldown_s=%d resume_grace_s=%d\n",
        g.enabled, g.autoStop, g.warnPct, g.stopPct, g.stopSustain, g.alertCooldownSec, g.resumeGraceSec);
    std::printf("  watch_cpu=%d watch_mem=%d normalize_cpu=%d refresh_ms=%d db_every_s=%d db_enabled=%d db_path=%s\n",
        g.watchCpu, g.watchMem, g.normalizeCpu, c.cfg.refreshMs, c.cfg.dbEverySec, c.cfg.dbEnabled, c.cfg.dbPath.c_str());
    return 0;
}
}  // namespace

bool isCliCommand(const char* word) {
    for (auto c : kCommands) if (std::string(word) == c) return true;
    return false;
}

int runCli(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    const std::string cmd = argv[1];
    if (cmd == "help" || cmd == "--help" || cmd == "-h") { usage(); return 0; }
    if (cmd == "version" || cmd == "--version") { std::puts("NovaPM 1.0.0"); return 0; }
    Args a = parseArgs(argc, argv, 1);          // a.pos[0] is the command itself
    Ctx c; loadCtx(a, c);
    std::signal(SIGINT, onSigint);
    if (cmd == "top")        return cmdTop(a);
    if (cmd == "watch")      return cmdWatch(a, c);
    if (cmd == "history")    return cmdHistory(a, c);
    if (cmd == "alerts")     return cmdAlerts(a, c);
    if (cmd == "query")      return cmdQuery(a, c);
    if (cmd == "signal")     return cmdSignal(a, c);
    if (cmd == "containers") return cmdContainers();
    if (cmd == "config")     return cmdConfig(a, c);
    usage();
    return 2;
}
