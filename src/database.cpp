#include "database.hpp"
#include <algorithm>
#include <ctime>

namespace {
struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
    ~Stmt() { sqlite3_finalize(s); }
};
const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS process_meta(
    pid INTEGER PRIMARY KEY, name TEXT, user TEXT, first_seen INTEGER, last_seen INTEGER, container TEXT);
CREATE TABLE IF NOT EXISTS snapshots(
    id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL,
    pid INTEGER NOT NULL, cpu REAL, rss_kb INTEGER, threads INTEGER, state TEXT);
CREATE TABLE IF NOT EXISTS system_stats(
    ts INTEGER PRIMARY KEY, cpu REAL, mem_used_kb INTEGER, mem_total_kb INTEGER,
    swap_used_kb INTEGER, swap_total_kb INTEGER, uptime REAL, procs INTEGER, threads INTEGER);
CREATE TABLE IF NOT EXISTS events(
    id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL,
    pid INTEGER, name TEXT, event TEXT, detail TEXT);
CREATE TABLE IF NOT EXISTS alerts(
    id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL,
    pid INTEGER, name TEXT, metric TEXT, action TEXT,
    value REAL, threshold REAL, result TEXT);
CREATE INDEX IF NOT EXISTS idx_alerts_ts   ON alerts(ts);
CREATE INDEX IF NOT EXISTS idx_snap_pid_ts ON snapshots(pid, ts);
CREATE INDEX IF NOT EXISTS idx_snap_ts     ON snapshots(ts);
CREATE INDEX IF NOT EXISTS idx_events_ts   ON events(ts);
)SQL";
}

Database::Database(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) { err_ = sqlite3_errmsg(db_); return; }
    exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
    ok_ = exec(kSchema);
    { char* e = nullptr; sqlite3_exec(db_, "ALTER TABLE process_meta ADD COLUMN container TEXT", nullptr, nullptr, &e); sqlite3_free(e); }  // upgrade old files; error = already there
}

Database::~Database() { if (db_) sqlite3_close(db_); }

bool Database::exec(const char* sql) {
    char* e = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &e) != SQLITE_OK) {
        err_ = e ? e : "sqlite error"; sqlite3_free(e); return false;
    }
    return true;
}

