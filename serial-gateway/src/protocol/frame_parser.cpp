#include "protocol/frame_parser.hpp"

#include "protocol/crc16.hpp"

#include <chrono>
#include <sstream>

namespace sg {

namespace {

constexpr std::uint8_t kHeader1 = 0xAA;
constexpr std::uint8_t kHeader2 = 0x55;
constexpr std::size_t kSensorPayloadSizeMin = 8;
constexpr std::size_t kSensorPayloadSizeV1 = 12; // old: status@7 seq@8..11
constexpr std::size_t kSensorPayloadSizeV2 = 14; // new: light@7..8 status@9 seq@10..13
constexpr std::size_t kMinFrameSize = 2 + 1 + 1 + 2; // header + len + type + crc

std::uint16_t readU16LE(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8U);
}

std::int16_t readI16LE(const std::uint8_t* p) {
    return static_cast<std::int16_t>(readU16LE(p));
}

std::uint64_t unixMsNow() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string toHex(const std::uint8_t* p, std::size_t len) {
    std::ostringstream oss;
    oss.setf(std::ios::hex, std::ios::basefield);
    oss.setf(std::ios::uppercase);
    for (std::size_t i = 0; i < len; ++i) {
        if (i > 0) {
            oss << ' ';
        }
        const unsigned int v = static_cast<unsigned int>(p[i]);
        if (v < 0x10U) {
            oss << '0';
        }
        oss << v;
    }
    return oss.str();
}

const char* alarmEventName(std::uint8_t code) {
    switch (code) {
        case 1: return "alarm_enter";
        case 2: return "alarm_recover";
        case 3: return "sensor_invalid";
        case 4: return "sensor_recovered";
        default: return "alarm_code";
    }
}

} // namespace

void FrameParser::append(const std::uint8_t* data, std::size_t len) {
    buffer_.insert(buffer_.end(), data, data + len);
}

std::vector<SensorData> FrameParser::extract(ParseCounters& counters) {
    std::vector<SensorData> out;

    while (buffer_.size() >= kMinFrameSize) {
        if (buffer_[0] != kHeader1 || buffer_[1] != kHeader2) {
            buffer_.erase(buffer_.begin());
            counters.dropped_bytes++;
            continue;
        }

        const std::uint8_t len = buffer_[2];
        if (len < 1) {
            buffer_.erase(buffer_.begin());
            counters.dropped_bytes++;
            continue;
        }

        const std::size_t total = 2 + 1 + static_cast<std::size_t>(len) + 2;
        if (buffer_.size() < total) {
            break;
        }

        counters.received_frames++;

        const std::uint16_t wire_crc = readU16LE(&buffer_[total - 2]);
        const std::uint16_t calc_crc = crc16_modbus(&buffer_[2], 1 + len);
        if (wire_crc != calc_crc) {
            counters.crc_errors++;
            counters.dropped_bytes += total;
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(total));
            continue;
        }

        const std::uint8_t wire_type = buffer_[3];
        const FrameType type = frameTypeFromWire(wire_type);
        const std::size_t payload_len = static_cast<std::size_t>(len - 1);
        const std::uint8_t* p = &buffer_[4];

        SensorData data;
        data.timestamp_unix_ms = unixMsNow();
        data.frame_type = type;
        data.link_type = "serial";

        bool parsed = false;
        switch (type) {
            case FrameType::SENSOR_DATA:
                if (payload_len >= kSensorPayloadSizeMin) {
                    data.device_id = p[0];
                    data.temperature = static_cast<double>(readI16LE(p + 1)) / 10.0;
                    data.humidity = static_cast<double>(readU16LE(p + 3)) / 10.0;
                    data.voltage = static_cast<double>(readU16LE(p + 5)) / 1000.0;
                    if (payload_len >= kSensorPayloadSizeV2) {
                        data.light = static_cast<double>(readU16LE(p + 7));
                        data.status = p[9];
                        data.seq = static_cast<std::uint64_t>(p[10]) |
                                   (static_cast<std::uint64_t>(p[11]) << 8U) |
                                   (static_cast<std::uint64_t>(p[12]) << 16U) |
                                   (static_cast<std::uint64_t>(p[13]) << 24U);
                    } else {
                        data.status = p[7];
                        if (payload_len >= kSensorPayloadSizeV1) {
                            data.seq = static_cast<std::uint64_t>(p[8]) |
                                       (static_cast<std::uint64_t>(p[9]) << 8U) |
                                       (static_cast<std::uint64_t>(p[10]) << 16U) |
                                       (static_cast<std::uint64_t>(p[11]) << 24U);
                        }
                    }
                    data.led_on = (data.status & 0x01U) ? 1 : 0;
                    data.alarm_on = (data.status & 0x02U) ? 1 : 0;
                    data.sensor_valid = (data.status & 0x04U) ? 1 : 0;
                    data.auto_mode = (data.status & 0x08U) ? 1 : 0;
                    data.payload_summary =
                        "temp=" + std::to_string(data.temperature) +
                        ",hum=" + std::to_string(data.humidity) +
                        ",voltage=" + std::to_string(data.voltage) +
                        ",light=" + std::to_string(data.light);
                    parsed = true;
                }
                break;
            case FrameType::ALARM_EVENT:
                if (payload_len >= 2) {
                    data.device_id = p[0];
                    data.status = p[1];
                    data.payload_summary =
                        std::string(alarmEventName(p[1])) + "=" + std::to_string(static_cast<int>(p[1]));
                    parsed = true;
                }
                break;
            case FrameType::HEARTBEAT:
                if (payload_len >= 1) {
                    data.device_id = p[0];
                    data.payload_summary = "heartbeat";
                    parsed = true;
                }
                break;
            case FrameType::COMMAND_ACK:
                if (payload_len >= 4) {
                    data.device_id = p[0];
                    data.command_id = readU16LE(p + 1);
                    data.command_result = p[3];
                    data.payload_summary =
                        "cmd_id=" + std::to_string(data.command_id) +
                        ",result=" + std::to_string(static_cast<int>(data.command_result));
                    parsed = true;
                }
                break;
            case FrameType::DEVICE_STATUS:
                if (payload_len >= 3) {
                    data.device_id = p[0];
                    data.status = p[1];
                    std::ostringstream oss;
                    oss << "online=" << (p[1] > 0 ? "1" : "0") << ",version=" << static_cast<int>(p[2]);
                    if (payload_len >= 4) {
                        oss << "." << static_cast<int>(p[3]);
                    }
                    data.payload_summary = oss.str();
                    parsed = true;
                }
                break;
            case FrameType::REPORT_ACK:
                data.payload_summary = "report_ack";
                break;
            case FrameType::UNKNOWN:
                counters.unknown_types++;
                data.payload_summary = "unknown_type=" + std::to_string(static_cast<int>(wire_type)) +
                                       " payload_hex=" + toHex(p, payload_len);
                break;
        }

        if (parsed) {
            out.push_back(std::move(data));
            counters.parsed_ok++;
        } else if (type != FrameType::UNKNOWN) {
            counters.dropped_bytes += payload_len;
        }

        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(total));
    }

    return out;
}

} // namespace sg
