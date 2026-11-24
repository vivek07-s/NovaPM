#pragma once
#include <string>

enum { NPM_SIGKILL = 9, NPM_SIGTERM = 15, NPM_SIGCONT = 18, NPM_SIGSTOP = 19 };

struct SignalResult { bool ok; std::string msg; };

// Linux: real signals via kill(2).  Windows mapping:  TERM = ask windows to close (WM_CLOSE), STOP = suspend,
//                   CONT = resume, KILL = TerminateProcess.
class SignalManager {
public:
    static SignalResult send(int pid, int sig);
    static const char* name(int sig);
    static bool isSuspended(int pid);
};
