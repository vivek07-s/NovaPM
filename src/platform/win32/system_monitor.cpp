#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "system_monitor.hpp"

static unsigned long long ft(const FILETIME& f) {
    return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

SysStats SystemMonitor::sample(int procCount, int threadCount) {
    SysStats s;
    s.procs = procCount;
    s.threads = threadCount;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    s.cores = si.dwNumberOfProcessors ? static_cast<int>(si.dwNumberOfProcessors) : 1;

    FILETIME idleT, kernT, userT;
    if (GetSystemTimes(&idleT, &kernT, &userT)) {
        unsigned long long idle = ft(idleT), total = ft(kernT) + ft(userT);   // kernel time includes idle
        if (prevTotal_ && total > prevTotal_) {
            double dt = double(total - prevTotal_), di = double(idle - prevIdle_);
            s.cpu = 100.0 * (dt - di) / dt;
        }
        prevTotal_ = total; prevIdle_ = idle;
    }

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        s.mem_total_kb = static_cast<long>(ms.ullTotalPhys / 1024);
        s.mem_used_kb  = static_cast<long>((ms.ullTotalPhys - ms.ullAvailPhys) / 1024);
        long long commitLimit = static_cast<long long>(ms.ullTotalPageFile);
        long long commitUsed  = static_cast<long long>(ms.ullTotalPageFile - ms.ullAvailPageFile);
        long long swapTotal   = commitLimit - static_cast<long long>(ms.ullTotalPhys);          // page file size (approx)
        long long swapUsed    = commitUsed  - static_cast<long long>(ms.ullTotalPhys - ms.ullAvailPhys);
        if (swapTotal < 0) swapTotal = 0;
        if (swapUsed  < 0) swapUsed = 0;
        if (swapUsed > swapTotal) swapUsed = swapTotal;
        s.swap_total_kb = static_cast<long>(swapTotal / 1024);
        s.swap_used_kb  = static_cast<long>(swapUsed / 1024);
    }
    s.uptime = GetTickCount64() / 1000.0;
    return s;
}
