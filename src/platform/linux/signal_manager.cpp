// Linux signal manager: real POSIX signals through kill(2).
#include "signal_manager.hpp"
#include <cerrno>
#include <csignal>
#include <mutex>
#include <set>
#include <unistd.h>

namespace {
std::mutex g_m;
std::set<int> g_susp;
}

const char* SignalManager::name(int sig) {
    switch (sig) {
        case NPM_SIGTERM: return "SIGTERM";
        case NPM_SIGSTOP: return "SIGSTOP";
        case NPM_SIGCONT: return "SIGCONT";
        case NPM_SIGKILL: return "SIGKILL";
        default: return "SIG?";
    }
}

bool SignalManager::isSuspended(int pid) {
    std::lock_guard<std::mutex> lk(g_m);
    return g_susp.count(pid) > 0;
}

SignalResult SignalManager::send(int pid, int sig) {
    if (pid <= 1)            return {false, "Refusing to touch system processes (PID <= 1)"};
    if (pid == (int)getpid()) return {false, "Refusing to signal NovaPM itself"};
    int real;
    switch (sig) {
        case NPM_SIGTERM: real = SIGTERM; break;
        case NPM_SIGSTOP: real = SIGSTOP; break;
        case NPM_SIGCONT: real = SIGCONT; break;
        case NPM_SIGKILL: real = SIGKILL; break;
        default: return {false, "Signal not allowed"};
    }
    const std::string sp = std::to_string(pid);
    if (kill(pid, real) != 0) {
        if (errno == EPERM) return {false, "Permission denied for PID " + sp + " (try sudo)"};
        if (errno == ESRCH) return {false, "Process " + sp + " already exited"};
        return {false, "Operation failed (errno " + std::to_string(errno) + ")"};
    }
    std::lock_guard<std::mutex> lk(g_m);
    if (sig == NPM_SIGSTOP) g_susp.insert(pid);
    else if (sig == NPM_SIGCONT || sig == NPM_SIGKILL) g_susp.erase(pid);
    return {true, std::string(name(sig)) + " sent to " + sp};
}
