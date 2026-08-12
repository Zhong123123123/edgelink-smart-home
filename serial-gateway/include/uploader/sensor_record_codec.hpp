#pragma once

#include "protocol/frame.hpp"

#include <string>

namespace sg {

std::string encodeSensorRecord(const SensorData& data);
bool decodeSensorRecord(const std::string& line, SensorData& out);

} // namespace sg
