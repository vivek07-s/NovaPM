#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "signal_manager.hpp"
#include <mutex>
#include <set>

namespace {
std::mutex g_m;
std::set<int> g_susp;
typedef LONG (NTAPI *NtProcFn)(HANDLE);

NtProcFn ntFn(const char* n) {
    HMODULE h = GetModuleHandleW(L"ntdll.dll");
    return h ? reinterpret_cast<NtProcFn>(reinterpret_cast<void*>(GetProcAddress(h, n))) : nullptr;
}
SignalResult fail(int pid, DWORD err) {
    if (err == ERROR_ACCESS_DENIED)     return {false, "Permission denied for PID " + std::to_string(pid) + " (try running as Administrator)"};
    if (err == ERROR_INVALID_PARAMETER) return {false, "Process " + std::to_string(pid) + " already exited"};
    return {false, "Operation failed (Windows error " + std::to_string(err) + ")"};
}
struct EnumCtx { DWORD pid; int count; };
BOOL CALLBACK enumWin(HWND hwnd, LPARAM lp) {
    auto* c = reinterpret_cast<EnumCtx*>(lp);
    DWORD wp = 0;
    GetWindowThreadProcessId(hwnd, &wp);
    if (wp == c->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        c->count++;
    }
    return TRUE;
}
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
    if (pid <= 4)                              return {false, "Refusing to touch system processes (PID <= 4)"};
    if (pid == (int)GetCurrentProcessId())     return {false, "Refusing to signal NovaPM itself"};
    const std::string sp = std::to_string(pid);

    if (sig == NPM_SIGTERM) {
        EnumCtx c{(DWORD)pid, 0};
        EnumWindows(enumWin, reinterpret_cast<LPARAM>(&c));
        if (c.count == 0) return {false, "No window to close for PID " + sp + " - use Kill instead"};
        return {true, "Close request sent to " + sp};
    }
    if (sig == NPM_SIGKILL) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!h) return fail(pid, GetLastError());
        BOOL ok = TerminateProcess(h, 1);
        DWORD err = GetLastError();
        CloseHandle(h);
        return ok ? SignalResult{true, "SIGKILL sent to " + sp} : fail(pid, err);
    }
    if (sig == NPM_SIGSTOP || sig == NPM_SIGCONT) {
        NtProcFn fn = ntFn(sig == NPM_SIGSTOP ? "NtSuspendProcess" : "NtResumeProcess");
        if (!fn) return {false, "Suspend/resume not available on this Windows version"};
        HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid);
        if (!h) return fail(pid, GetLastError());
        LONG st = fn(h);
        CloseHandle(h);
        if (st != 0) return {false, "Suspend/resume failed (NTSTATUS " + std::to_string(st) + ")"};
        std::lock_guard<std::mutex> lk(g_m);
        if (sig == NPM_SIGSTOP) g_susp.insert(pid); else g_susp.erase(pid);
        return {true, std::string(name(sig)) + " sent to " + sp};
    }
    return {false, "Signal not allowed"};
}
