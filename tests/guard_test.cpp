// Unit tests for ThresholdGuard (alert at 60 %, auto-stop at 80 %).
// Build & run:  g++ -std=c++20 -Isrc tests/guard_test.cpp src/threshold_guard.cpp -o guard_test && ./guard_test
#include "threshold_guard.hpp"
#include <cstdio>
#include <cstdlib>

using namespace std::chrono_literals;
using Clock = ThresholdGuard::Clock;

static int g_pass = 0, g_fail = 0;
#define CHECK(name, cond) do { if (cond) { ++g_pass; std::printf("  PASS  %s\n", name); } \
                               else { ++g_fail; std::printf("  FAIL  %s   (%s:%d)\n", name, __FILE__, __LINE__); } } while (0)

static ProcInfo proc(int pid, const char* name, double cpu, long rss_kb = 1000, char st = 'R') {
    ProcInfo p; p.pid = pid; p.name = name; p.cpu = cpu; p.rss_kb = rss_kb; p.state = st; return p;
}
static SysStats sysStats(int cores = 8) { SysStats s; s.mem_total_kb = 16L * 1024 * 1024; s.cores = cores; return s; }
static int count(const std::vector<GuardEvent>& v, GuardAction a) { int n = 0; for (auto& e : v) n += e.action == a; return n; }

