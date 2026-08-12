#include "uploader/sensor_record_codec.hpp"

#include <iomanip>
#include <sstream>
#include <vector>

namespace sg {

namespace {

std::string sanitize(const std::string& s) {
    std::string out = s;
    for (char& ch : out) {
        if (ch == '\t' || ch == '\n' || ch == '\r') {
            ch = ' ';
        }
    }
    return out;
}

std::vector<std::string> splitTab(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t pos = s.find('\t', start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

} // namespace

std::string encodeSensorRecord(const SensorData& data) {
    std::ostringstream oss;
    oss << data.timestamp_unix_ms << '\t'
        << static_cast<unsigned int>(data.device_id) << '\t'
        << static_cast<unsigned int>(data.frame_type) << '\t'
        << sanitize(data.device_name) << '\t'
        << sanitize(data.topic_suffix) << '\t'
        << sanitize(data.payload_summary) << '\t'
        << std::fixed << std::setprecision(3) << data.temperature << '\t'
        << std::fixed << std::setprecision(3) << data.humidity << '\t'
        << std::fixed << std::setprecision(3) << data.voltage << '\t'
        << static_cast<unsigned int>(data.status) << '\t'
        << static_cast<unsigned int>(data.command_id) << '\t'
        << static_cast<unsigned int>(data.command_result);
    return oss.str();
}

bool decodeSensorRecord(const std::string& line, SensorData& out) {
    const auto parts = splitTab(line);
    if (parts.size() != 8 && parts.size() != 12) {
        return false;
    }

    try {
        out.timestamp_unix_ms = static_cast<std::uint64_t>(std::stoull(parts[0]));
        out.device_id = static_cast<std::uint8_t>(std::stoul(parts[1]));
        std::size_t idx = 2;
        if (parts.size() == 12) {
            out.frame_type = frameTypeFromWire(static_cast<std::uint8_t>(std::stoul(parts[idx++])));
        } else {
            out.frame_type = FrameType::SENSOR_DATA;
        }
        out.device_name = parts[idx++];
        out.topic_suffix = parts[idx++];
        out.payload_summary = parts.size() == 12 ? parts[idx++] : std::string{};
        out.temperature = std::stod(parts[idx++]);
        out.humidity = std::stod(parts[idx++]);
        out.voltage = std::stod(parts[idx++]);
        out.status = static_cast<std::uint8_t>(std::stoul(parts[idx++]));
        out.command_id = parts.size() == 12 ? static_cast<std::uint16_t>(std::stoul(parts[idx++])) : 0;
        out.command_result = parts.size() == 12 ? static_cast<std::uint8_t>(std::stoul(parts[idx++])) : 0;
    } catch (...) {
        return false;
    }
    return true;
}

} // namespace sg
