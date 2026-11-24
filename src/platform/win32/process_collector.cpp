#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include "process_collector.hpp"
#include "signal_manager.hpp"
#include <cstdio>

namespace {
std::string toUtf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
unsigned long long ft(const FILETIME& f) {
    return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}
std::string lookupUser(HANDLE proc) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(proc, TOKEN_QUERY, &tok)) return "?";
    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    std::string res = "?";
    if (len) {
        std::vector<char> buf(len);
        if (GetTokenInformation(tok, TokenUser, buf.data(), len, &len)) {
            auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
            wchar_t name[256], dom[256];
            DWORD nl = 256, dl = 256;
            SID_NAME_USE use;
            if (LookupAccountSidW(nullptr, tu->User.Sid, name, &nl, dom, &dl, &use)) res = toUtf8(name);
        }
    }
    CloseHandle(tok);
    return res;
}
}

std::vector<ProcInfo> ProcessCollector::collect() {
    using clock = std::chrono::steady_clock;
    auto now = clock::now();
    bool havePrev = prevTime_.time_since_epoch().count() != 0;
    double dt = havePrev ? std::chrono::duration<double>(now - prevTime_).count() : 0.0;

    std::vector<ProcInfo> out;
    std::map<int, unsigned long long> newTicks;
    std::map<int, std::string> newUsers;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        int pid = static_cast<int>(pe.th32ProcessID);
        if (pid == 0) continue;                       // System Idle Process

        ProcInfo p;
        p.pid = pid;
        p.ppid = static_cast<int>(pe.th32ParentProcessID);
        p.name = toUtf8(pe.szExeFile);
        p.threads = static_cast<int>(pe.cntThreads);
        p.state = SignalManager::isSuspended(pid) ? 'T' : 'R';

        unsigned long long ticks = 0;
        std::string user = userCache_.count(pid) ? userCache_[pid] : "";
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (h) {
            FILETIME c, e, k, u;
            if (GetProcessTimes(h, &c, &e, &k, &u)) ticks = ft(k) + ft(u);
            PROCESS_MEMORY_COUNTERS pmc{};
            if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) p.rss_kb = static_cast<long>(pmc.WorkingSetSize / 1024);
            if (user.empty()) user = lookupUser(h);
            CloseHandle(h);
        }
        p.user = user.empty() ? (pid == 4 ? "SYSTEM" : "?") : user;
        newUsers[pid] = p.user;

        newTicks[pid] = ticks;
        auto it = prevTicks_.find(pid);
        if (havePrev && dt > 0 && it != prevTicks_.end() && ticks >= it->second)
            p.cpu = 100.0 * (static_cast<double>(ticks - it->second) / 1e7) / dt;
        out.push_back(std::move(p));
    }
    CloseHandle(snap);
    prevTicks_ = std::move(newTicks);
    userCache_ = std::move(newUsers);
    prevTime_ = now;
    return out;
}

bool ProcessCollector::details(int pid, ProcDetails& d) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    wchar_t path[MAX_PATH * 4];
    DWORD n = sizeof(path) / sizeof(path[0]);
    d.exe = QueryFullProcessImageNameW(h, 0, path, &n) ? toUtf8(path) : "(not accessible)";
    d.cmdline = "(not available on Windows)";
    DWORD hc = 0;
    d.handles = GetProcessHandleCount(h, &hc) ? static_cast<int>(hc) : -1;
    switch (GetPriorityClass(h)) {
        case IDLE_PRIORITY_CLASS:         d.priority = "Idle"; break;
        case BELOW_NORMAL_PRIORITY_CLASS: d.priority = "Below normal"; break;
        case NORMAL_PRIORITY_CLASS:       d.priority = "Normal"; break;
        case ABOVE_NORMAL_PRIORITY_CLASS: d.priority = "Above normal"; break;
        case HIGH_PRIORITY_CLASS:         d.priority = "High"; break;
        case REALTIME_PRIORITY_CLASS:     d.priority = "Realtime"; break;
        default:                          d.priority = "?"; break;
    }
    FILETIME c, e, k, u;
    if (GetProcessTimes(h, &c, &e, &k, &u)) {
        FILETIME lc; SYSTEMTIME st;
        if (FileTimeToLocalFileTime(&c, &lc) && FileTimeToSystemTime(&lc, &st)) {
            char b[32];
            std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            d.started = b;
        }
    }
    d.container.clear();
    CloseHandle(h);
    return true;
}
