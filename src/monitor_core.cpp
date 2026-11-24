#include "monitor_core.hpp"
#include "signal_manager.hpp"
#include <cstdio>
#include <ctime>

std::string clockText() {
    char b[16]; std::time_t t = std::time(nullptr); std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::strftime(b, sizeof b, "%H:%M:%S", &tm);
    return b;
}

AlertRec applyGuardAction(const GuardEvent& e, Database& db) {
    char val[32], thr[32];
    std::snprintf(val, sizeof val, "%.1f%%", e.value);
    std::snprintf(thr, sizeof thr, "%.0f%%", e.threshold);
    const std::string who = e.name + " [" + std::to_string(e.pid) + "] " + ThresholdGuard::metricName(e.metric);

    AlertRec a;
    a.time = clockText(); a.pid = e.pid; a.name = e.name; a.value = e.value;
    a.metric = ThresholdGuard::metricName(e.metric);
    std::string action, result;

    switch (e.action) {
    case GuardAction::Warn:
        a.level = "WARNING";
        a.msg = who + " at " + val + " (alert threshold " + thr + ")";
        action = "warn"; result = "alert raised";
        db.logEvent(e.pid, e.name, "alert", a.msg);
        break;
    case GuardAction::Stop: {
        SignalResult r = SignalManager::send(e.pid, NPM_SIGSTOP);
        a.level = r.ok ? "AUTO-STOP" : "STOP FAILED";
        a.msg = who + " at " + val + " (stop threshold " + thr + ") - " +
                (r.ok ? "process suspended, use Resume to continue it" : r.msg);
        action = "stop"; result = r.ok ? "suspended" : "failed: " + r.msg;
        db.logEvent(e.pid, e.name, "auto_stop", std::string("SIGSTOP ") + (r.ok ? "ok" : "failed: " + r.msg) + " at " + val);
        break;
    }
    case GuardAction::StopSkipped:
        a.level = "PROTECTED";
        a.msg = who + " at " + val + " - protected system process, not stopped";
        action = "stop_skipped"; result = "protected";
        db.logEvent(e.pid, e.name, "alert", a.msg);
        break;
    }
    db.logAlert(e.pid, e.name, a.metric, action, e.value, e.threshold, result);
    return a;
}

void logLifecycle(const std::map<int, std::string>& prev, const std::map<int, std::string>& cur, Database& db) {
    for (auto& [pid, name] : cur)  if (!prev.count(pid)) db.logEvent(pid, name, "start", "");
    for (auto& [pid, name] : prev) if (!cur.count(pid))  db.logEvent(pid, name, "exit", "");
}
