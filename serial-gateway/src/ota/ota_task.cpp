#include "ota/ota_task.hpp"

namespace sg::ota {

const char* toString(OtaTaskState state) {
    switch (state) {
        case OtaTaskState::CREATED: return "CREATED";
        case OtaTaskState::WAIT_DEVICE_ONLINE: return "WAIT_DEVICE_ONLINE";
        case OtaTaskState::PRECHECK: return "PRECHECK";
        case OtaTaskState::PREPARE_DEVICE: return "PREPARE_DEVICE";
        case OtaTaskState::TRANSFERRING: return "TRANSFERRING";
        case OtaTaskState::VERIFYING: return "VERIFYING";
        case OtaTaskState::COMMITTING: return "COMMITTING";
        case OtaTaskState::REBOOTING: return "REBOOTING";
        case OtaTaskState::VERSION_CHECK: return "VERSION_CHECK";
        case OtaTaskState::HEALTH_CONFIRM: return "HEALTH_CONFIRM";
        case OtaTaskState::SUCCESS: return "SUCCESS";
        case OtaTaskState::FAILED: return "FAILED";
        case OtaTaskState::ROLLBACK: return "ROLLBACK";
        case OtaTaskState::CANCELED: return "CANCELED";
    }
    return "UNKNOWN";
}

bool fromString(const std::string& s, OtaTaskState& out) {
    if (s == "CREATED") out = OtaTaskState::CREATED;
    else if (s == "WAIT_DEVICE_ONLINE") out = OtaTaskState::WAIT_DEVICE_ONLINE;
    else if (s == "PRECHECK") out = OtaTaskState::PRECHECK;
    else if (s == "PREPARE_DEVICE") out = OtaTaskState::PREPARE_DEVICE;
    else if (s == "TRANSFERRING") out = OtaTaskState::TRANSFERRING;
    else if (s == "VERIFYING") out = OtaTaskState::VERIFYING;
    else if (s == "COMMITTING") out = OtaTaskState::COMMITTING;
    else if (s == "REBOOTING") out = OtaTaskState::REBOOTING;
    else if (s == "VERSION_CHECK") out = OtaTaskState::VERSION_CHECK;
    else if (s == "HEALTH_CONFIRM") out = OtaTaskState::HEALTH_CONFIRM;
    else if (s == "SUCCESS") out = OtaTaskState::SUCCESS;
    else if (s == "FAILED") out = OtaTaskState::FAILED;
    else if (s == "ROLLBACK") out = OtaTaskState::ROLLBACK;
    else if (s == "CANCELED") out = OtaTaskState::CANCELED;
    else return false;
    return true;
}

} // namespace sg::ota
