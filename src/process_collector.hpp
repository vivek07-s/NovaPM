#pragma once
#include "types.hpp"
#include <chrono>
#include <map>
#include <string>
#include <vector>

// Platform-independent: container id from the text of /proc/<pid>/cgroup ("" if not in a container).
// Recognises docker, containerd/CRI, CRI-O, podman (libpod) and generic kubepods paths.
std::string containerFromCgroup(const std::string& cgroupText);

// Linux  (src/platform/linux):  /proc/<pid>/stat, status, cgroup.
// Windows (src/platform/win32): Toolhelp32 snapshot + GetProcessTimes / GetProcessMemoryInfo.
// CPU% (of one core) is computed from CPU-time deltas between two calls to collect().
class ProcessCollector {
public:
    std::vector<ProcInfo> collect();
    static bool details(int pid, ProcDetails& out);   // false if the process cannot be inspected
private:
    std::map<int, std::string> containerCache_;
    std::map<int, unsigned long long> prevTicks_;   // 100-ns units
    std::map<int, std::string> userCache_;
    std::chrono::steady_clock::time_point prevTime_{};
};
