#pragma once

#include "protocol/frame.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sg {

struct ParseCounters {
    std::uint64_t received_frames = 0;
    std::uint64_t parsed_ok = 0;
    std::uint64_t crc_errors = 0;
    std::uint64_t unknown_types = 0;
    std::uint64_t dropped_bytes = 0;
};

class FrameParser {
public:
    void append(const std::uint8_t* data, std::size_t len);
    std::vector<SensorData> extract(ParseCounters& counters);

private:
    std::vector<std::uint8_t> buffer_;
};

} // namespace sg
