#include "protocol/crc16.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    // Placeholder protocol sanity test for AA55 frame CRC path.
    std::vector<std::uint8_t> payload = {0x04, 0x30, 0x01, 0x02, 0x03};
    const std::uint16_t crc = sg::crc16_modbus(payload.data(), payload.size());
    assert(crc != 0);
    return 0;
}
