#pragma once
#include "types.hpp"
#include <mutex>
#include <string>
#include <vector>
#include <sqlite3.h>

struct QueryResult {
    std::vector<std::string> cols;
    std::vector<std::vector<std::string>> rows;
    std::string error;
};

struct HistPoint { long long ts = 0; double cpu = 0; long rss_kb = 0; };

// SQLite persistence. All public methods are guarded by one mutex, so the
// collector thread (writer) and the GUI thread (reader) can share it safely.
class Database {
public:
    explicit Database(const std::string& path);
    ~Database();
    bool ok() const { return ok_; }
    const std::string& error() const { return err_; }

    void insertSnapshot(const std::vector<ProcInfo>& procs, const SysStats& sys);
    void logEvent(int pid, const std::string& name, const std::string& event, const std::string& detail);
    void logAlert(int pid, const std::string& name, const std::string& metric, const std::string& action,
                  double value, double threshold, const std::string& result);
    // Stored CPU / memory samples of one process (oldest first) for the Details graphs and `novapm-cli history`.
    std::vector<HistPoint> processHistory(int pid, int seconds, int maxPoints = 2000);
    void purgeOlderThan(int hours);
    QueryResult query(const std::string& sql, int maxRows = 500);   // read-only only

private:
    bool exec(const char* sql);
    sqlite3* db_ = nullptr;
    bool ok_ = false;
    std::string err_;
    std::mutex m_;
};
