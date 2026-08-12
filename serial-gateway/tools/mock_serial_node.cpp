#include "protocol/crc16.hpp"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

speed_t toSpeed(int baudrate) {
    switch (baudrate) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        default: return B115200;
    }
}

bool setupSerial(int fd, int baudrate) {
    termios tty{};
    if (tcgetattr(fd, &tty) != 0) return false;
    cfmakeraw(&tty);
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CSIZE);
    tty.c_cflag |= CS8;
    cfsetispeed(&tty, toSpeed(baudrate));
    cfsetospeed(&tty, toSpeed(baudrate));
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;
    return tcsetattr(fd, TCSANOW, &tty) == 0;
}

void appendU16LE(std::vector<std::uint8_t>& v, std::uint16_t val) {
    v.push_back(static_cast<std::uint8_t>(val & 0xFFU));
    v.push_back(static_cast<std::uint8_t>((val >> 8U) & 0xFFU));
}

std::vector<std::uint8_t> buildFrame(std::uint8_t frame_type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> frame;
    frame.push_back(0xAA);
    frame.push_back(0x55);
    frame.push_back(static_cast<std::uint8_t>(payload.size() + 1));
    frame.push_back(frame_type);
    frame.insert(frame.end(), payload.begin(), payload.end());
    const std::uint16_t crc = sg::crc16_modbus(frame.data() + 2, frame.size() - 2);
    appendU16LE(frame, crc);
    return frame;
}

std::vector<std::uint8_t> buildSensorData(std::uint8_t dev, double temp, double hum, double voltage, std::uint8_t status) {
    std::vector<std::uint8_t> payload;
    payload.push_back(dev);
    appendU16LE(payload, static_cast<std::uint16_t>(static_cast<std::int16_t>(temp * 10.0)));
    appendU16LE(payload, static_cast<std::uint16_t>(hum * 10.0));
    appendU16LE(payload, static_cast<std::uint16_t>(voltage * 1000.0));
    payload.push_back(status);
    return buildFrame(0x01, payload);
}

std::vector<std::uint8_t> buildHeartbeat(std::uint8_t dev) {
    return buildFrame(0x03, std::vector<std::uint8_t>{dev});
}

std::vector<std::uint8_t> buildCommandAck(std::uint8_t dev, std::uint16_t cmd_id, std::uint8_t result) {
    std::vector<std::uint8_t> payload;
    payload.push_back(dev);
    appendU16LE(payload, cmd_id);
    payload.push_back(result);
    return buildFrame(0x04, payload);
}

bool parseInt(const std::string& s, int& out) {
    try { out = std::stoi(s); return true; } catch (...) { return false; }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: mock_serial_node <serial_dev> [device_id] [period_ms] [baudrate]\n";
        return 1;
    }

    const std::string dev = argv[1];
    int device_id = 1;
    int period_ms = 1000;
    int baudrate = 115200;
    if (argc >= 3) parseInt(argv[2], device_id);
    if (argc >= 4) parseInt(argv[3], period_ms);
    if (argc >= 5) parseInt(argv[4], baudrate);

    const int fd = ::open(dev.c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) {
        std::cerr << "open failed: " << std::strerror(errno) << "\n";
        return 2;
    }
    if (!setupSerial(fd, baudrate)) {
        std::cerr << "setup serial failed\n";
        ::close(fd);
        return 3;
    }

    std::vector<std::uint8_t> rx;
    int i = 0;
    auto last_hb = std::chrono::steady_clock::now();
    while (true) {
        const double t = 20.0 + (i % 10) * 0.3;
        const double h = 45.0 + (i % 7) * 0.6;
        const double v = 3.30;
        const auto sensor = buildSensorData(static_cast<std::uint8_t>(device_id), t, h, v, static_cast<std::uint8_t>(i % 2));
        ::write(fd, sensor.data(), sensor.size());
        if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - last_hb).count() >= 5) {
            const auto hb = buildHeartbeat(static_cast<std::uint8_t>(device_id));
            ::write(fd, hb.data(), hb.size());
            last_hb = std::chrono::steady_clock::now();
        }

        std::uint8_t buf[256] = {0};
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            rx.insert(rx.end(), buf, buf + n);
            while (rx.size() >= 6) {
                if (rx[0] != 0xAA || rx[1] != 0x55) {
                    rx.erase(rx.begin());
                    continue;
                }
                const std::size_t len = rx[2];
                const std::size_t total = 2 + 1 + len + 2;
                if (rx.size() < total) break;
                const std::uint8_t type = rx[3];
                if (type == 0x10 && len >= 5) {
                    const std::uint8_t dev_id = rx[4];
                    const std::uint16_t cmd_id = static_cast<std::uint16_t>(rx[5]) | (static_cast<std::uint16_t>(rx[6]) << 8U);
                    const auto ack = buildCommandAck(dev_id, cmd_id, 0);
                    ::write(fd, ack.data(), ack.size());
                }
                rx.erase(rx.begin(), rx.begin() + static_cast<long>(total));
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
        ++i;
    }

    return 0;
}
