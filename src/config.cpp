#include "config.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool parseBool(const std::string& v, bool& out) {
    std::string t = lower(v);
    if (t == "1" || t == "true" || t == "yes" || t == "on")  { out = true;  return true; }
    if (t == "0" || t == "false" || t == "no" || t == "off") { out = false; return true; }
    return false;
}
bool parseNum(const std::string& v, double& out) {
    char* end = nullptr; out = std::strtod(v.c_str(), &end);
    return end && *end == '\0' && !v.empty();
}
}

std::string defaultConfigPath() {
    if (const char* e = std::getenv("NOVAPM_CONFIG")) return e;
#ifdef _WIN32
    if (const char* a = std::getenv("APPDATA")) return std::string(a) + "\\NovaPM\\novapm.ini";
    return "novapm.ini";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME")) return std::string(x) + "/novapm/novapm.ini";
    if (const char* h = std::getenv("HOME")) return std::string(h) + "/.config/novapm/novapm.ini";
    return "novapm.ini";
#endif
}

void sanitizeConfig(AppConfig& c) {
    GuardConfig& g = c.guard;
    g.warnPct = std::clamp(g.warnPct, 1.0, 99.0);
    g.stopPct = std::clamp(g.stopPct, g.warnPct + 1.0, 100.0);
    g.stopSustain = std::clamp(g.stopSustain, 1, 60);
    g.alertCooldownSec = std::clamp(g.alertCooldownSec, 1, 3600);
    g.resumeGraceSec = std::clamp(g.resumeGraceSec, 0, 3600);
    c.refreshMs = std::clamp(c.refreshMs, 100, 60000);
    c.dbEverySec = std::clamp(c.dbEverySec, 1, 3600);
    if (c.dbPath.empty()) c.dbPath = "novapm.db";
}

bool setConfigValue(AppConfig& c, const std::string& rawKey, const std::string& value, std::string& err) {
    const std::string key = lower(trim(rawKey));
    bool b = false; double n = 0;
    auto needBool = [&]() { if (parseBool(value, b)) return true; err = "'" + key + "' expects true/false (got '" + value + "')"; return false; };
    auto needNum  = [&]() { if (parseNum(value, n))  return true; err = "'" + key + "' expects a number (got '" + value + "')"; return false; };
    GuardConfig& g = c.guard;
    if      (key == "enabled")          { if (!needBool()) return false; g.enabled = b; }
    else if (key == "auto_stop")        { if (!needBool()) return false; g.autoStop = b; }
    else if (key == "watch_cpu")        { if (!needBool()) return false; g.watchCpu = b; }
    else if (key == "watch_mem")        { if (!needBool()) return false; g.watchMem = b; }
    else if (key == "normalize_cpu")    { if (!needBool()) return false; g.normalizeCpu = b; }
    else if (key == "db_enabled")       { if (!needBool()) return false; c.dbEnabled = b; }
    else if (key == "warn_pct")         { if (!needNum())  return false; g.warnPct = n; }
    else if (key == "stop_pct")         { if (!needNum())  return false; g.stopPct = n; }
    else if (key == "stop_sustain")     { if (!needNum())  return false; g.stopSustain = (int)n; }
    else if (key == "alert_cooldown_s") { if (!needNum())  return false; g.alertCooldownSec = (int)n; }
    else if (key == "resume_grace_s")   { if (!needNum())  return false; g.resumeGraceSec = (int)n; }
    else if (key == "refresh_ms")       { if (!needNum())  return false; c.refreshMs = (int)n; }
    else if (key == "db_every_s")       { if (!needNum())  return false; c.dbEverySec = (int)n; }
    else if (key == "db_path")          { c.dbPath = value; }
    else { err = "unknown setting '" + key + "'"; return false; }
    return true;
}

bool loadConfig(const std::string& path, AppConfig& out, std::string* warnings) {
    out = AppConfig{};
    std::ifstream f(path);
    if (!f) return true;                                   // no file yet: defaults
    std::string line, warn; int ln = 0;
    while (std::getline(f, line)) {
        ++ln;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        auto eq = line.find('=');
        std::string err;
        if (eq == std::string::npos) { warn += "line " + std::to_string(ln) + ": expected key = value\n"; continue; }
        if (!setConfigValue(out, line.substr(0, eq), trim(line.substr(eq + 1)), err))
            warn += "line " + std::to_string(ln) + ": " + err + "\n";
    }
    sanitizeConfig(out);
    if (warnings) *warnings = warn;
    return true;
}

bool saveConfig(const std::string& path, const AppConfig& in, std::string* error) {
    AppConfig c = in; sanitizeConfig(c);
    std::error_code ec;
    fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) { if (error) *error = "cannot write " + path; return false; }
    auto yn = [](bool v) { return v ? "true" : "false"; };
    const GuardConfig& g = c.guard;
    f << "# NovaPM settings\n"
      << "# Threshold guard: alert at warn_pct, suspend the process at stop_pct (sustained for stop_sustain samples)\n"
      << "enabled = " << yn(g.enabled) << "\n"
      << "auto_stop = " << yn(g.autoStop) << "\n"
      << "warn_pct = " << g.warnPct << "\n"
      << "stop_pct = " << g.stopPct << "\n"
      << "stop_sustain = " << g.stopSustain << "\n"
      << "alert_cooldown_s = " << g.alertCooldownSec << "\n"
      << "resume_grace_s = " << g.resumeGraceSec << "\n"
      << "watch_cpu = " << yn(g.watchCpu) << "\n"
      << "watch_mem = " << yn(g.watchMem) << "\n"
      << "normalize_cpu = " << yn(g.normalizeCpu) << "\n"
      << "# Collector and database\n"
      << "refresh_ms = " << c.refreshMs << "\n"
      << "db_every_s = " << c.dbEverySec << "\n"
      << "db_enabled = " << yn(c.dbEnabled) << "\n"
      << "db_path = " << c.dbPath << "\n";
    return f.good();
}
