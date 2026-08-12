#include "storage/history_store.hpp"

#include "common/logger.hpp"
#include "protocol/frame_types.hpp"

#include <sqlite3.h>
#include <chrono>
#include <filesystem>
#include <system_error>

namespace sg::storage {
namespace {

bool bindText(sqlite3_stmt* stmt, int idx, const std::string& v) {
    return sqlite3_bind_text(stmt, idx, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

} // namespace

HistoryStore::HistoryStore(const std::string& db_path)
    : db_path_(db_path) {
    // Ensure parent directory exists
    const auto parent = std::filesystem::path(db_path_).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            Logger::instance().error("[HistoryStore] mkdir failed: " + parent.string() + ": " + ec.message());
        }
    }

    if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
        Logger::instance().error("[HistoryStore] open failed: " + db_path_);
        db_ = nullptr;
        return;
    }

    if (!initDb()) {
        Logger::instance().error("[HistoryStore] init failed, closing");
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }

    Logger::instance().info("[HistoryStore] opened " + db_path_);
}

HistoryStore::~HistoryStore() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stmt_insert_sensor_) {
        sqlite3_finalize(stmt_insert_sensor_);
        stmt_insert_sensor_ = nullptr;
    }
    if (stmt_insert_system_) {
        sqlite3_finalize(stmt_insert_system_);
        stmt_insert_system_ = nullptr;
    }
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool HistoryStore::initDb() {
    auto pragma = [&](const char* sql) -> bool {
        char* msg = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
            Logger::instance().error(std::string("[HistoryStore] pragma: ") + (msg ? msg : sql));
            if (msg) sqlite3_free(msg);
            return false;
        }
        return true;
    };

    if (!pragma("PRAGMA journal_mode=WAL;")) return false;
    if (!pragma("PRAGMA busy_timeout=3000;")) return false;

    const char* ddl_system =
        "CREATE TABLE IF NOT EXISTS system_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "ts_unix_ms INTEGER NOT NULL,"
        "event_type TEXT NOT NULL,"
        "device_id INTEGER NOT NULL DEFAULT 0,"
        "detail TEXT NOT NULL DEFAULT ''"
        ");";
    {
        char* m = nullptr;
        if (sqlite3_exec(db_, ddl_system, nullptr, nullptr, &m) != SQLITE_OK) {
            Logger::instance().error(std::string("[HistoryStore] DDL system_events: ") + (m ? m : "unknown"));
            if (m) sqlite3_free(m);
            return false;
        }
    }

    const char* ddl =
        "CREATE TABLE IF NOT EXISTS sensor_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "ts_unix_ms INTEGER NOT NULL,"
        "device_id INTEGER NOT NULL,"
        "device_name TEXT NOT NULL DEFAULT '',"
        "frame_type TEXT NOT NULL DEFAULT '',"
        "link_type TEXT NOT NULL DEFAULT '',"
        "seq INTEGER NOT NULL DEFAULT 0,"
        "temperature REAL NOT NULL DEFAULT 0.0,"
        "humidity REAL NOT NULL DEFAULT 0.0,"
        "voltage REAL NOT NULL DEFAULT 0.0,"
        "light REAL NOT NULL DEFAULT -1.0,"
        "status INTEGER NOT NULL DEFAULT 0,"
        "command_id INTEGER NOT NULL DEFAULT 0,"
        "command_result INTEGER NOT NULL DEFAULT 0,"
        "payload_summary TEXT NOT NULL DEFAULT '',"
        "last_error TEXT NOT NULL DEFAULT '',"
        "wifi_rssi INTEGER NOT NULL DEFAULT 0,"
        "wifi_connected INTEGER NOT NULL DEFAULT 0,"
        "wifi_last_seen_ms INTEGER NOT NULL DEFAULT 0,"
        "mq2_alarm INTEGER NOT NULL DEFAULT -1,"
        "ld2402_presence INTEGER NOT NULL DEFAULT -1,"
        "led_on INTEGER NOT NULL DEFAULT -1,"
        "alarm_on INTEGER NOT NULL DEFAULT -1,"
        "sensor_valid INTEGER NOT NULL DEFAULT -1,"
        "auto_mode INTEGER NOT NULL DEFAULT -1"
        ");";

    char* msg = nullptr;
    if (sqlite3_exec(db_, ddl, nullptr, nullptr, &msg) != SQLITE_OK) {
        Logger::instance().error(std::string("[HistoryStore] DDL: ") + (msg ? msg : "unknown"));
        if (msg) sqlite3_free(msg);
        return false;
    }

    // Time-range index, useful when /api/history is added later
    auto idx = [&](const char* sql) {
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
            Logger::instance().warn(std::string("[HistoryStore] index: ") + (msg ? msg : sql));
            if (msg) sqlite3_free(msg);
        }
    };
    idx("CREATE INDEX IF NOT EXISTS idx_sensor_events_ts ON sensor_events(ts_unix_ms);");
    idx("CREATE INDEX IF NOT EXISTS idx_sensor_events_dev_ts ON sensor_events(device_id, ts_unix_ms);");
    idx("CREATE INDEX IF NOT EXISTS idx_system_events_ts ON system_events(ts_unix_ms);");
    idx("CREATE INDEX IF NOT EXISTS idx_system_events_type ON system_events(event_type, ts_unix_ms);");

    return prepareStatements();
}

