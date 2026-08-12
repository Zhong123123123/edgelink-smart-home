#include "protocol/crc16.hpp"
#include "protocol/frame_parser.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

void appendU16LE(std::vector<std::uint8_t>& v, std::uint16_t val) {
    v.push_back(static_cast<std::uint8_t>(val & 0xFFU));
    v.push_back(static_cast<std::uint8_t>((val >> 8U) & 0xFFU));
}

std::vector<std::uint8_t> makeValidFrame() {
    std::vector<std::uint8_t> frame = {
        0xAA, 0x55, 0x09, 0x01,
        0x01,
        0xEA, 0x00,
        0x1C, 0x02,
        0x7E, 0x0E,
        0x00
    };
    const std::uint16_t crc = sg::crc16_modbus(frame.data() + 2, 10);
    appendU16LE(frame, crc);
    return frame;
}

std::vector<std::uint8_t> makeCommandAckFrame(std::uint8_t device_id, std::uint16_t cmd_id, std::uint8_t result) {
    std::vector<std::uint8_t> frame = {
        0xAA, 0x55, 0x05, 0x04,
        device_id,
        static_cast<std::uint8_t>(cmd_id & 0xFFU),
        static_cast<std::uint8_t>((cmd_id >> 8U) & 0xFFU),
        result
    };
    const std::uint16_t crc = sg::crc16_modbus(frame.data() + 2, 6);
    appendU16LE(frame, crc);
    return frame;
}

std::vector<std::uint8_t> makeValidFrameV2() {
    // len=15(type+payload14), type=0x01
    // payload: dev, t, h, v, light, status, seq
    std::vector<std::uint8_t> frame = {
        0xAA, 0x55, 0x0F, 0x01,
        0x01,       // device
        0xEA, 0x00, // temp=23.4
        0x1C, 0x02, // hum=54.0
        0x7E, 0x0E, // v=3.71V
        0xBC, 0x02, // light=700
        0x0D,       // status bits
        0x11, 0x22, 0x33, 0x44 // seq
    };
    const std::uint16_t crc = sg::crc16_modbus(frame.data() + 2, 16);
    appendU16LE(frame, crc);
    return frame;
}

} // namespace

int main() {
    sg::FrameParser parser;
    sg::ParseCounters counters{};

    auto f = makeValidFrame();
    parser.append(f.data(), 5);
    parser.append(f.data() + 5, f.size() - 5);
    auto out = parser.extract(counters);

    if (out.size() != 1 || counters.parsed_ok != 1 || counters.crc_errors != 0) {
        std::cerr << "valid frame parse failed\n";
        return 1;
    }

    auto bad = makeValidFrame();
    bad[5] ^= 0xFFU;
    parser.append(bad.data(), bad.size());
    out = parser.extract(counters);

    if (!out.empty()) {
        std::cerr << "invalid frame should not parse\n";
        return 2;
    }

    if (counters.crc_errors < 1) {
        std::cerr << "crc error counter not increased\n";
        return 3;
    }

    auto ack = makeCommandAckFrame(5, 1234, 1);
    parser.append(ack.data(), ack.size());
    out = parser.extract(counters);
    if (out.size() != 1) {
        std::cerr << "command ack frame parse failed\n";
        return 4;
    }
    if (out[0].frame_type != sg::FrameType::COMMAND_ACK ||
        out[0].device_id != 5 ||
        out[0].command_id != 1234 ||
        out[0].command_result != 1) {
        std::cerr << "command ack values mismatch\n";
        return 5;
    }

    auto v2 = makeValidFrameV2();
    parser.append(v2.data(), v2.size());
    out = parser.extract(counters);
    if (out.size() != 1) {
        std::cerr << "v2 frame parse failed\n";
        return 6;
    }
    if (out[0].seq != 0x44332211ULL || static_cast<int>(out[0].light) != 700) {
        std::cerr << "v2 seq/light mismatch\n";
        return 7;
    }

    return 0;
}
