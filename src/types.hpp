#pragma once
#include <string>

struct ProcInfo {
    int pid = 0, ppid = 0;
    std::string name, user;
    char state = '?';
    double cpu = 0.0;      // percent of one core
    long rss_kb = 0;
    int threads = 0;
    std::string container;  // e.g. "docker:3f2a9c1d7b44"; empty = not in a container (Linux cgroups)
};

// Extra information shown in the Details panel / `novapm-cli`. Filled on demand.
struct ProcDetails {
    std::string exe, cmdline, started, priority, container;
    int handles = -1;       // open file descriptors (Linux) / handle count (Windows); -1 = unknown
};

struct SysStats {
    double cpu = 0.0;      // percent, all cores
    long mem_total_kb = 0, mem_used_kb = 0;
    long swap_total_kb = 0, swap_used_kb = 0;
    double uptime = 0.0;
    int procs = 0, threads = 0;
    int cores = 1;         // logical processors (used to normalise per-process CPU %)
};
