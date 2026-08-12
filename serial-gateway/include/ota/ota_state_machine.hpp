#pragma once

#include "ota/ota_task.hpp"

namespace sg::ota {

class OtaStateMachine {
public:
    static bool canTransition(OtaTaskState from, OtaTaskState to);
};

} // namespace sg::ota
