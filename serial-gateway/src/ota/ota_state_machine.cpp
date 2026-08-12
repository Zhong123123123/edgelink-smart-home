#include "ota/ota_state_machine.hpp"

namespace sg::ota {

bool OtaStateMachine::canTransition(OtaTaskState from, OtaTaskState to) {
    if (from == to) return true;
    if (to == OtaTaskState::FAILED || to == OtaTaskState::CANCELED || to == OtaTaskState::ROLLBACK) return true;
    switch (from) {
        case OtaTaskState::CREATED: return to == OtaTaskState::WAIT_DEVICE_ONLINE;
        case OtaTaskState::WAIT_DEVICE_ONLINE: return to == OtaTaskState::PRECHECK;
        case OtaTaskState::PRECHECK: return to == OtaTaskState::PREPARE_DEVICE;
        case OtaTaskState::PREPARE_DEVICE: return to == OtaTaskState::TRANSFERRING;
        case OtaTaskState::TRANSFERRING: return to == OtaTaskState::VERIFYING;
        case OtaTaskState::VERIFYING: return to == OtaTaskState::COMMITTING;
        case OtaTaskState::COMMITTING: return to == OtaTaskState::REBOOTING;
        case OtaTaskState::REBOOTING: return to == OtaTaskState::VERSION_CHECK;
        case OtaTaskState::VERSION_CHECK: return to == OtaTaskState::HEALTH_CONFIRM;
        case OtaTaskState::HEALTH_CONFIRM: return to == OtaTaskState::SUCCESS;
        case OtaTaskState::SUCCESS:
        case OtaTaskState::FAILED:
        case OtaTaskState::ROLLBACK:
        case OtaTaskState::CANCELED:
            return false;
    }
    return false;
}

} // namespace sg::ota
