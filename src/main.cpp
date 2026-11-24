// NovaPM - process & resource monitor (C++20, Dear ImGui, SQLite). Linux (/proc) and Windows (Win32) back ends.
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "cli.hpp"
#include "config.hpp"
#include "database.hpp"
#include "monitor_core.hpp"
#include "process_collector.hpp"
#include "signal_manager.hpp"
#include "system_monitor.hpp"
#include "threshold_guard.hpp"

using namespace std::chrono_literals;

// ---------- state shared between worker thread and GUI thread ----------
struct Shared {
    std::mutex m;
    std::vector<ProcInfo> procs;
    SysStats sys;
    std::deque<float> cpuHist, memHist;
    GuardConfig cfg;                 // threshold-guard settings (edited by GUI, read by worker)
    std::deque<AlertRec> alerts;     // alert / auto-stop log (newest at back)
    int unack = 0;                   // alerts the user has not looked at yet
};
static std::atomic<bool> g_run{true};
static std::atomic<int>  g_intervalMs{1000};
static std::atomic<int>  g_dbEverySec{5};
static std::atomic<bool> g_dbEnabled{true};
static constexpr size_t kHistory = 120;
static constexpr size_t kMaxAlerts = 200;

// Execute one ThresholdGuard decision (see monitor_core.cpp) and publish it to the alert log.
static void applyGuardEvent(const GuardEvent& e, Shared& S, Database& db) {
    AlertRec a = applyGuardAction(e, db);
    std::lock_guard<std::mutex> lk(S.m);
    S.alerts.push_back(std::move(a));
    while (S.alerts.size() > kMaxAlerts) S.alerts.pop_front();
    ++S.unack;
}

// ---------- collector + persistence thread ----------
static void worker(Shared& S, Database& db) {
    ProcessCollector pc;
    SystemMonitor sm;
    ThresholdGuard guard;
    std::map<int, std::string> prev;
    bool first = true;
    auto lastDb = std::chrono::steady_clock::now() - 1h;
    auto lastPurge = std::chrono::steady_clock::now();

    while (g_run) {
        auto procs = pc.collect();
        int threads = 0;
        for (auto& p : procs) threads += p.threads;
        SysStats sys = sm.sample((int)procs.size(), threads);

        // lifecycle events (start / exit)
        std::map<int, std::string> cur;
        for (auto& p : procs) cur[p.pid] = p.name;
        if (!first && g_dbEnabled) logLifecycle(prev, cur, db);
        prev = std::move(cur);
        first = false;

        // threshold guard: alert at warnPct, suspend at stopPct (see threshold_guard.hpp)
        GuardConfig gcfg;
        { std::lock_guard<std::mutex> lk(S.m); gcfg = S.cfg; }
        for (const GuardEvent& ev : guard.evaluate(procs, sys, gcfg, std::chrono::steady_clock::now()))
            applyGuardEvent(ev, S, db);

        {
            std::lock_guard<std::mutex> lk(S.m);
            S.procs = procs;
            S.sys = sys;
            S.cpuHist.push_back((float)sys.cpu);
            S.memHist.push_back(sys.mem_total_kb ? 100.f * sys.mem_used_kb / sys.mem_total_kb : 0.f);
            if (S.cpuHist.size() > kHistory) S.cpuHist.pop_front();
            if (S.memHist.size() > kHistory) S.memHist.pop_front();
        }

        auto now = std::chrono::steady_clock::now();
        if (g_dbEnabled && now - lastDb >= std::chrono::seconds(g_dbEverySec.load())) {
            db.insertSnapshot(procs, sys);
            lastDb = now;
        }
        if (now - lastPurge > 10min) { db.purgeOlderThan(24); lastPurge = now; }  // retention: 24h

        for (int slept = 0; slept < g_intervalMs && g_run; slept += 50) std::this_thread::sleep_for(50ms);
    }
}

