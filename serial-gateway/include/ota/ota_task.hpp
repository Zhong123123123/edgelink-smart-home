#pragma once

#include <cstdint>
#include <string>

namespace sg::ota {

enum class OtaTaskState {
    CREATED,
    WAIT_DEVICE_ONLINE,
    PRECHECK,
    PREPARE_DEVICE,
    TRANSFERRING,
    VERIFYING,
    COMMITTING,
    REBOOTING,
    VERSION_CHECK,
    HEALTH_CONFIRM,
    SUCCESS,
    FAILED,
    ROLLBACK,
    CANCELED
};

struct OtaTask {
    std::string task_uuid;
    std::uint8_t device_id = 0;
    std::string device_type;
    std::string firmware_id;
    OtaTaskState state = OtaTaskState::CREATED;
    std::string last_error;
};

const char* toString(OtaTaskState state);
bool fromString(const std::string& s, OtaTaskState& out);

} // namespace sg::ota
