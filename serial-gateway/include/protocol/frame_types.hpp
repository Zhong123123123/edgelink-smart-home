#pragma once

#include <cstdint>
#include <string>

namespace sg {

enum class FrameType : std::uint8_t {
    SENSOR_DATA = 0x01,
    ALARM_EVENT = 0x02,
    HEARTBEAT = 0x03,
    COMMAND_ACK = 0x04,
    DEVICE_STATUS = 0x05,
    REPORT_ACK = 0x06,
    UNKNOWN = 0xFF
};

inline FrameType frameTypeFromWire(std::uint8_t type) {
    switch (type) {
        case static_cast<std::uint8_t>(FrameType::SENSOR_DATA): return FrameType::SENSOR_DATA;
        case static_cast<std::uint8_t>(FrameType::ALARM_EVENT): return FrameType::ALARM_EVENT;
        case static_cast<std::uint8_t>(FrameType::HEARTBEAT): return FrameType::HEARTBEAT;
        case static_cast<std::uint8_t>(FrameType::COMMAND_ACK): return FrameType::COMMAND_ACK;
        case static_cast<std::uint8_t>(FrameType::DEVICE_STATUS): return FrameType::DEVICE_STATUS;
        case static_cast<std::uint8_t>(FrameType::REPORT_ACK): return FrameType::REPORT_ACK;
        default: return FrameType::UNKNOWN;
    }
}

inline const char* frameTypeName(FrameType type) {
    switch (type) {
        case FrameType::SENSOR_DATA: return "sensor_data";
        case FrameType::ALARM_EVENT: return "alarm_event";
        case FrameType::HEARTBEAT: return "heartbeat";
        case FrameType::COMMAND_ACK: return "command_ack";
        case FrameType::DEVICE_STATUS: return "device_status";
        case FrameType::REPORT_ACK: return "report_ack";
        case FrameType::UNKNOWN: return "unknown";
    }
    return "unknown";
}

} // namespace sg