// ---------- GUI helpers ----------
static void renderResult(const char* id, const QueryResult& r) {
    if (!r.error.empty()) { ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "Error: %s", r.error.c_str()); return; }
    if (r.cols.empty()) { ImGui::TextDisabled("(no data yet)"); return; }
    if (ImGui::BeginTable(id, (int)r.cols.size(),
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 220))) {
        for (auto& c : r.cols) ImGui::TableSetupColumn(c.c_str());
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (auto& row : r.rows) {
            ImGui::TableNextRow();
            for (size_t i = 0; i < row.size(); ++i) { ImGui::TableSetColumnIndex((int)i); ImGui::TextUnformatted(row[i].c_str()); }
        }
        ImGui::EndTable();
    }
}

static bool containsNoCase(const std::string& h, const std::string& n) {
    if (n.empty()) return true;
    auto it = std::search(h.begin(), h.end(), n.begin(), n.end(),
        [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
    return it != h.end();
}

static void drawTree(const ProcInfo* p, const std::map<int, std::vector<const ProcInfo*>>& kids, int& sel) {
    auto it = kids.find(p->pid);
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (it == kids.end()) fl |= ImGuiTreeNodeFlags_Leaf;
    if (sel == p->pid) fl |= ImGuiTreeNodeFlags_Selected;
    bool open = ImGui::TreeNodeEx((void*)(intptr_t)p->pid, fl, "%s [%d]  cpu %.1f%%  mem %.1f MB",
                                  p->name.c_str(), p->pid, p->cpu, p->rss_kb / 1024.0);
    if (ImGui::IsItemClicked()) sel = p->pid;
    if (open) {
        if (it != kids.end()) for (auto* c : it->second) drawTree(c, kids, sel);
        ImGui::TreePop();
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && isCliCommand(argv[1])) return runCli(argc, argv);   // e.g.  novapm watch --warn 60 --stop 80

    const std::string cfgPath = defaultConfigPath();
    AppConfig appCfg;
    loadConfig(cfgPath, appCfg);
    std::string dbPath = argc > 1 ? argv[1] : appCfg.dbPath;
    Database db(dbPath);
    if (!db.ok()) { std::fprintf(stderr, "DB error: %s\n", db.error().c_str()); return 1; }

    if (!glfwInit()) return 1;
    GLFWwindow* win = glfwCreateWindow(1280, 760, "NovaPM - Process & Resource Monitor", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    Shared S;
    S.cfg = appCfg.guard;
    g_intervalMs = appCfg.refreshMs; g_dbEverySec = appCfg.dbEverySec; g_dbEnabled = appCfg.dbEnabled;
    std::thread th(worker, std::ref(S), std::ref(db));

    int selPid = -1;
    std::string status;
    char filter[64] = "";
    char sql[1024] = "SELECT * FROM system_stats ORDER BY ts DESC LIMIT 20";
    QueryResult userRes, topCpu, topMem, events;
    auto lastHist = std::chrono::steady_clock::now() - 1h;
    bool alertsTabOpen = false;      // alert list is only copied while its tab is visible
    // Details tab state (refreshed on selection change and every 2 s)
    int detPid = -1; ProcDetails det; bool detOk = false; std::vector<HistPoint> hist; int histMinutes = 10;
    auto detTime = std::chrono::steady_clock::now() - 1h;
    auto saveSettings = [&]() {
        GuardConfig gc; { std::lock_guard<std::mutex> lk(S.m); gc = S.cfg; }
        appCfg.guard = gc; appCfg.refreshMs = g_intervalMs; appCfg.dbEverySec = g_dbEverySec; appCfg.dbEnabled = g_dbEnabled;
        std::string err;
        return saveConfig(cfgPath, appCfg, &err) ? "Settings saved to " + cfgPath : "Could not save settings: " + err;
    };

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // snapshot of shared data (short lock)
        std::vector<ProcInfo> view; SysStats sys;
        std::vector<float> cpuH, memH;
        GuardConfig cfg; std::vector<AlertRec> alertList; AlertRec lastAlert; int unack = 0;
        {
            std::lock_guard<std::mutex> lk(S.m);
            view = S.procs; sys = S.sys;
            cfg = S.cfg; unack = S.unack;
            if (!S.alerts.empty()) lastAlert = S.alerts.back();
            if (alertsTabOpen) alertList.assign(S.alerts.begin(), S.alerts.end());
            cpuH.assign(S.cpuHist.begin(), S.cpuHist.end());
            memH.assign(S.memHist.begin(), S.memHist.end());
        }

        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("NovaPM", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::Text("CPU %.1f%%   RAM %.0f/%.0f MB   Swap %.0f/%.0f MB   Procs %d   Threads %d   Uptime %dh %dm",
            sys.cpu, sys.mem_used_kb / 1024.0, sys.mem_total_kb / 1024.0,
            sys.swap_used_kb / 1024.0, sys.swap_total_kb / 1024.0, sys.procs, sys.threads,
            (int)sys.uptime / 3600, ((int)sys.uptime % 3600) / 60);
        ImGui::Separator();

        // alert banner: stays until acknowledged (or the Alerts tab is opened)
        if (unack > 0) {
            const bool warnOnly = lastAlert.level == "WARNING";
            ImGui::PushStyleColor(ImGuiCol_Text, warnOnly ? ImVec4(1.f, .8f, .2f, 1.f) : ImVec4(1.f, .4f, .35f, 1.f));
            ImGui::Text("[!] %d new alert(s). Latest: %s - %s", unack, lastAlert.level.c_str(), lastAlert.msg.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::SmallButton("Acknowledge")) { std::lock_guard<std::mutex> lk(S.m); S.unack = 0; }
            ImGui::Separator();
        }

        // signal action bar (used by Processes + Tree tabs)
        auto actionBar = [&]() {
            const ProcInfo* sp = nullptr;
            for (auto& p : view) if (p.pid == selPid) { sp = &p; break; }
            ImGui::Text("Selected: %s", sp ? (sp->name + " [" + std::to_string(sp->pid) + "]").c_str() : "none");
            struct { const char* label; int sig; } btns[] = {
                {"Terminate (close window)", NPM_SIGTERM}, {"Stop (suspend)", NPM_SIGSTOP},
                {"Continue (resume)", NPM_SIGCONT}, {"Kill (force)", NPM_SIGKILL}};
            for (auto& b : btns) {
                ImGui::SameLine();
                if (ImGui::Button(b.label) && sp) {
                    auto r = SignalManager::send(sp->pid, b.sig);
                    status = r.msg;
                    db.logEvent(sp->pid, sp->name, "signal",
                                std::string(SignalManager::name(b.sig)) + (r.ok ? " ok" : " failed: " + r.msg));
                }
            }
            if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());
        };

        if (ImGui::BeginTabBar("tabs")) {
            // ---------------- Processes ----------------
            if (ImGui::BeginTabItem("Processes")) {
                ImGui::SetNextItemWidth(220);
                ImGui::InputTextWithHint("##f", "filter by name / pid / user / container", filter, sizeof filter);
                std::vector<ProcInfo> rows;
                for (auto& p : view)
                    if (containsNoCase(p.name, filter) || containsNoCase(std::to_string(p.pid), filter) ||
                        containsNoCase(p.user, filter) || containsNoCase(p.container, filter)) rows.push_back(p);

                if (ImGui::BeginTable("procs", 9,
                        ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp,
                        ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.6f))) {
                    ImGui::TableSetupColumn("PID");
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("User");
                    ImGui::TableSetupColumn("State");
                    ImGui::TableSetupColumn("CPU %", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending);
                    ImGui::TableSetupColumn("Mem (MB)", ImGuiTableColumnFlags_PreferSortDescending);
                    ImGui::TableSetupColumn("Threads", ImGuiTableColumnFlags_PreferSortDescending);
                    ImGui::TableSetupColumn("PPID");
                    ImGui::TableSetupColumn("Container");
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableHeadersRow();

                    if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
                        std::sort(rows.begin(), rows.end(), [&](const ProcInfo& a, const ProcInfo& b) {
                            for (int n = 0; n < sp->SpecsCount; ++n) {
                                const auto& s = sp->Specs[n];
                                int d = 0;
                                switch (s.ColumnIndex) {
                                    case 0: d = (a.pid > b.pid) - (a.pid < b.pid); break;
                                    case 1: d = a.name.compare(b.name); break;
                                    case 2: d = a.user.compare(b.user); break;
                                    case 3: d = a.state - b.state; break;
                                    case 4: d = (a.cpu > b.cpu) - (a.cpu < b.cpu); break;
                                    case 5: d = (a.rss_kb > b.rss_kb) - (a.rss_kb < b.rss_kb); break;
                                    case 6: d = a.threads - b.threads; break;
                                    case 7: d = a.ppid - b.ppid; break;
                                    case 8: d = a.container.compare(b.container); break;
                                }
                                if (d) return s.SortDirection == ImGuiSortDirection_Ascending ? d < 0 : d > 0;
                            }
                            return a.pid < b.pid;
                        });
                    }

                    ImGuiListClipper clip;
                    clip.Begin((int)rows.size());
                    while (clip.Step()) {
                        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                            const ProcInfo& p = rows[i];
                            ImGui::TableNextRow();
                            {   // colour rows by guard level: yellow >= alert threshold, red >= stop threshold, blue = suspended
                                ImU32 bg = 0;
                                if (p.state == 'T') bg = IM_COL32(40, 70, 140, 120);
                                else {
                                    int lv = ThresholdGuard::level(p, sys, cfg);
                                    if (lv == 2) bg = IM_COL32(160, 30, 30, 140);
                                    else if (lv == 1) bg = IM_COL32(150, 120, 0, 110);
                                }
                                if (bg) { ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bg); ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, bg); }
                            }
                            ImGui::TableSetColumnIndex(0);
                            char lbl[32]; std::snprintf(lbl, sizeof lbl, "%d", p.pid);
                            if (ImGui::Selectable(lbl, selPid == p.pid, ImGuiSelectableFlags_SpanAllColumns)) selPid = p.pid;
                            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(p.name.c_str());
                            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(p.user.c_str());
                            ImGui::TableSetColumnIndex(3); ImGui::Text("%c", p.state);
                            ImGui::TableSetColumnIndex(4); ImGui::Text("%.1f", p.cpu);
                            ImGui::TableSetColumnIndex(5); ImGui::Text("%.1f", p.rss_kb / 1024.0);
                            ImGui::TableSetColumnIndex(6); ImGui::Text("%d", p.threads);
                            ImGui::TableSetColumnIndex(7); ImGui::Text("%d", p.ppid);
                            ImGui::TableSetColumnIndex(8); ImGui::TextUnformatted(p.container.c_str());
                        }
                    }
                    ImGui::EndTable();
                }
                actionBar();
                ImGui::EndTabItem();
            }

            // ---------------- Details (selected process) ----------------
            if (ImGui::BeginTabItem("Details")) {
                actionBar();
                ImGui::Separator();
                const ProcInfo* sp = nullptr;
                for (auto& p : view) if (p.pid == selPid) { sp = &p; break; }
                if (!sp) {
                    ImGui::TextDisabled("Select a process in the Processes or Process Tree tab first.");
                } else {
                    auto nowT = std::chrono::steady_clock::now();
                    if (detPid != sp->pid || nowT - detTime > 2s) {
                        detOk = ProcessCollector::details(sp->pid, det);
                        hist = db.processHistory(sp->pid, histMinutes * 60);
                        detPid = sp->pid; detTime = nowT;
                    }
                    std::string parent = "-";
                    for (auto& q : view) if (q.pid == sp->ppid) { parent = q.name + " [" + std::to_string(q.pid) + "]"; break; }
                    int children = 0; for (auto& q : view) if (q.ppid == sp->pid) ++children;

                    ImGui::Text("%s  [PID %d]", sp->name.c_str(), sp->pid);
                    if (ImGui::BeginTable("det", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                        auto row = [&](const char* k, const std::string& v) {
                            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(k);
                            ImGui::TableSetColumnIndex(1); ImGui::TextWrapped("%s", v.c_str()); };
                        char b[64];
                        row("User", sp->user);
                        row("State", std::string(1, sp->state) + (sp->state == 'T' ? "  (suspended)" : ""));
                        row("Parent", parent + "   (children: " + std::to_string(children) + ")");
                        std::snprintf(b, sizeof b, "%.1f %% of one core (%.1f %% of machine)", sp->cpu, sys.cores ? sp->cpu / sys.cores : sp->cpu); row("CPU", b);
                        std::snprintf(b, sizeof b, "%.1f MB (%.1f %% of RAM)", sp->rss_kb / 1024.0, sys.mem_total_kb ? 100.0 * sp->rss_kb / sys.mem_total_kb : 0.0); row("Memory", b);
                        row("Threads", std::to_string(sp->threads));
                        if (detOk) {
                            row("Executable", det.exe);
                            row("Command line", det.cmdline);
                            row("Started", det.started.empty() ? "?" : det.started);
                            row("Priority", det.priority);
                            row(
#ifdef _WIN32
                                "Handles",
#else
                                "Open files",
#endif
                                det.handles < 0 ? "?" : std::to_string(det.handles));
                        } else row("Details", "not accessible (try Administrator / sudo)");
                        row("Container", sp->container.empty() ? "(host process)" : sp->container);
                        int lv = ThresholdGuard::level(*sp, sys, cfg);
                        std::snprintf(b, sizeof b, "alert >= %.0f %%, stop >= %.0f %% (%s)", cfg.warnPct, cfg.stopPct, cfg.autoStop ? "auto-stop on" : "alert only");
                        row("Guard", std::string(!cfg.enabled ? "disabled" : lv == 2 ? "AT STOP LEVEL" : lv == 1 ? "AT ALERT LEVEL" : "normal") + " - " + b);
                        ImGui::EndTable();
                    }

                    ImGui::Separator();
                    int mins = histMinutes;
                    ImGui::SetNextItemWidth(160);
                    if (ImGui::SliderInt("History window (min)", &mins, 1, 60)) { histMinutes = mins; detTime = nowT - 1h; }
                    if (hist.size() < 2) {
                        ImGui::TextDisabled("Not enough stored samples yet (snapshots are written every %d s while 'Write to database' is on).", g_dbEverySec.load());
                    } else {
                        std::vector<float> cpuV, memV;
                        float cpuMax = 0, memMax = 0;
                        for (auto& h : hist) { cpuV.push_back((float)h.cpu); memV.push_back(h.rss_kb / 1024.f); cpuMax = std::max(cpuMax, cpuV.back()); memMax = std::max(memMax, memV.back()); }
                        char ov[64];
                        std::snprintf(ov, sizeof ov, "CPU %% (peak %.1f)", cpuMax);
                        ImGui::PlotLines("##hc", cpuV.data(), (int)cpuV.size(), 0, ov, 0.f, FLT_MAX, ImVec2(-1, 90));
                        std::snprintf(ov, sizeof ov, "Memory MB (peak %.1f)", memMax);
                        ImGui::PlotLines("##hm", memV.data(), (int)memV.size(), 0, ov, 0.f, FLT_MAX, ImVec2(-1, 90));
                        ImGui::TextDisabled("%zu stored samples from the database", hist.size());
                    }
                }
                ImGui::EndTabItem();
            }

            // ---------------- Tree ----------------
            if (ImGui::BeginTabItem("Process Tree")) {
                std::set<int> pids; for (auto& p : view) pids.insert(p.pid);
                std::map<int, std::vector<const ProcInfo*>> kids;
                std::vector<const ProcInfo*> roots;
                for (auto& p : view) (pids.count(p.ppid) && p.ppid != p.pid ? kids[p.ppid] : roots).push_back(&p);
                ImGui::BeginChild("tree", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.6f), true);
                for (auto* r : roots) drawTree(r, kids, selPid);
                ImGui::EndChild();
                actionBar();
                ImGui::EndTabItem();
            }

            // ---------------- Alerts / Threshold guard ----------------
            {
                char alertsLabel[48];
                std::snprintf(alertsLabel, sizeof alertsLabel, unack ? "Alerts (%d)###alerts" : "Alerts###alerts", unack);
                alertsTabOpen = false;
                if (ImGui::BeginTabItem(alertsLabel)) {
                    alertsTabOpen = true;
                    if (unack) { std::lock_guard<std::mutex> lk(S.m); S.unack = 0; }   // user is looking at them

                    GuardConfig c = cfg; bool changed = false;
                    changed |= ImGui::Checkbox("Enable threshold guard", &c.enabled);
                    ImGui::SameLine();
                    changed |= ImGui::Checkbox("Auto-stop at stop threshold", &c.autoStop);
                    changed |= ImGui::Checkbox("Watch CPU", &c.watchCpu);
                    ImGui::SameLine();
                    changed |= ImGui::Checkbox("Watch memory (% of RAM)", &c.watchMem);
                    ImGui::SameLine();
                    changed |= ImGui::Checkbox("CPU as % of whole machine (else % of one core)", &c.normalizeCpu);

                    float warn = (float)c.warnPct, stop = (float)c.stopPct;
                    if (ImGui::SliderFloat("Alert threshold (%)", &warn, 10.f, 99.f, "%.0f")) { c.warnPct = warn; changed = true; }
                    if (ImGui::SliderFloat("Stop threshold (%)", &stop, 11.f, 100.f, "%.0f")) { c.stopPct = stop; changed = true; }
                    if (c.stopPct < c.warnPct + 1) c.stopPct = c.warnPct + 1;            // stop must stay above alert
                    changed |= ImGui::SliderInt("Samples above stop threshold before stopping", &c.stopSustain, 1, 10);
                    changed |= ImGui::SliderInt("Alert cooldown per process (s)", &c.alertCooldownSec, 5, 300);
                    changed |= ImGui::SliderInt("No auto-stop after Resume for (s)", &c.resumeGraceSec, 0, 600);
                    if (ImGui::Button("Reset to defaults (60 / 80)")) { c = GuardConfig{}; changed = true; }
                    ImGui::SameLine();
                    if (ImGui::Button("Save settings")) status = saveSettings();
                    ImGui::SameLine();
                    if (ImGui::Button("Clear alert list")) { std::lock_guard<std::mutex> lk(S.m); S.alerts.clear(); S.unack = 0; }
                    if (changed) { std::lock_guard<std::mutex> lk(S.m); S.cfg = c; }

                    ImGui::TextDisabled("Usage >= %.0f%% raises an alert. Usage >= %.0f%% for %d samples suspends the process (SIGSTOP). "
                                        "System-critical processes are never stopped.", c.warnPct, c.stopPct, c.stopSustain);
                    ImGui::TextDisabled("History:  SELECT name, action, COUNT(*) FROM alerts GROUP BY name, action   (see 'History / SQL')");
                    ImGui::Separator();

                    if (ImGui::BeginTable("alerttbl", 8,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 0))) {
                        ImGui::TableSetupColumn("Time");  ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("PID");
                        ImGui::TableSetupColumn("Process"); ImGui::TableSetupColumn("Metric"); ImGui::TableSetupColumn("Usage %");
                        ImGui::TableSetupColumn("Message"); ImGui::TableSetupColumn("Action");
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableHeadersRow();
                        for (int i = (int)alertList.size() - 1; i >= 0; --i) {       // newest first
                            const AlertRec& a = alertList[i];
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(a.time.c_str());
                            ImGui::TableSetColumnIndex(1);
                            ImVec4 col = a.level == "WARNING" ? ImVec4(1.f, .8f, .2f, 1.f)
                                       : a.level == "AUTO-STOP" ? ImVec4(1.f, .4f, .35f, 1.f) : ImVec4(.8f, .6f, 1.f, 1.f);
                            ImGui::TextColored(col, "%s", a.level.c_str());
                            ImGui::TableSetColumnIndex(2); ImGui::Text("%d", a.pid);
                            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(a.name.c_str());
                            ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(a.metric.c_str());
                            ImGui::TableSetColumnIndex(5); ImGui::Text("%.1f", a.value);
                            ImGui::TableSetColumnIndex(6); ImGui::TextWrapped("%s", a.msg.c_str());
                            ImGui::TableSetColumnIndex(7);
                            bool suspended = false;
                            for (auto& p : view) if (p.pid == a.pid && p.name == a.name && p.state == 'T') { suspended = true; break; }
                            if (a.level == "AUTO-STOP" && suspended) {
                                ImGui::PushID(i);
                                if (ImGui::SmallButton("Resume")) {
                                    auto r = SignalManager::send(a.pid, NPM_SIGCONT);
                                    status = r.msg;
                                    db.logEvent(a.pid, a.name, "signal", std::string("SIGCONT ") + (r.ok ? "ok (resume after auto-stop)" : "failed: " + r.msg));
                                }
                                ImGui::PopID();
                            }
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
            }

            // ---------------- System ----------------
            if (ImGui::BeginTabItem("System")) {
                ImGui::Text("CPU usage");
                ImGui::PlotLines("##cpu", cpuH.data(), (int)cpuH.size(), 0, nullptr, 0.f, 100.f, ImVec2(-1, 130));
                ImGui::Text("Memory usage (%%)");
                ImGui::PlotLines("##mem", memH.data(), (int)memH.size(), 0, nullptr, 0.f, 100.f, ImVec2(-1, 130));
                float swapFrac = sys.swap_total_kb ? (float)sys.swap_used_kb / sys.swap_total_kb : 0.f;
                ImGui::Text("Swap"); ImGui::ProgressBar(swapFrac, ImVec2(-1, 0));
                ImGui::Separator();
                int iv = g_intervalMs, ds = g_dbEverySec; bool en = g_dbEnabled;
                if (ImGui::SliderInt("Refresh interval (ms)", &iv, 250, 5000)) g_intervalMs = iv;
                if (ImGui::SliderInt("DB snapshot every (s)", &ds, 1, 60)) g_dbEverySec = ds;
                if (ImGui::Checkbox("Write to database", &en)) g_dbEnabled = en;
                ImGui::EndTabItem();
            }

            // ---------------- History (SQL) ----------------
            if (ImGui::BeginTabItem("History / SQL")) {
                auto now = std::chrono::steady_clock::now();
                if (ImGui::Button("Refresh") || now - lastHist > 3s) {
                    topCpu = db.query("SELECT m.name, ROUND(AVG(s.cpu),2) AS avg_cpu, ROUND(AVG(s.rss_kb)/1024.0,1) AS avg_mem_mb, "
                                      "COUNT(*) AS samples FROM snapshots s JOIN process_meta m ON m.pid=s.pid "
                                      "GROUP BY m.pid ORDER BY avg_cpu DESC LIMIT 15");
                    topMem = db.query("SELECT m.name, ROUND(AVG(s.rss_kb)/1024.0,1) AS avg_mem_mb, ROUND(AVG(s.cpu),2) AS avg_cpu, "
                                      "COUNT(*) AS samples FROM snapshots s JOIN process_meta m ON m.pid=s.pid "
                                      "GROUP BY m.pid ORDER BY avg_mem_mb DESC LIMIT 15");
                    lastHist = now;
                }
                ImGui::Text("Top processes by average CPU");   renderResult("topcpu", topCpu);
                ImGui::Text("Top processes by average memory"); renderResult("topmem", topMem);
                ImGui::Separator();
                ImGui::Text("SQL console (read-only)");
                ImGui::InputTextMultiline("##sql", sql, sizeof sql, ImVec2(-1, 60));
                if (ImGui::Button("Run query")) userRes = db.query(sql);
                renderResult("userres", userRes);
                ImGui::TextDisabled("Tables: process_meta, snapshots, system_stats, events, alerts");
                ImGui::EndTabItem();
            }

            // ---------------- Events ----------------
            if (ImGui::BeginTabItem("Events")) {
                if (ImGui::Button("Refresh##ev") || events.cols.empty())
                    events = db.query("SELECT datetime(ts,'unixepoch','localtime') AS time, pid, name, event, detail "
                                      "FROM events ORDER BY id DESC LIMIT 200");
                renderResult("events", events);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();

        ImGui::Render();
        int w, h; glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.08f, 0.08f, 0.1f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    { std::string m = saveSettings(); std::fprintf(stderr, "%s\n", m.c_str()); }
    g_run = false;
    th.join();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
