#include "threshold_guard.hpp"
#include <algorithm>
#include <cctype>
#include <set>

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
constexpr double kRearmMargin = 5.0;   // usage must fall this far below warnPct to re-arm the alert
}

double ThresholdGuard::metricValue(const ProcInfo& p, const SysStats& sys, const GuardConfig& cfg, GuardMetric m) {
    if (m == GuardMetric::Cpu) {
        double v = p.cpu;
        if (cfg.normalizeCpu && sys.cores > 0) v /= sys.cores;
        return v;
    }
    return sys.mem_total_kb > 0 ? 100.0 * (double)p.rss_kb / (double)sys.mem_total_kb : 0.0;
}

int ThresholdGuard::level(const ProcInfo& p, const SysStats& sys, const GuardConfig& cfg) {
    if (!cfg.enabled) return 0;
    int lv = 0;
    for (GuardMetric m : {GuardMetric::Cpu, GuardMetric::Memory}) {
        if ((m == GuardMetric::Cpu && !cfg.watchCpu) || (m == GuardMetric::Memory && !cfg.watchMem)) continue;
        double v = metricValue(p, sys, cfg, m);
        if (v >= cfg.stopPct) lv = std::max(lv, 2);
        else if (v >= cfg.warnPct) lv = std::max(lv, 1);
    }
    return lv;
}

bool ThresholdGuard::isProtected(const ProcInfo& p) {
    if (p.pid <= 4) return true;
    static const std::set<std::string> kNames = {
        // Windows core
        "system", "registry", "smss.exe", "csrss.exe", "wininit.exe", "winlogon.exe",
        "services.exe", "lsass.exe", "svchost.exe", "dwm.exe", "explorer.exe",
        "fontdrvhost.exe", "memory compression", "securityhealthservice.exe",
        // Linux core (for the /proc build)
        "systemd", "init", "kthreadd", "dbus-daemon", "xorg", "gnome-shell",
        // NovaPM itself
        "novapm.exe", "novapm"};
    return kNames.count(lower(p.name)) > 0;
}

std::vector<GuardEvent> ThresholdGuard::evaluate(const std::vector<ProcInfo>& procs, const SysStats& sys,
                                                 const GuardConfig& cfg, Clock::time_point now) {
    std::vector<GuardEvent> out;

    // forget processes that no longer exist
    std::set<int> alive;
    for (const auto& p : procs) alive.insert(p.pid);
    for (auto it = st_.begin(); it != st_.end();)       it = alive.count(it->first.first) ? std::next(it) : st_.erase(it);
    for (auto it = prevState_.begin(); it != prevState_.end();) it = alive.count(it->first) ? std::next(it) : prevState_.erase(it);
    for (auto it = grace_.begin(); it != grace_.end();)  it = alive.count(it->first) ? std::next(it) : grace_.erase(it);

    // keep state tracking up to date even when the guard is disabled
    std::set<int> stoppedThisPass;
    for (const auto& p : procs) {
        if (p.pid == 0) continue;

        char before = prevState_.count(p.pid) ? prevState_[p.pid] : p.state;
        prevState_[p.pid] = p.state;
        if (before == 'T' && p.state != 'T')                       // user resumed it -> grace period
            grace_[p.pid] = now + std::chrono::seconds(cfg.resumeGraceSec);

        if (!cfg.enabled) continue;
        if (p.state == 'T') {                                      // already suspended: nothing to watch
            for (int m = 0; m < 2; ++m) st_.erase({p.pid, m});
            continue;
        }

        for (GuardMetric m : {GuardMetric::Cpu, GuardMetric::Memory}) {
            if ((m == GuardMetric::Cpu && !cfg.watchCpu) || (m == GuardMetric::Memory && !cfg.watchMem)) continue;

            State& s = st_[{p.pid, (int)m}];
            if (s.name != p.name) { s = State{}; s.name = p.name; }   // new process / PID reuse
            const double v = metricValue(p, sys, cfg, m);

            if (v < cfg.warnPct - kRearmMargin) { s.warned = false; s.highCount = 0; continue; }
            if (v < cfg.warnPct)                 { s.highCount = 0; continue; }

            // ---- level 1: alert at >= warnPct ----
            const bool cooled = s.lastWarn == Clock::time_point{} ||
                                now - s.lastWarn >= std::chrono::seconds(cfg.alertCooldownSec);
            if (!s.warned || cooled) {
                out.push_back({p.pid, p.name, m, GuardAction::Warn, v, cfg.warnPct});
                s.warned = true;
                s.lastWarn = now;
            }

            // ---- level 2: stop at >= stopPct, only if sustained ----
            if (v < cfg.stopPct) { s.highCount = 0; continue; }
            ++s.highCount;
            if (!cfg.autoStop || s.highCount < std::max(1, cfg.stopSustain)) continue;
            if (stoppedThisPass.count(p.pid)) continue;
            if (s.nextStopTry != Clock::time_point{} && now < s.nextStopTry) continue;
            auto g = grace_.find(p.pid);
            if (g != grace_.end() && now < g->second) continue;

            s.highCount = 0;
            s.nextStopTry = now + std::chrono::seconds(cfg.alertCooldownSec);   // limits retries if the stop fails
            if (isProtected(p)) {
                out.push_back({p.pid, p.name, m, GuardAction::StopSkipped, v, cfg.stopPct});
            } else {
                out.push_back({p.pid, p.name, m, GuardAction::Stop, v, cfg.stopPct});
                stoppedThisPass.insert(p.pid);
            }
        }
    }
    return out;
}
