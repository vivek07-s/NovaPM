// Linux process collector: reads /proc/<pid>/stat, owner uid, /proc/<pid>/cgroup.
#include "process_collector.hpp"
#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

namespace {
bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf();
    out = ss.str();
    return true;
}
bool isNumber(const char* s) { if (!*s) return false; for (; *s; ++s) if (!std::isdigit((unsigned char)*s)) return false; return true; }

std::string userName(unsigned uid) {
    passwd pw{}, *res = nullptr; char buf[1024];
    if (getpwuid_r(uid, &pw, buf, sizeof buf, &res) == 0 && res) return pw.pw_name;
    return std::to_string(uid);
}

// Parses the text of /proc/<pid>/stat. Fields after the ")" are numbered from "state" = 0.
struct Stat { std::string name; std::vector<std::string> f; };
bool parseStat(const std::string& s, Stat& out) {
    auto lp = s.find('('), rp = s.rfind(')');          // the name may itself contain ')' and spaces
    if (lp == std::string::npos || rp == std::string::npos || rp < lp) return false;
    out.name = s.substr(lp + 1, rp - lp - 1);
    std::istringstream is(s.substr(rp + 1));
    out.f.clear();
    for (std::string tok; is >> tok;) out.f.push_back(tok);
    return out.f.size() >= 22;
}
}

std::vector<ProcInfo> ProcessCollector::collect() {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    const bool havePrev = prevTime_.time_since_epoch().count() != 0;
    const double dt = havePrev ? std::chrono::duration<double>(now - prevTime_).count() : 0.0;
    static const long clk = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    static const long pageKb = sysconf(_SC_PAGESIZE) / 1024;

    std::vector<ProcInfo> out;
    std::map<int, unsigned long long> newTicks;
    std::map<int, std::string> newUsers, newContainers;

    DIR* d = opendir("/proc");
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        if (!isNumber(e->d_name)) continue;
        const int pid = std::atoi(e->d_name);
        const std::string base = std::string("/proc/") + e->d_name;

        std::string text; Stat st;
        if (!readFile(base + "/stat", text) || !parseStat(text, st)) continue;   // exited meanwhile

        ProcInfo p;
        p.pid = pid;
        p.name = st.name;
        p.state = st.f[0][0];
        p.ppid = std::atoi(st.f[1].c_str());
        p.threads = std::atoi(st.f[17].c_str());
        p.rss_kb = std::atol(st.f[21].c_str()) * pageKb;

        // CPU time in 100-ns units so the maths matches the Windows collector
        const unsigned long long ticks = (std::strtoull(st.f[11].c_str(), nullptr, 10) +
                                          std::strtoull(st.f[12].c_str(), nullptr, 10)) * 10000000ULL / clk;
        newTicks[pid] = ticks;
        auto it = prevTicks_.find(pid);
        if (havePrev && dt > 0 && it != prevTicks_.end() && ticks >= it->second)
            p.cpu = 100.0 * (double(ticks - it->second) / 1e7) / dt;

        auto uc = userCache_.find(pid);
        if (uc != userCache_.end()) p.user = uc->second;
        else { struct stat sb{}; p.user = ::stat(base.c_str(), &sb) == 0 ? userName(sb.st_uid) : "?"; }
        newUsers[pid] = p.user;

        auto cc = containerCache_.find(pid);
        if (cc != containerCache_.end()) p.container = cc->second;
        else { std::string cg; if (readFile(base + "/cgroup", cg)) p.container = containerFromCgroup(cg); }
        newContainers[pid] = p.container;

        out.push_back(std::move(p));
    }
    closedir(d);
    prevTicks_ = std::move(newTicks);
    userCache_ = std::move(newUsers);
    containerCache_ = std::move(newContainers);
    prevTime_ = now;
    return out;
}

bool ProcessCollector::details(int pid, ProcDetails& d) {
    const std::string base = "/proc/" + std::to_string(pid);
    std::string text; Stat st;
    if (!readFile(base + "/stat", text) || !parseStat(text, st)) return false;

    char buf[4096];
    ssize_t n = readlink((base + "/exe").c_str(), buf, sizeof buf - 1);
    d.exe = n > 0 ? std::string(buf, (size_t)n) : "(not accessible)";

    std::string cmd;
    if (readFile(base + "/cmdline", cmd) && !cmd.empty()) {
        for (auto& c : cmd) if (c == '\0') c = ' ';
        while (!cmd.empty() && cmd.back() == ' ') cmd.pop_back();
        d.cmdline = cmd;
    } else d.cmdline = "(none / kernel thread)";

    d.handles = -1;
    if (DIR* fd = opendir((base + "/fd").c_str())) {
        int c = 0; while (dirent* e = readdir(fd)) if (isNumber(e->d_name)) ++c;
        closedir(fd); d.handles = c;
    }

    d.priority = "nice " + st.f[16];

    // start time = boot time + starttime / CLK_TCK
    static const long clk = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    std::string stat0; long long btime = 0;
    if (readFile("/proc/stat", stat0)) { auto k = stat0.find("btime "); if (k != std::string::npos) btime = std::atoll(stat0.c_str() + k + 6); }
    if (btime > 0) {
        std::time_t t = (std::time_t)(btime + std::strtoull(st.f[19].c_str(), nullptr, 10) / clk);
        std::tm tm{}; localtime_r(&t, &tm);
        char tb[32]; std::strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", &tm);
        d.started = tb;
    }

    std::string cg;
    d.container = readFile(base + "/cgroup", cg) ? containerFromCgroup(cg) : "";
    return true;
}