bool HistoryStore::prepareStatements() {
    const char* sql =
        "INSERT INTO sensor_events("
        "ts_unix_ms,device_id,device_name,frame_type,link_type,"
        "seq,temperature,humidity,voltage,light,status,"
        "command_id,command_result,payload_summary,last_error,"
        "wifi_rssi,wifi_connected,wifi_last_seen_ms,"
        "mq2_alarm,ld2402_presence,led_on,alarm_on,sensor_valid,auto_mode"
        ") VALUES("
        "?,?,?,?,?,"
        "?,?,?,?,?,?,"
        "?,?,?,?,"
        "?,?,?,"
        "?,?,?,?,?,?"
        ")";

    const int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt_insert_sensor_, nullptr);
    if (rc != SQLITE_OK) {
        Logger::instance().error(std::string("[HistoryStore] prepare sensor: ") + sqlite3_errmsg(db_));
        return false;
    }

    const char* sql_sys =
        "INSERT INTO system_events(ts_unix_ms,event_type,device_id,detail) VALUES(?,?,?,?);";
    const int r2 = sqlite3_prepare_v2(db_, sql_sys, -1, &stmt_insert_system_, nullptr);
    if (r2 != SQLITE_OK) {
        Logger::instance().error(std::string("[HistoryStore] prepare system: ") + sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool HistoryStore::insertSensorEvent(const SensorData& data) {
    if (!db_ || !stmt_insert_sensor_) {
        Logger::instance().warn("[HistoryStore] unavailable, insert skipped");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    sqlite3_reset(stmt_insert_sensor_);
    sqlite3_clear_bindings(stmt_insert_sensor_);

    int idx = 1;
    bool ok = true;

    ok = ok && sqlite3_bind_int64(stmt_insert_sensor_, idx++, static_cast<sqlite3_int64>(data.timestamp_unix_ms)) == SQLITE_OK;
    ok = ok && sqlite3_bind_int(stmt_insert_sensor_, idx++, static_cast<int>(data.device_id)) == SQLITE_OK;
    ok = ok && bindText(stmt_insert_sensor_, idx++, data.device_name);
    ok = ok && bindText(stmt_insert_sensor_, idx++, frameTypeName(data.frame_type));
    ok = ok && bindText(stmt_insert_sensor_, idx++, data.link_type);
    ok = ok && sqlite3_bind_int64(stmt_insert_sensor_, idx++, static_cast<sqlite3_int64>(data.seq)) == SQLITE_OK;
    ok = ok && sqlite3_bind_double(stmt_insert_sensor_, idx++, data.temperature) == SQLITE_OK;
    ok = ok && sqlite3_bind_double(stmt_insert_sensor_, idx++, data.humidity) == SQLITE_OK;
    ok = ok && sqlite3_bind_double(stmt_insert_sensor_, idx++, data.voltage) == SQLITE_OK;
    ok = ok && sqlite3_bind_double(stmt_insert_sensor_, idx++, data.light) == SQLITE_OK;
    ok = ok && sqlite3_bind_int(stmt_insert_sensor_, idx++, static_cast<int>(data.status)) == SQLITE_OK;
    ok = ok && sqlite3_bind_int(stmt_insert_sensor_, idx++, static_cast<int>(data.command_id)) == SQLITE_OK;
    ok = ok && sqlite3_bind_int(stmt_insert_sensor_, idx++, static_cast<int>(data.command_result)) == SQLITE_OK;
    ok = ok && bindText(stmt_insert_sensor_, idx++, data.payload_summary);
    ok = ok && bindText(stmt_insert_sensor_, idx++, data.last_error);
    ok = ok && sqlite3_bind_int(stmt_insert_sensor_, idx++, data.wifi_rssi) == SQLITE_OK;
    ok = ok && sqlite3_bind_int(stmt_insert_sensor_, idx++, data.wifi_connected ? 1 : 0) == SQLITE_OK;
    ok = ok && sqlite3_bind_int64(stmt_insert_sensor_, idx++, static_cast<sqlite3_int64>(data.wifi_last_seen_ms)) == SQLITE_OK;
    ok = ok && sqlite3_bind_int(stmt_insert_sensor_, idx++, data.mq2_alarm) == SQLITE_OK;
    ok = ok && sqlite3_bind_int(stmt_insert_sensor_, idx++, data.ld2402_presence) == SQLITE_OK;
    ok = ok && sqlite3_bind_int(stmt_insert_sensor_, idx++, data.led_on) == SQLITE_OK;
    ok = ok && sqlite3_bind_int(stmt_insert_sensor_, idx++, data.alarm_on) == SQLITE_OK;
    ok = ok && sqlite3_bind_int(stmt_insert_sensor_, idx++, data.sensor_valid) == SQLITE_OK;
    ok = ok && sqlite3_bind_int(stmt_insert_sensor_, idx++, data.auto_mode) == SQLITE_OK;

    if (!ok) {
        Logger::instance().error("[HistoryStore] bind failed");
        return false;
    }

    const int rc = sqlite3_step(stmt_insert_sensor_);
    if (rc != SQLITE_DONE) {
        Logger::instance().error(std::string("[HistoryStore] step: ") + sqlite3_errmsg(db_));
        return false;
    }

    sensor_insert_count_++;
    if (sensor_insert_count_ >= kSensorPruneInterval) {
        sensor_insert_count_ = 0;
        pruneSensorEvents();
    }

    return true;
}

/* static */ std::uint64_t HistoryStore::nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void HistoryStore::insertSystemEvent(const std::string& event_type, int device_id, const std::string& detail) {
    if (!db_ || !stmt_insert_system_) {
        Logger::instance().warn("[HistoryStore] unavailable, system event skipped");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    sqlite3_reset(stmt_insert_system_);
    sqlite3_clear_bindings(stmt_insert_system_);

    const bool ok =
        sqlite3_bind_int64(stmt_insert_system_, 1, static_cast<sqlite3_int64>(nowMs())) == SQLITE_OK &&
        bindText(stmt_insert_system_, 2, event_type) &&
        sqlite3_bind_int(stmt_insert_system_, 3, device_id) == SQLITE_OK &&
        bindText(stmt_insert_system_, 4, detail);

    if (!ok) {
        Logger::instance().error("[HistoryStore] bind failed for insertSystemEvent");
        return;
    }

    const int rc = sqlite3_step(stmt_insert_system_);
    if (rc != SQLITE_DONE) {
        Logger::instance().error(std::string("[HistoryStore] system event step: ") + sqlite3_errmsg(db_));
    }

    system_insert_count_++;
    if (system_insert_count_ >= kSystemPruneInterval) {
        system_insert_count_ = 0;
        pruneSystemEvents();
    }
}

void HistoryStore::pruneSensorEvents() {
    if (!db_) return;
    const std::string sql =
        "DELETE FROM sensor_events WHERE id <= COALESCE("
        "(SELECT id FROM sensor_events ORDER BY id DESC LIMIT 1 OFFSET " +
        std::to_string(kSensorMaxRows) + "), 0);";
    char* msg = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
        Logger::instance().error(std::string("[HistoryStore] prune sensor_events: ") + (msg ? msg : "unknown"));
        if (msg) sqlite3_free(msg);
    }
}

void HistoryStore::pruneSystemEvents() {
    if (!db_) return;
    const std::string sql =
        "DELETE FROM system_events WHERE id <= COALESCE("
        "(SELECT id FROM system_events ORDER BY id DESC LIMIT 1 OFFSET " +
        std::to_string(kSystemMaxRows) + "), 0);";
    char* msg = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
        Logger::instance().error(std::string("[HistoryStore] prune system_events: ") + (msg ? msg : "unknown"));
        if (msg) sqlite3_free(msg);
    }
}

std::vector<SensorEventRecord> HistoryStore::querySensorEvents(int device_id, int limit, int offset) const {
    std::vector<SensorEventRecord> out;
    if (!db_) return out;

    std::lock_guard<std::mutex> lock(mutex_);

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id,ts_unix_ms,device_id,device_name,frame_type,link_type,"
        "seq,temperature,humidity,voltage,light,status,"
        "command_id,command_result,payload_summary,last_error,"
        "wifi_rssi,wifi_connected,wifi_last_seen_ms,"
        "mq2_alarm,ld2402_presence,led_on,alarm_on,sensor_valid,auto_mode "
        "FROM sensor_events "
        "WHERE (?1 = 0 OR device_id = ?1) "
        "ORDER BY id DESC "
        "LIMIT ?2 OFFSET ?3;";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        Logger::instance().error(std::string("[HistoryStore] query prepare: ") + sqlite3_errmsg(db_));
        return out;
    }

    sqlite3_bind_int(stmt, 1, device_id);
    sqlite3_bind_int(stmt, 2, limit);
    sqlite3_bind_int(stmt, 3, offset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int c = 0;
        SensorEventRecord r;
        r.id = sqlite3_column_int64(stmt, c++);
        r.ts_unix_ms = sqlite3_column_int64(stmt, c++);
        r.device_id = sqlite3_column_int(stmt, c++);
        if (auto s = sqlite3_column_text(stmt, c++)) r.device_name = reinterpret_cast<const char*>(s);
        if (auto s = sqlite3_column_text(stmt, c++)) r.frame_type = reinterpret_cast<const char*>(s);
        if (auto s = sqlite3_column_text(stmt, c++)) r.link_type = reinterpret_cast<const char*>(s);
        r.seq = sqlite3_column_int64(stmt, c++);
        r.temperature = sqlite3_column_double(stmt, c++);
        r.humidity = sqlite3_column_double(stmt, c++);
        r.voltage = sqlite3_column_double(stmt, c++);
        r.light = sqlite3_column_double(stmt, c++);
        r.status = sqlite3_column_int(stmt, c++);
        r.command_id = sqlite3_column_int(stmt, c++);
        r.command_result = sqlite3_column_int(stmt, c++);
        if (auto s = sqlite3_column_text(stmt, c++)) r.payload_summary = reinterpret_cast<const char*>(s);
        if (auto s = sqlite3_column_text(stmt, c++)) r.last_error = reinterpret_cast<const char*>(s);
        r.wifi_rssi = sqlite3_column_int(stmt, c++);
        r.wifi_connected = sqlite3_column_int(stmt, c++) != 0;
        r.wifi_last_seen_ms = sqlite3_column_int64(stmt, c++);
        r.mq2_alarm = sqlite3_column_int(stmt, c++);
        r.ld2402_presence = sqlite3_column_int(stmt, c++);
        r.led_on = sqlite3_column_int(stmt, c++);
        r.alarm_on = sqlite3_column_int(stmt, c++);
        r.sensor_valid = sqlite3_column_int(stmt, c++);
        r.auto_mode = sqlite3_column_int(stmt, c++);
        out.push_back(std::move(r));
    }

    sqlite3_finalize(stmt);
    return out;
}

std::vector<SystemEventRecord> HistoryStore::querySystemEvents(int limit, int offset) const {
    std::vector<SystemEventRecord> out;
    if (!db_) return out;

    std::lock_guard<std::mutex> lock(mutex_);

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id,ts_unix_ms,event_type,device_id,detail "
        "FROM system_events "
        "ORDER BY id DESC "
        "LIMIT ?1 OFFSET ?2;";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        Logger::instance().error(std::string("[HistoryStore] sys query prepare: ") + sqlite3_errmsg(db_));
        return out;
    }

    sqlite3_bind_int(stmt, 1, limit);
    sqlite3_bind_int(stmt, 2, offset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int c = 0;
        SystemEventRecord r;
        r.id = sqlite3_column_int64(stmt, c++);
        r.ts_unix_ms = sqlite3_column_int64(stmt, c++);
        if (auto s = sqlite3_column_text(stmt, c++)) r.event_type = reinterpret_cast<const char*>(s);
        r.device_id = sqlite3_column_int(stmt, c++);
        if (auto s = sqlite3_column_text(stmt, c++)) r.detail = reinterpret_cast<const char*>(s);
        out.push_back(std::move(r));
    }

    sqlite3_finalize(stmt);
    return out;
}

} // namespace sg::storage
