#pragma once

#include "ota/ota_task.hpp"

#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace sg::storage {

struct OtaTaskEvent {
    std::string task_uuid;
    std::uint64_t ts_unix_ms = 0;
    std::string state;
    std::string detail;
};

class PersistentStore {
public:
    explicit PersistentStore(std::string file_path);
    ~PersistentStore();

    bool saveTask(const sg::ota::OtaTask& task, std::string& err);
    std::vector<sg::ota::OtaTask> listTasks() const;
    bool appendEvent(const OtaTaskEvent& event, std::string& err);
    std::vector<OtaTaskEvent> listEvents(const std::string& task_uuid) const;

private:
    bool initSchema(std::string& err);
    bool execSql(const char* sql, std::string& err) const;
    std::string file_path_;
    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
};

} // namespace sg::storage