void Database::insertSnapshot(const std::vector<ProcInfo>& procs, const SysStats& s) {
    std::lock_guard<std::mutex> lk(m_);
    if (!ok_) return;
    const sqlite3_int64 ts = std::time(nullptr);
    exec("BEGIN");
    {
        Stmt meta(db_, "INSERT INTO process_meta(pid,name,user,first_seen,last_seen,container) VALUES(?,?,?,?,?,?) "
                       "ON CONFLICT(pid) DO UPDATE SET name=excluded.name,last_seen=excluded.last_seen,container=excluded.container");
        Stmt snap(db_, "INSERT INTO snapshots(ts,pid,cpu,rss_kb,threads,state) VALUES(?,?,?,?,?,?)");
        for (const auto& p : procs) {
            sqlite3_reset(meta.s);
            sqlite3_bind_int(meta.s, 1, p.pid);
            sqlite3_bind_text(meta.s, 2, p.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(meta.s, 3, p.user.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(meta.s, 4, ts);
            sqlite3_bind_int64(meta.s, 5, ts);
            sqlite3_bind_text(meta.s, 6, p.container.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(meta.s);

            sqlite3_reset(snap.s);
            sqlite3_bind_int64(snap.s, 1, ts);
            sqlite3_bind_int(snap.s, 2, p.pid);
            sqlite3_bind_double(snap.s, 3, p.cpu);
            sqlite3_bind_int64(snap.s, 4, p.rss_kb);
            sqlite3_bind_int(snap.s, 5, p.threads);
            sqlite3_bind_text(snap.s, 6, std::string(1, p.state).c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(snap.s);
        }
        Stmt sys(db_, "INSERT OR REPLACE INTO system_stats VALUES(?,?,?,?,?,?,?,?,?)");
        sqlite3_bind_int64(sys.s, 1, ts);
        sqlite3_bind_double(sys.s, 2, s.cpu);
        sqlite3_bind_int64(sys.s, 3, s.mem_used_kb);
        sqlite3_bind_int64(sys.s, 4, s.mem_total_kb);
        sqlite3_bind_int64(sys.s, 5, s.swap_used_kb);
        sqlite3_bind_int64(sys.s, 6, s.swap_total_kb);
        sqlite3_bind_double(sys.s, 7, s.uptime);
        sqlite3_bind_int(sys.s, 8, s.procs);
        sqlite3_bind_int(sys.s, 9, s.threads);
        sqlite3_step(sys.s);
    }
    exec("COMMIT");
}

void Database::logEvent(int pid, const std::string& name, const std::string& event, const std::string& detail) {
    std::lock_guard<std::mutex> lk(m_);
    if (!ok_) return;
    Stmt st(db_, "INSERT INTO events(ts,pid,name,event,detail) VALUES(?,?,?,?,?)");
    sqlite3_bind_int64(st.s, 1, std::time(nullptr));
    sqlite3_bind_int(st.s, 2, pid);
    sqlite3_bind_text(st.s, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 4, event.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 5, detail.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st.s);
}

void Database::logAlert(int pid, const std::string& name, const std::string& metric, const std::string& action,
                        double value, double threshold, const std::string& result) {
    std::lock_guard<std::mutex> lk(m_);
    if (!ok_) return;
    Stmt st(db_, "INSERT INTO alerts(ts,pid,name,metric,action,value,threshold,result) VALUES(?,?,?,?,?,?,?,?)");
    sqlite3_bind_int64(st.s, 1, std::time(nullptr));
    sqlite3_bind_int(st.s, 2, pid);
    sqlite3_bind_text(st.s, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 4, metric.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 5, action.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st.s, 6, value);
    sqlite3_bind_double(st.s, 7, threshold);
    sqlite3_bind_text(st.s, 8, result.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st.s);
}

std::vector<HistPoint> Database::processHistory(int pid, int seconds, int maxPoints) {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<HistPoint> out;
    if (!ok_) return out;
    Stmt st(db_, "SELECT ts,cpu,rss_kb FROM snapshots WHERE pid=? AND ts>=? ORDER BY ts DESC LIMIT ?");
    sqlite3_bind_int(st.s, 1, pid);
    sqlite3_bind_int64(st.s, 2, (sqlite3_int64)std::time(nullptr) - seconds);
    sqlite3_bind_int(st.s, 3, maxPoints);
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back({sqlite3_column_int64(st.s, 0), sqlite3_column_double(st.s, 1), (long)sqlite3_column_int64(st.s, 2)});
    std::reverse(out.begin(), out.end());           // newest-first query -> oldest-first result
    return out;
}

void Database::purgeOlderThan(int hours) {
    std::lock_guard<std::mutex> lk(m_);
    if (!ok_) return;
    sqlite3_int64 cutoff = std::time(nullptr) - sqlite3_int64(hours) * 3600;
    for (const char* sql : {"DELETE FROM snapshots WHERE ts<?", "DELETE FROM system_stats WHERE ts<?",
                            "DELETE FROM events WHERE ts<?", "DELETE FROM alerts WHERE ts<?"}) {
        Stmt st(db_, sql);
        sqlite3_bind_int64(st.s, 1, cutoff);
        sqlite3_step(st.s);
    }
}

QueryResult Database::query(const std::string& sql, int maxRows) {
    std::lock_guard<std::mutex> lk(m_);
    QueryResult r;
    if (!ok_) { r.error = "Database not open"; return r; }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        r.error = sqlite3_errmsg(db_); return r;
    }
    if (!st) { r.error = "Empty statement"; return r; }
    if (!sqlite3_stmt_readonly(st)) {
        r.error = "Only read-only statements (SELECT) are allowed";
        sqlite3_finalize(st); return r;
    }
    int n = sqlite3_column_count(st);
    for (int i = 0; i < n; ++i) r.cols.push_back(sqlite3_column_name(st, i));
    while (sqlite3_step(st) == SQLITE_ROW && (int)r.rows.size() < maxRows) {
        std::vector<std::string> row;
        for (int i = 0; i < n; ++i) {
            const unsigned char* t = sqlite3_column_text(st, i);
            row.emplace_back(t ? reinterpret_cast<const char*>(t) : "NULL");
        }
        r.rows.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    return r;
}
