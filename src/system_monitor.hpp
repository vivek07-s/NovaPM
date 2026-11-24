#pragma once
#include "types.hpp"

class SystemMonitor {
public:
    SysStats sample(int procCount, int threadCount);
private:
    unsigned long long prevTotal_ = 0, prevIdle_ = 0;
};
