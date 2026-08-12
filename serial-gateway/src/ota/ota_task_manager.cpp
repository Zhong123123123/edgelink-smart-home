#include "ota/ota_task_manager.hpp"

#include "ota/ota_state_machine.hpp"

#include <chrono>

namespace sg::ota {

OtaTaskManager::OtaTaskManager(sg::storage::PersistentStore& store) : store_(store) {
    for (const auto& t : store_.listTasks()) {
        tasks_[t.task_uuid] = t;
    }
}

std::string OtaTaskManager::nowUuid() const {
    const auto ms = nowMs();
    return "ota-" + std::to_string(ms);
}

std::uint64_t OtaTaskManager::nowMs() const {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

OtaTask OtaTaskManager::create(std::uint8_t device_id, std::string device_type, std::string firmware_id, std::string& err) {
    OtaTask t;
    t.task_uuid = nowUuid();
    t.device_id = device_id;
    t.device_type = std::move(device_type);
    t.firmware_id = std::move(firmware_id);
    t.state = OtaTaskState::CREATED;
    tasks_[t.task_uuid] = t;
    store_.saveTask(t, err);
    store_.appendEvent({t.task_uuid, nowMs(), toString(t.state), "task created"}, err);
    return t;
}

bool OtaTaskManager::updateState(const std::string& task_uuid, OtaTaskState next, const std::string& detail, std::string& err) {
    auto it = tasks_.find(task_uuid);
    if (it == tasks_.end()) {
        err = "task not found";
        return false;
    }
    if (!OtaStateMachine::canTransition(it->second.state, next)) {
        err = "invalid state transition";
        return false;
    }
    it->second.state = next;
    if (!store_.saveTask(it->second, err)) return false;
    return store_.appendEvent({task_uuid, nowMs(), toString(next), detail}, err);
}

bool OtaTaskManager::fail(const std::string& task_uuid, const std::string& reason, std::string& err) {
    auto it = tasks_.find(task_uuid);
    if (it == tasks_.end()) {
        err = "task not found";
        return false;
    }
    it->second.state = OtaTaskState::FAILED;
    it->second.last_error = reason;
    if (!store_.saveTask(it->second, err)) return false;
    return store_.appendEvent({task_uuid, nowMs(), toString(OtaTaskState::FAILED), reason}, err);
}

bool OtaTaskManager::cancel(const std::string& task_uuid, std::string& err) {
    auto it = tasks_.find(task_uuid);
    if (it == tasks_.end()) {
        err = "task not found";
        return false;
    }
    if (!OtaStateMachine::canTransition(it->second.state, OtaTaskState::CANCELED)) {
        err = "cannot cancel in current state";
        return false;
    }
    it->second.state = OtaTaskState::CANCELED;
    if (!store_.saveTask(it->second, err)) return false;
    return store_.appendEvent({task_uuid, nowMs(), toString(OtaTaskState::CANCELED), "task canceled"}, err);
}

bool OtaTaskManager::retry(const std::string& task_uuid, std::string& err) {
    auto it = tasks_.find(task_uuid);
    if (it == tasks_.end()) {
        err = "task not found";
        return false;
    }
    if (it->second.state != OtaTaskState::FAILED && it->second.state != OtaTaskState::CANCELED) {
        err = "retry requires FAILED/CANCELED state";
        return false;
    }
    it->second.state = OtaTaskState::CREATED;
    it->second.last_error.clear();
    if (!store_.saveTask(it->second, err)) return false;
    return store_.appendEvent({task_uuid, nowMs(), toString(OtaTaskState::CREATED), "task retried"}, err);
}

std::optional<OtaTask> OtaTaskManager::get(const std::string& task_uuid) const {
    auto it = tasks_.find(task_uuid);
    if (it == tasks_.end()) return std::nullopt;
    return it->second;
}

std::vector<OtaTask> OtaTaskManager::list() const {
    std::vector<OtaTask> out;
    out.reserve(tasks_.size());
    for (const auto& kv : tasks_) out.push_back(kv.second);
    return out;
}

std::vector<sg::storage::OtaTaskEvent> OtaTaskManager::events(const std::string& task_uuid) const {
    return store_.listEvents(task_uuid);
}

} // namespace sg::ota