int main() {
    const SysStats sys = sysStats();
    const Clock::time_point t0 = Clock::now();

    std::puts("T1  below 60 % -> silent");
    { ThresholdGuard g; GuardConfig c;
      auto e = g.evaluate({proc(100, "a.exe", 59.9)}, sys, c, t0);
      CHECK("no events at 59.9 %", e.empty()); }

    std::puts("T2  crossing 60 % -> exactly one alert, process untouched");
    { ThresholdGuard g; GuardConfig c;
      auto e1 = g.evaluate({proc(100, "a.exe", 60.0)}, sys, c, t0);
      CHECK("one Warn at 60.0 %", e1.size() == 1 && e1[0].action == GuardAction::Warn && e1[0].threshold == 60.0);
      auto e2 = g.evaluate({proc(100, "a.exe", 65.0)}, sys, c, t0 + 1s);
      CHECK("no repeat alert 1 s later (cooldown)", e2.empty());
      auto e3 = g.evaluate({proc(100, "a.exe", 70.0)}, sys, c, t0 + 2s);
      CHECK("never a Stop below 80 %", count(e3, GuardAction::Stop) == 0); }

    std::puts("T3  alert repeats after cooldown while still high");
    { ThresholdGuard g; GuardConfig c;
      g.evaluate({proc(100, "a.exe", 65)}, sys, c, t0);
      auto e = g.evaluate({proc(100, "a.exe", 65)}, sys, c, t0 + 31s);
      CHECK("Warn again after 31 s", count(e, GuardAction::Warn) == 1); }

    std::puts("T4  80 % must be sustained for 3 samples before stop");
    { ThresholdGuard g; GuardConfig c;
      auto e1 = g.evaluate({proc(100, "a.exe", 85)}, sys, c, t0);
      auto e2 = g.evaluate({proc(100, "a.exe", 85)}, sys, c, t0 + 1s);
      CHECK("no Stop after 1 and 2 samples", count(e1, GuardAction::Stop) + count(e2, GuardAction::Stop) == 0);
      auto e3 = g.evaluate({proc(100, "a.exe", 85)}, sys, c, t0 + 2s);
      CHECK("Stop on 3rd sample", count(e3, GuardAction::Stop) == 1);
      CHECK("Stop event carries 80 % threshold", !e3.empty() && e3[0].threshold == 80.0); }

    std::puts("T5  one-sample spike does not stop a process");
    { ThresholdGuard g; GuardConfig c;
      g.evaluate({proc(100, "a.exe", 95)}, sys, c, t0);
      g.evaluate({proc(100, "a.exe", 10)}, sys, c, t0 + 1s);
      g.evaluate({proc(100, "a.exe", 95)}, sys, c, t0 + 2s);
      auto e = g.evaluate({proc(100, "a.exe", 95)}, sys, c, t0 + 3s);
      CHECK("counter reset by dip -> no Stop yet", count(e, GuardAction::Stop) == 0); }

    std::puts("T6  jump straight past 60 -> alert on first sample");
    { ThresholdGuard g; GuardConfig c;
      auto e = g.evaluate({proc(100, "a.exe", 97)}, sys, c, t0);
      CHECK("Warn on first sample at 97 %", count(e, GuardAction::Warn) == 1); }

    std::puts("T7  protected OS processes are never stopped");
    { ThresholdGuard g; GuardConfig c; std::vector<GuardEvent> all;
      for (int i = 0; i < 3; ++i) { auto e = g.evaluate({proc(900, "svchost.exe", 99)}, sys, c, t0 + std::chrono::seconds(i)); all.insert(all.end(), e.begin(), e.end()); }
      CHECK("StopSkipped reported", count(all, GuardAction::StopSkipped) == 1);
      CHECK("no Stop for svchost.exe", count(all, GuardAction::Stop) == 0);
      std::vector<GuardEvent> low;
      { ThresholdGuard g2; for (int i = 0; i < 3; ++i) { auto e = g2.evaluate({proc(4, "System", 99)}, sys, c, t0 + std::chrono::seconds(i)); low.insert(low.end(), e.begin(), e.end()); } }
      CHECK("PID <= 4 never stopped", count(low, GuardAction::Stop) == 0); }

    std::puts("T8  autoStop off -> alerts only");
    { ThresholdGuard g; GuardConfig c; c.autoStop = false; std::vector<GuardEvent> all;
      for (int i = 0; i < 5; ++i) { auto e = g.evaluate({proc(100, "a.exe", 99)}, sys, c, t0 + std::chrono::seconds(i)); all.insert(all.end(), e.begin(), e.end()); }
      CHECK("no Stop with autoStop=false", count(all, GuardAction::Stop) == 0);
      CHECK("still alerted", count(all, GuardAction::Warn) >= 1); }

    std::puts("T9  already-suspended process is ignored");
    { ThresholdGuard g; GuardConfig c;
      auto e = g.evaluate({proc(100, "a.exe", 99, 1000, 'T')}, sys, c, t0);
      CHECK("no events for state T", e.empty()); }

    std::puts("T10 resume grace period");
    { ThresholdGuard g; GuardConfig c; c.resumeGraceSec = 60;
      g.evaluate({proc(100, "a.exe", 0, 1000, 'T')}, sys, c, t0);                     // suspended
      std::vector<GuardEvent> in;
      for (int i = 1; i <= 4; ++i) { auto e = g.evaluate({proc(100, "a.exe", 99)}, sys, c, t0 + std::chrono::seconds(i)); in.insert(in.end(), e.begin(), e.end()); }
      CHECK("no Stop inside 60 s grace after resume", count(in, GuardAction::Stop) == 0);
      std::vector<GuardEvent> after;
      for (int i = 0; i < 4; ++i) { auto e = g.evaluate({proc(100, "a.exe", 99)}, sys, c, t0 + 70s + std::chrono::seconds(i)); after.insert(after.end(), e.begin(), e.end()); }
      CHECK("Stop again once grace has expired", count(after, GuardAction::Stop) == 1); }

    std::puts("T11 memory metric (% of total RAM)");
    { ThresholdGuard g; GuardConfig c; c.watchCpu = false;
      long rss = (long)(0.85 * sys.mem_total_kb);
      std::vector<GuardEvent> all;
      for (int i = 0; i < 3; ++i) { auto e = g.evaluate({proc(100, "leak.exe", 0, rss)}, sys, c, t0 + std::chrono::seconds(i)); all.insert(all.end(), e.begin(), e.end()); }
      CHECK("Stop for 85 % RAM", count(all, GuardAction::Stop) == 1);
      CHECK("event metric is MEM", !all.empty() && all.back().metric == GuardMetric::Memory); }

    std::puts("T12 CPU normalised by core count");
    { ThresholdGuard g; GuardConfig c; c.normalizeCpu = true;     // 8 cores: 400 % of a core = 50 % of machine
      auto e = g.evaluate({proc(100, "a.exe", 400)}, sys, c, t0);
      CHECK("400 % of one core = 50 % total -> silent", e.empty());
      auto e2 = g.evaluate({proc(101, "b.exe", 520)}, sys, c, t0);
      CHECK("520 % of one core = 65 % total -> Warn", count(e2, GuardAction::Warn) == 1); }

    std::puts("T13 alert re-arms after usage falls well below 60 %");
    { ThresholdGuard g; GuardConfig c;
      g.evaluate({proc(100, "a.exe", 65)}, sys, c, t0);
      g.evaluate({proc(100, "a.exe", 40)}, sys, c, t0 + 1s);
      auto e = g.evaluate({proc(100, "a.exe", 65)}, sys, c, t0 + 2s);
      CHECK("new Warn immediately after re-arm", count(e, GuardAction::Warn) == 1); }

    std::puts("T14 PID reuse by a different program resets state");
    { ThresholdGuard g; GuardConfig c;
      g.evaluate({proc(100, "a.exe", 85)}, sys, c, t0);
      g.evaluate({proc(100, "a.exe", 85)}, sys, c, t0 + 1s);
      auto e = g.evaluate({proc(100, "other.exe", 85)}, sys, c, t0 + 2s);
      CHECK("no Stop for the new owner of the PID", count(e, GuardAction::Stop) == 0); }

    std::puts("T15 guard disabled");
    { ThresholdGuard g; GuardConfig c; c.enabled = false; std::vector<GuardEvent> all;
      for (int i = 0; i < 5; ++i) { auto e = g.evaluate({proc(100, "a.exe", 99)}, sys, c, t0 + std::chrono::seconds(i)); all.insert(all.end(), e.begin(), e.end()); }
      CHECK("no events when disabled", all.empty()); }

    std::puts("T16 colour level helper");
    { GuardConfig c;
      CHECK("level 0 at 59", ThresholdGuard::level(proc(1, "a", 59), sys, c) == 0);
      CHECK("level 1 at 60", ThresholdGuard::level(proc(1, "a", 60), sys, c) == 1);
      CHECK("level 1 at 79.9", ThresholdGuard::level(proc(1, "a", 79.9), sys, c) == 1);
      CHECK("level 2 at 80", ThresholdGuard::level(proc(1, "a", 80), sys, c) == 2); }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
