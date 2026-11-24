// Linux system monitor: /proc/stat, /proc/meminfo, /proc/uptime.
#include "system_monitor.hpp"
#include <unistd.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

SysStats SystemMonitor::sample(int procCount, int threadCount) {
    SysStats s;
    s.procs = procCount;
    s.threads = threadCount;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    s.cores = n > 0 ? (int)n : 1;

    {   // first line: cpu user nice system idle iowait irq softirq steal ...
        std::ifstream f("/proc/stat");
        std::string label; f >> label;
        unsigned long long v[8] = {0};
        for (auto& x : v) f >> x;
        unsigned long long total = 0;
        for (auto x : v) total += x;
        unsigned long long idle = v[3] + v[4];                       // idle + iowait
        if (prevTotal_ && total > prevTotal_) {
            double dt = double(total - prevTotal_), di = double(idle - prevIdle_);
            s.cpu = 100.0 * (dt - di) / dt;
        }
        prevTotal_ = total; prevIdle_ = idle;
    }
    {
        std::ifstream f("/proc/meminfo");
        long memTotal = 0, memAvail = 0, swapTotal = 0, swapFree = 0;
        std::string key; long val; std::string unit;
        while (f >> key >> val) {
            std::getline(f, unit);
            if (key == "MemTotal:") memTotal = val;
            else if (key == "MemAvailable:") memAvail = val;
            else if (key == "SwapTotal:") swapTotal = val;
            else if (key == "SwapFree:") swapFree = val;
        }
        s.mem_total_kb = memTotal;
        s.mem_used_kb = memTotal - memAvail;
        s.swap_total_kb = swapTotal;
        s.swap_used_kb = swapTotal - swapFree;
    }
    {
        std::ifstream f("/proc/uptime");
        f >> s.uptime;
    }
    return s;
}
