#pragma once
#include "threshold_guard.hpp"
#include <string>

// Persistent settings, stored as a plain  key = value  file (lines starting with # are comments).
struct AppConfig {
    GuardConfig guard;
    int  refreshMs   = 1000;        // collector interval
    int  dbEverySec  = 5;           // snapshot interval
    bool dbEnabled   = true;
    std::string dbPath = "novapm.db";
};

// $NOVAPM_CONFIG, else %APPDATA%\NovaPM\novapm.ini (Windows) or ~/.config/novapm/novapm.ini (Linux)
std::string defaultConfigPath();
// Missing file -> defaults and true. Unknown keys / bad values are reported in *warnings and skipped.
bool loadConfig(const std::string& path, AppConfig& out, std::string* warnings = nullptr);
bool saveConfig(const std::string& path, const AppConfig& cfg, std::string* error = nullptr);
// Set one key (names as in the file). Returns false + message for unknown key / invalid value.
bool setConfigValue(AppConfig& cfg, const std::string& key, const std::string& value, std::string& error);
// Clamp to sane ranges; guarantees stopPct > warnPct.
void sanitizeConfig(AppConfig& cfg);
