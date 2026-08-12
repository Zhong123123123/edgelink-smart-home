#pragma once

#include "ota/ota_task.hpp"
#include "storage/sqlite_store.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sg::ota {

class OtaTaskManager {
public:
    explicit OtaTaskManager(sg::storage::PersistentStore& store);

    OtaTask create(std::uint8_t device_id, std::string device_type, std::string firmware_id, std::string& err);
    bool updateState(const std::string& task_uuid, OtaTaskState next, const std::string& detail, std::string& err);
    bool fail(const std::string& task_uuid, const std::string& reason, std::string& err);
    bool cancel(const std::string& task_uuid, std::string& err);
    bool retry(const std::string& task_uuid, std::string& err);
    std::optional<OtaTask> get(const std::string& task_uuid) const;
    std::vector<OtaTask> list() const;
    std::vector<sg::storage::OtaTaskEvent> events(const std::string& task_uuid) const;

private:
    std::string nowUuid() const;
    std::uint64_t nowMs() const;

    sg::storage::PersistentStore& store_;
    std::unordered_map<std::string, OtaTask> tasks_;
};

} // namespace sg::ota
