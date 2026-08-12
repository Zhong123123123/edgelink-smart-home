#include "storage/sqlite_store.hpp"

#include "ota/ota_task.hpp"

#include <sqlite3.h>
#include <filesystem>

namespace sg::storage {
namespace {
constexpr std::size_t kMaxPersistedEvents = 5000;

bool bindText(sqlite3_stmt* stmt, int idx, const std::string& v) {
    return sqlite3_bind_text(stmt, idx, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}
} // namespace

PersistentStore::PersistentStore(std::string file_path) : file_path_(std::move(file_path)) {
    const auto parent = std::filesystem::path(file_path_).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    std::string err;
    if (sqlite3_open(file_path_.c_str(), &db_) != SQLITE_OK) {
        db_ = nullptr;
        return;
    }
    (void)initSchema(err);
}

PersistentStore::~PersistentStore() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool PersistentStore::execSql(const char* sql, std::string& err) const {
    char* msg = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &msg);
    if (rc != SQLITE_OK) {
        err = (msg != nullptr) ? msg : "sqlite exec failed";
        if (msg != nullptr) sqlite3_free(msg);
        return false;
    }
    return true;
}

bool PersistentStore::initSchema(std::string& err) {
    if (db_ == nullptr) {
        err = "sqlite db not open";
        return false;
    }
    const char* ddl_tasks =
        "CREATE TABLE IF NOT EXISTS ota_tasks ("
        "task_uuid TEXT PRIMARY KEY,"
        "device_id INTEGER NOT NULL,"
        "device_type TEXT NOT NULL,"
        "firmware_id TEXT NOT NULL,"
        "state TEXT NOT NULL,"
        "last_error TEXT NOT NULL"
        ");";
    const char* ddl_events =
        "CREATE TABLE IF NOT EXISTS ota_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "task_uuid TEXT NOT NULL,"
        "ts_unix_ms INTEGER NOT NULL,"
        "state TEXT NOT NULL,"
        "detail TEXT NOT NULL"
        ");";
    const char* idx_events =
        "CREATE INDEX IF NOT EXISTS idx_ota_events_task_ts ON ota_events(task_uuid, ts_unix_ms);";

    return execSql(ddl_tasks, err) && execSql(ddl_events, err) && execSql(idx_events, err);
}

bool PersistentStore::saveTask(const sg::ota::OtaTask& task, std::string& err) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        err = "sqlite db not open";
        return false;
    }

    const char* sql =
        "INSERT INTO ota_tasks(task_uuid,device_id,device_type,firmware_id,state,last_error) "
        "VALUES(?,?,?,?,?,?) "
        "ON CONFLICT(task_uuid) DO UPDATE SET "
        "device_id=excluded.device_id,device_type=excluded.device_type,firmware_id=excluded.firmware_id,state=excluded.state,last_error=excluded.last_error;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = "prepare saveTask failed";
        return false;
    }

    const bool ok =
        bindText(stmt, 1, task.task_uuid) &&
        sqlite3_bind_int(stmt, 2, static_cast<int>(task.device_id)) == SQLITE_OK &&
        bindText(stmt, 3, task.device_type) &&
        bindText(stmt, 4, task.firmware_id) &&
        bindText(stmt, 5, sg::ota::toString(task.state)) &&
        bindText(stmt, 6, task.last_error);

    if (!ok) {
        sqlite3_finalize(stmt);
        err = "bind saveTask failed";
        return false;
    }

    const int step_rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step_rc != SQLITE_DONE) {
        err = "execute saveTask failed";
        return false;
    }
    return true;
}

std::vector<sg::ota::OtaTask> PersistentStore::listTasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<sg::ota::OtaTask> out;
    if (db_ == nullptr) {
        return out;
    }

    const char* sql = "SELECT task_uuid,device_id,device_type,firmware_id,state,last_error FROM ota_tasks;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        sg::ota::OtaTask t;
        t.task_uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        t.device_id = static_cast<std::uint8_t>(sqlite3_column_int(stmt, 1));
        t.device_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        t.firmware_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        if (state != nullptr) {
            (void)sg::ota::fromString(state, t.state);
        }
        const unsigned char* last_error = sqlite3_column_text(stmt, 5);
        t.last_error = (last_error != nullptr) ? reinterpret_cast<const char*>(last_error) : "";
        out.push_back(std::move(t));
    }
    sqlite3_finalize(stmt);
    return out;
}

bool PersistentStore::appendEvent(const OtaTaskEvent& event, std::string& err) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        err = "sqlite db not open";
        return false;
    }

    if (!execSql("BEGIN IMMEDIATE TRANSACTION;", err)) {
        return false;
    }

    const char* ins = "INSERT INTO ota_events(task_uuid,ts_unix_ms,state,detail) VALUES(?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, ins, -1, &stmt, nullptr) != SQLITE_OK) {
        (void)execSql("ROLLBACK;", err);
        err = "prepare appendEvent failed";
        return false;
    }

    const bool ok =
        bindText(stmt, 1, event.task_uuid) &&
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(event.ts_unix_ms)) == SQLITE_OK &&
        bindText(stmt, 3, event.state) &&
        bindText(stmt, 4, event.detail);

    if (!ok || sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        (void)execSql("ROLLBACK;", err);
        err = "execute appendEvent failed";
        return false;
    }
    sqlite3_finalize(stmt);

    const char* trim_sql =
        "DELETE FROM ota_events "
        "WHERE id IN ("
        "  SELECT id FROM ota_events ORDER BY id DESC LIMIT -1 OFFSET ?"
        ");";
    sqlite3_stmt* trim_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, trim_sql, -1, &trim_stmt, nullptr) != SQLITE_OK) {
        (void)execSql("ROLLBACK;", err);
        err = "prepare trim events failed";
        return false;
    }
    sqlite3_bind_int(trim_stmt, 1, static_cast<int>(kMaxPersistedEvents));
    if (sqlite3_step(trim_stmt) != SQLITE_DONE) {
        sqlite3_finalize(trim_stmt);
        (void)execSql("ROLLBACK;", err);
        err = "execute trim events failed";
        return false;
    }
    sqlite3_finalize(trim_stmt);

    return execSql("COMMIT;", err);
}

std::vector<OtaTaskEvent> PersistentStore::listEvents(const std::string& task_uuid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OtaTaskEvent> out;
    if (db_ == nullptr) {
        return out;
    }

    const char* sql_all = "SELECT task_uuid,ts_unix_ms,state,detail FROM ota_events ORDER BY id ASC;";
    const char* sql_one = "SELECT task_uuid,ts_unix_ms,state,detail FROM ota_events WHERE task_uuid=? ORDER BY id ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, task_uuid.empty() ? sql_all : sql_one, -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    if (!task_uuid.empty()) {
        (void)bindText(stmt, 1, task_uuid);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OtaTaskEvent e;
        e.task_uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        e.ts_unix_ms = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));
        e.state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        e.detail = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        out.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    return out;
}

} // namespace sg::storage
