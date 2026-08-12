#include "uploader/sensor_record_codec.hpp"

#include <cmath>
#include <iostream>

int main() {
    sg::SensorData in;
    in.timestamp_unix_ms = 1710000000123ULL;
    in.device_id = 7;
    in.device_name = "sensor-seven";
    in.topic_suffix = "line/a";
    in.temperature = 23.4;
    in.humidity = 55.1;
    in.voltage = 3.721;
    in.status = 1;

    const std::string line = sg::encodeSensorRecord(in);

    sg::SensorData out;
    if (!sg::decodeSensorRecord(line, out)) {
        std::cerr << "decode failed\n";
        return 1;
    }

    if (out.timestamp_unix_ms != in.timestamp_unix_ms || out.device_id != in.device_id || out.status != in.status ||
        out.device_name != in.device_name || out.topic_suffix != in.topic_suffix) {
        std::cerr << "integer fields mismatch\n";
        return 2;
    }

    if (std::fabs(out.temperature - in.temperature) > 0.001 || std::fabs(out.humidity - in.humidity) > 0.001 ||
        std::fabs(out.voltage - in.voltage) > 0.001) {
        std::cerr << "float fields mismatch\n";
        return 3;
    }

    return 0;
}
