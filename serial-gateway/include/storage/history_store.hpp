#pragma once

#include "protocol/frame.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace sg::storage {

struct SensorEventRecord {
    std::int64_t id = 0;
    std::int64_t ts_unix_ms = 0;
    int device_id = 0;
    std::string device_name;
    std::string frame_type;
    std::string link_type;
    std::int64_t seq = 0;
    double temperature = 0.0;
    double humidity = 0.0;
    double voltage = 0.0;
    double light = -1.0;
    int status = 0;
    int command_id = 0;
    int command_result = 0;
    std::string payload_summary;
    std::string last_error;
    int wifi_rssi = 0;
    bool wifi_connected = false;
    std::int64_t wifi_last_seen_ms = 0;
    int mq2_alarm = -1;
    int ld2402_presence = -1;
    int led_on = -1;
    int alarm_on = -1;
    int sensor_valid = -1;
    int auto_mode = -1;
};

struct SystemEventRecord {
    std::int64_t id = 0;
    std::int64_t ts_unix_ms = 0;
    std::string event_type;
    int device_id = 0;
    std::string detail;
};

class HistoryStore {
public:
    explicit HistoryStore(const std::string& db_path = "data/gateway_history.db");
    ~HistoryStore();

    // Non-copyable, non-movable
    HistoryStore(const HistoryStore&) = delete;
    HistoryStore& operator=(const HistoryStore&) = delete;

    // Logs on error internally, never throws. Returns false on failure.
    bool insertSensorEvent(const SensorData& data);

    // device_id = 0 means not applicable.
    void insertSystemEvent(const std::string& event_type, int device_id, const std::string& detail);

    // Query helpers. device_id=0 returns all. limit is clamped at construction time.
    std::vector<SensorEventRecord> querySensorEvents(int device_id, int limit, int offset) const;
    std::vector<SystemEventRecord> querySystemEvents(int limit, int offset) const;

private:
    bool initDb();
    bool prepareStatements();
    static std::uint64_t nowMs();

    void pruneSensorEvents();
    void pruneSystemEvents();

    static constexpr int kSensorMaxRows = 100000;
    static constexpr int kSystemMaxRows = 50000;
    static constexpr int kSensorPruneInterval = 1000;
    static constexpr int kSystemPruneInterval = 500;

    std::string db_path_;
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_insert_sensor_ = nullptr;
    sqlite3_stmt* stmt_insert_system_ = nullptr;
    int sensor_insert_count_ = 0;
    int system_insert_count_ = 0;
    mutable std::mutex mutex_;
};

} // namespace sg::storage
