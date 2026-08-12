#pragma once

#include "protocol/frame_types.hpp"

#include <cstdint>
#include <string>

namespace sg {

struct SensorData {
    std::uint64_t timestamp_unix_ms = 0;
    std::uint8_t device_id = 0;
    FrameType frame_type = FrameType::SENSOR_DATA;
    std::string device_name;
    std::string topic_suffix;
    std::string payload_summary;
    std::string last_error;
    double temperature = 0.0;
    double humidity = 0.0;
    double voltage = 0.0;
    double light = -1.0;
    std::uint8_t status = 0;
    std::uint16_t command_id = 0;
    std::uint8_t command_result = 0;
    std::string link_type = "serial";
    int wifi_rssi = 0;
    bool wifi_connected = false;
    std::uint64_t wifi_last_seen_ms = 0;
    std::uint64_t seq = 0;
    int mq2_alarm = -1;
    int ld2402_presence = -1;
    int led_on = -1;
    int alarm_on = -1;
    int sensor_valid = -1;
    int auto_mode = -1;
};

} // namespace sg
