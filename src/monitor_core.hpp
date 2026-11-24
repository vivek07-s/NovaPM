#pragma once
#include "database.hpp"
#include "threshold_guard.hpp"
#include <map>
#include <string>

// Pieces shared by the GUI worker thread and the command-line `watch` mode.

struct AlertRec {
    std::string time, level, name, metric, msg;   // level: WARNING | AUTO-STOP | STOP FAILED | PROTECTED
    int pid = 0;
    double value = 0.0;
};

std::string clockText();   // HH:MM:SS

// Carry out one ThresholdGuard decision: for Stop it suspends the process (SIGSTOP). Everything is
// written to the `alerts` and `events` tables. Returns the record to show to the user.
AlertRec applyGuardAction(const GuardEvent& e, Database& db);

// Writes "start" / "exit" events for PIDs that appeared / disappeared between two samples.
void logLifecycle(const std::map<int, std::string>& prev, const std::map<int, std::string>& cur, Database& db);
