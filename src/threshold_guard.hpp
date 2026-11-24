#pragma once
#include "types.hpp"
#include <chrono>
#include <map>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Threshold Guard - per-process usage policy.
//
//   usage >= warnPct (default 60 %)  -> ALERT   (user is warned, nothing is touched)
//   usage >= stopPct (default 80 %)  -> STOP    (process is suspended = SIGSTOP)
//
// The guard is pure logic: it looks at one sample and returns the actions the
// caller should take. It never sends signals or touches the GUI/database, which
// keeps it platform independent and unit-testable (see tests/guard_test.cpp).
// ---------------------------------------------------------------------------

struct GuardConfig {
    bool   enabled          = true;
    bool   autoStop         = true;   // false = alerts only
    double warnPct          = 60.0;
    double stopPct          = 80.0;
    int    stopSustain      = 3;      // consecutive samples >= stopPct before stopping
    int    alertCooldownSec = 30;     // minimum gap between repeat alerts for one process
    int    resumeGraceSec   = 60;     // no auto-stop for this long after the user resumes a process
    bool   watchCpu         = true;
    bool   watchMem         = true;
    bool   normalizeCpu     = false;  // false: % of ONE core (as shown in the table)
                                      // true : % of the whole machine (value / core count)
};

enum class GuardMetric { Cpu, Memory };
enum class GuardAction {
    Warn,             // crossed warnPct
    Stop,             // sustained >= stopPct: caller must suspend the process
    StopSkipped       // sustained >= stopPct but process is protected: not stopped
};

struct GuardEvent {
    int         pid = 0;
    std::string name;
    GuardMetric metric = GuardMetric::Cpu;
    GuardAction action = GuardAction::Warn;
    double      value = 0.0;       // usage in percent that triggered the event
    double      threshold = 0.0;   // threshold that was crossed
};

class ThresholdGuard {
public:
    using Clock = std::chrono::steady_clock;

    // Evaluate one collector sample. Call once per sample, from one thread.
    std::vector<GuardEvent> evaluate(const std::vector<ProcInfo>& procs, const SysStats& sys,
                                     const GuardConfig& cfg, Clock::time_point now);

    // Usage of one process for one metric, in percent.
    static double metricValue(const ProcInfo& p, const SysStats& sys, const GuardConfig& cfg, GuardMetric m);
    // 0 = below warn, 1 = warn level, 2 = stop level (used for row colouring).
    static int level(const ProcInfo& p, const SysStats& sys, const GuardConfig& cfg);
    // Critical OS processes are never auto-stopped.
    static bool isProtected(const ProcInfo& p);

    static const char* metricName(GuardMetric m) { return m == GuardMetric::Cpu ? "CPU" : "MEM"; }

private:
    struct State {
        std::string name;
        bool   warned = false;
        int    highCount = 0;
        Clock::time_point lastWarn{};
        Clock::time_point nextStopTry{};
    };
    std::map<std::pair<int, int>, State> st_;      // (pid, metric) -> state
    std::map<int, char> prevState_;                // pid -> state in previous sample
    std::map<int, Clock::time_point> grace_;       // pid -> no auto-stop before this time
};
