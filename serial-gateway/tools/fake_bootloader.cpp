#include "protocol/crc16.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

struct Options {
    int port = 19090;
    int nack_seq = -1;
    int disconnect_at_seq = -1;
    bool verify_fail = false;
};

std::uint16_t readU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8U);
}

std::uint32_t readU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) |
           (static_cast<std::uint32_t>(p[3]) << 24U);
}

void pushU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
}

bool sendAck(int fd, std::uint16_t seq, bool ok) {
    std::vector<std::uint8_t> buf;
    buf.push_back(0xAA);
    buf.push_back(0x55);
    buf.push_back(3);
    buf.push_back(ok ? 0x22 : 0x23);
    pushU16(buf, seq);
    const std::uint16_t crc = sg::crc16_modbus(&buf[2], 4);
    pushU16(buf, crc);
    return ::send(fd, buf.data(), buf.size(), 0) == static_cast<ssize_t>(buf.size());
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) opt.port = std::stoi(argv[++i]);
        else if (a == "--nack-seq" && i + 1 < argc) opt.nack_seq = std::stoi(argv[++i]);
        else if (a == "--disconnect-at-seq" && i + 1 < argc) opt.disconnect_at_seq = std::stoi(argv[++i]);
        else if (a == "--verify-fail") opt.verify_fail = true;
    }

    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return 1;
    int one = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(opt.port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return 1;
    if (::listen(server_fd, 1) != 0) return 1;

    std::cout << "fake_bootloader listening on :" << opt.port << "\n";
    int fd = ::accept(server_fd, nullptr, nullptr);
    if (fd < 0) return 1;

    std::vector<std::uint8_t> image;
    std::uint32_t expected_size = 0;
    std::uint32_t expected_crc = 0;

    while (true) {
        std::uint8_t head[4]{};
        const ssize_t hn = ::recv(fd, head, sizeof(head), MSG_WAITALL);
        if (hn <= 0) break;
        if (hn != 4 || head[0] != 0xAA || head[1] != 0x55) break;

        const std::uint8_t len = head[2];
        const std::uint8_t type = head[3];
        if (len < 1) break;
        const std::size_t payload_len = len - 1U;

        std::vector<std::uint8_t> payload(payload_len);
        if (payload_len > 0 && ::recv(fd, payload.data(), payload.size(), MSG_WAITALL) != static_cast<ssize_t>(payload.size())) break;

        std::uint8_t crc_raw[2]{};
        if (::recv(fd, crc_raw, 2, MSG_WAITALL) != 2) break;

        std::vector<std::uint8_t> crc_buf;
        crc_buf.push_back(len);
        crc_buf.push_back(type);
        crc_buf.insert(crc_buf.end(), payload.begin(), payload.end());
        const std::uint16_t wire_crc = readU16(crc_raw);
        const std::uint16_t calc_crc = sg::crc16_modbus(crc_buf.data(), crc_buf.size());
        if (wire_crc != calc_crc) break;

        std::uint16_t seq = 0;
        if (payload.size() >= 2) seq = readU16(payload.data());

        if (opt.disconnect_at_seq >= 0 && seq == static_cast<std::uint16_t>(opt.disconnect_at_seq)) {
            ::close(fd);
            break;
        }

        if (opt.nack_seq >= 0 && seq == static_cast<std::uint16_t>(opt.nack_seq)) {
            sendAck(fd, seq, false);
            opt.nack_seq = -1;
            continue;
        }

        if (type == 0x31 && payload.size() >= 12) {
            expected_size = readU32(&payload[2]);
            expected_crc = readU32(&payload[6]);
            image.clear();
            image.resize(expected_size, 0);
        } else if (type == 0x32 && payload.size() >= 10) {
            const std::uint32_t off = readU32(&payload[2]);
            const std::uint16_t n = readU16(&payload[6]);
            if (payload.size() >= static_cast<std::size_t>(8 + n) && off + n <= image.size()) {
                std::memcpy(image.data() + off, &payload[8], n);
            }
        } else if (type == 0x33) {
            if (opt.verify_fail) {
                sendAck(fd, seq, false);
                continue;
            }
            const std::uint32_t crc = sg::crc16_modbus(image.data(), image.size());
            (void)expected_crc;
            (void)crc;
        }

        if (!sendAck(fd, seq, true)) break;
    }

    ::close(server_fd);
    return 0;
}
