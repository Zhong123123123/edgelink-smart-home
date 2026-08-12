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

std::vector<std::uint8_t> buildFrame(std::uint8_t dev, double temp, double hum, double voltage, std::uint8_t status) {
    const std::int16_t t = static_cast<std::int16_t>(temp * 10.0);
    const std::uint16_t h = static_cast<std::uint16_t>(hum * 10.0);
    const std::uint16_t v = static_cast<std::uint16_t>(voltage * 1000.0);

    std::vector<std::uint8_t> frame;
    frame.push_back(0xAA);
    frame.push_back(0x55);
    frame.push_back(0x09);
    frame.push_back(0x01);
    frame.push_back(dev);
    appendU16LE(frame, static_cast<std::uint16_t>(t));
    appendU16LE(frame, h);
    appendU16LE(frame, v);
    frame.push_back(status);

    const std::uint16_t crc = sg::crc16_modbus(frame.data() + 2, 10);
    appendU16LE(frame, crc);
    return frame;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: fake_sensor <serial_dev> [interval_ms] [baudrate]\n";
        return 1;
    }

    const std::string dev = argv[1];
    const int interval_ms = (argc >= 3) ? std::stoi(argv[2]) : 500;
    const int baudrate = (argc >= 4) ? std::stoi(argv[3]) : 115200;

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

    int i = 0;
    while (true) {
        const double t = 20.0 + (i % 10) * 0.3;
        const double h = 50.0 + (i % 7) * 0.5;
        const double v = 3.70 + (i % 5) * 0.01;
        auto frame = buildFrame(1, t, h, v, static_cast<std::uint8_t>(i % 2));
        const ssize_t n = ::write(fd, frame.data(), frame.size());
        if (n < 0) {
            std::cerr << "write failed: " << std::strerror(errno) << "\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        ++i;
    }

    return 0;
}
