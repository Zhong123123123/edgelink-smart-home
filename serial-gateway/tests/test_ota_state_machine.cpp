#include "ota/ota_state_machine.hpp"

#include <cassert>

int main() {
    using sg::ota::OtaStateMachine;
    using sg::ota::OtaTaskState;
    assert(OtaStateMachine::canTransition(OtaTaskState::CREATED, OtaTaskState::WAIT_DEVICE_ONLINE));
    assert(OtaStateMachine::canTransition(OtaTaskState::TRANSFERRING, OtaTaskState::FAILED));
    assert(!OtaStateMachine::canTransition(OtaTaskState::SUCCESS, OtaTaskState::PRECHECK));
    return 0;
}
