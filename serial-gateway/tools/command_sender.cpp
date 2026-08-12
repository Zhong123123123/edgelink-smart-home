#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::uint16_t crc16Modbus(const std::vector<std::uint8_t>& data) {
    std::uint16_t crc = 0xFFFF;
    for (std::uint8_t b : data) {
        crc ^= static_cast<std::uint16_t>(b);
        for (int i = 0; i < 8; ++i) {
            if ((crc & 0x0001U) != 0U) {
                crc = static_cast<std::uint16_t>((crc >> 1U) ^ 0xA001U);
            } else {
                crc = static_cast<std::uint16_t>(crc >> 1U);
            }
        }
    }
    return crc;
}

bool parseIntInRange(const std::string& s, int min_v, int max_v, int& out) {
    try {
        std::size_t pos = 0;
        const int v = std::stoi(s, &pos);
        if (pos != s.size() || v < min_v || v > max_v) {
            return false;
        }
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

bool parseFloat(const std::string& s, float& out) {
    try {
        std::size_t pos = 0;
        const float v = std::stof(s, &pos);
        if (pos != s.size()) {
            return false;
        }
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

std::uint16_t floatToU16x10(float value) {
    if (value <= 0.0f) return 0U;
    int scaled = static_cast<int>(value * 10.0f + 0.5f);
    scaled = std::clamp(scaled, 0, 65535);
    return static_cast<std::uint16_t>(scaled);
}

std::string bytesToHex(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setfill('0');
    for (std::uint8_t b : bytes) {
        oss << std::setw(2) << static_cast<unsigned int>(b);
    }
    return oss.str();
}

bool buildDeviceCommandFrame(const std::vector<std::string>& args, std::uint8_t device_id,
                             std::uint16_t command_id, std::string& command_name,
                             std::vector<std::uint8_t>& out_frame, std::string& err) {
    if (args.empty()) {
        err = "missing device command";
        return false;
    }

    const std::string cmd = args[0];
    command_name = cmd;

    std::vector<std::uint8_t> payload;
    payload.push_back(device_id);
    payload.push_back(static_cast<std::uint8_t>(command_id & 0xFFU));
    payload.push_back(static_cast<std::uint8_t>((command_id >> 8U) & 0xFFU));

    if (cmd == "set_led" || cmd == "set_buzzer" || cmd == "set_mode") {
        if (args.size() < 2) {
            err = "command needs value 0|1";
            return false;
        }
        int v = 0;
        if (!parseIntInRange(args[1], 0, 1, v)) {
            err = "value must be 0 or 1";
            return false;
        }
        if (cmd == "set_led") payload.push_back(0x01U);
        if (cmd == "set_buzzer") payload.push_back(0x02U);
        if (cmd == "set_mode") payload.push_back(0x03U);
        payload.push_back(static_cast<std::uint8_t>(v));
    } else if (cmd == "get_status") {
        payload.push_back(0x04U);
    } else if (cmd == "set_threshold") {
        if (args.size() < 3) {
            err = "set_threshold needs temp humi";
            return false;
        }
        float temp = 0.0f;
        float humi = 0.0f;
        if (!parseFloat(args[1], temp) || !parseFloat(args[2], humi)) {
            err = "threshold values must be float";
            return false;
        }
        const std::uint16_t t = floatToU16x10(temp);
        const std::uint16_t h = floatToU16x10(humi);
        payload.push_back(0x05U);
        payload.push_back(static_cast<std::uint8_t>(t & 0xFFU));
        payload.push_back(static_cast<std::uint8_t>((t >> 8U) & 0xFFU));
        payload.push_back(static_cast<std::uint8_t>(h & 0xFFU));
        payload.push_back(static_cast<std::uint8_t>((h >> 8U) & 0xFFU));
    } else if (cmd == "set_log_level") {
        if (args.size() < 2) {
            err = "set_log_level needs level(0:error 1:warn 2:info)";
            return false;
        }
        int level = 0;
        if (!parseIntInRange(args[1], 0, 2, level)) {
            err = "log level must be 0..2";
            return false;
        }
        payload.push_back(0x06U);
        payload.push_back(static_cast<std::uint8_t>(level));
    } else if (cmd == "reboot_to_bootloader") {
        payload.push_back(0x07U);
    } else {
        err = "unsupported command: " + cmd;
        return false;
    }

    out_frame.clear();
    out_frame.push_back(0xAAU);
    out_frame.push_back(0x55U);
    out_frame.push_back(static_cast<std::uint8_t>(payload.size() + 1U));
    out_frame.push_back(0x10U);
    out_frame.insert(out_frame.end(), payload.begin(), payload.end());

    std::vector<std::uint8_t> crc_data;
    crc_data.reserve(payload.size() + 2U);
    crc_data.push_back(out_frame[2]);
    crc_data.push_back(out_frame[3]);
    crc_data.insert(crc_data.end(), payload.begin(), payload.end());
    const std::uint16_t crc = crc16Modbus(crc_data);
    out_frame.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    out_frame.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xFFU));
    return true;
}

int openSocket(const std::string& host, int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "socket failed\n";
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "invalid host\n";
        ::close(fd);
        return -1;
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "connect failed: " << std::strerror(errno) << "\n";
        ::close(fd);
        return -1;
    }
    return fd;
}

void printUsage() {
    std::cerr
        << "Usage:\n"
        << "  command_sender <host> <port> <HEX|TEXT> <payload>\n"
        << "  command_sender <host> <port> RAW <line>\n"
        << "  command_sender <host> <port> DEVCMD <device_id> <command_id> <timeout_ms> "
           "<set_led|set_buzzer|set_mode|get_status|set_threshold|set_log_level|reboot_to_bootloader> [args...]\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        printUsage();
        return 1;
    }

    const std::string host = argv[1];
    int port = 0;
    if (!parseIntInRange(argv[2], 1, 65535, port)) {
        std::cerr << "invalid port\n";
        return 1;
    }
    const std::string mode = argv[3];

    std::string line;
    if (mode == "HEX" || mode == "TEXT") {
        if (argc < 5) {
            printUsage();
            return 1;
        }
        line = mode + " " + argv[4] + "\n";
    } else if (mode == "RAW") {
        if (argc < 5) {
            printUsage();
            return 1;
        }
        line = std::string(argv[4]) + "\n";
    } else if (mode == "DEVCMD") {
        if (argc < 8) {
            printUsage();
            return 1;
        }
        int dev = 0;
        int cmd_id = 0;
        int timeout_ms = 0;
        if (!parseIntInRange(argv[4], 0, 255, dev) || !parseIntInRange(argv[5], 0, 65535, cmd_id) ||
            !parseIntInRange(argv[6], 10, 60000, timeout_ms)) {
            std::cerr << "invalid DEVCMD parameters\n";
            return 1;
        }
        std::vector<std::string> cmd_args;
        for (int i = 7; i < argc; ++i) {
            cmd_args.emplace_back(argv[i]);
        }
        std::vector<std::uint8_t> frame;
        std::string cmd_name;
        std::string err;
        if (!buildDeviceCommandFrame(cmd_args, static_cast<std::uint8_t>(dev),
                                     static_cast<std::uint16_t>(cmd_id), cmd_name, frame, err)) {
            std::cerr << "DEVCMD build failed: " << err << "\n";
            return 1;
        }
        line = "TRACK_HEX " + std::to_string(dev) + " " + std::to_string(cmd_id) + " " + cmd_name + " " +
               std::to_string(timeout_ms) + " " + bytesToHex(frame) + "\n";
    } else {
        printUsage();
        return 1;
    }

    const int fd = openSocket(host, port);
    if (fd < 0) {
        return 2;
    }

    if (::write(fd, line.data(), line.size()) < 0) {
        std::cerr << "write failed\n";
        ::close(fd);
        return 3;
    }

    char buf[256] = {0};
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        std::cout << std::string(buf, buf + n);
    }

    ::close(fd);
    return 0;
}
