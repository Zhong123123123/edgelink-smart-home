#include "ota/ota_task_manager.hpp"

#include <cassert>
#include <cstdio>

int main() {
    const char* path = "/tmp/sg_test_ota_tasks.sqlite3";
    std::remove(path);
    sg::storage::PersistentStore store(path);
    sg::ota::OtaTaskManager mgr(store);
    std::string err;
    auto t = mgr.create(1, "stm32f407-smarthome", "fw-1.0.0", err);
    assert(err.empty());
    assert(!t.task_uuid.empty());
    assert(mgr.updateState(t.task_uuid, sg::ota::OtaTaskState::WAIT_DEVICE_ONLINE, "online", err));
    auto now = mgr.get(t.task_uuid);
    assert(now.has_value());
    assert(now->state == sg::ota::OtaTaskState::WAIT_DEVICE_ONLINE);
    assert(mgr.cancel(t.task_uuid, err));
    now = mgr.get(t.task_uuid);
    assert(now->state == sg::ota::OtaTaskState::CANCELED);
    assert(mgr.retry(t.task_uuid, err));
    now = mgr.get(t.task_uuid);
    assert(now->state == sg::ota::OtaTaskState::CREATED);
    auto ev = mgr.events(t.task_uuid);
    assert(!ev.empty());
    return 0;
}
