#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cctype>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

enum class AckMode {
    OK,
    FAIL,
    TIMEOUT,
    LATE,
};

struct Opts {
    std::string host = "127.0.0.1";
    int port = 9100;
    int device_id = 2;
    int period_ms = 1000;
    AckMode ack_mode = AckMode::OK;
    int ack_result = 1;
    int ack_delay_ms = 7000;
    int disconnect_after_sec = 0;
    bool reconnect = true;
    int reconnect_delay_ms = 500;
    int max_reconnects = -1;
};

bool startsWith(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }

int toInt(const std::string& s, int defv = 0) {
    try {
        return std::stoi(s);
    } catch (...) {
        return defv;
    }
}

void parseArgs(int argc, char** argv, Opts& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (startsWith(a, "--host=")) o.host = a.substr(7);
        else if (startsWith(a, "--port=")) o.port = toInt(a.substr(7), o.port);
        else if (startsWith(a, "--device-id=")) o.device_id = toInt(a.substr(12), o.device_id);
        else if (startsWith(a, "--period-ms=")) o.period_ms = toInt(a.substr(12), o.period_ms);
        else if (startsWith(a, "--ack-mode=")) {
            const std::string v = a.substr(11);
            if (v == "ok") o.ack_mode = AckMode::OK;
            else if (v == "fail") o.ack_mode = AckMode::FAIL;
            else if (v == "timeout") o.ack_mode = AckMode::TIMEOUT;
            else if (v == "late") o.ack_mode = AckMode::LATE;
        } else if (startsWith(a, "--ack-result=")) o.ack_result = toInt(a.substr(13), o.ack_result);
        else if (startsWith(a, "--ack-delay-ms=")) o.ack_delay_ms = toInt(a.substr(15), o.ack_delay_ms);
        else if (startsWith(a, "--disconnect-after-sec=")) o.disconnect_after_sec = toInt(a.substr(23), o.disconnect_after_sec);
        else if (startsWith(a, "--reconnect=")) o.reconnect = (a.substr(12) != "false");
        else if (startsWith(a, "--reconnect-delay-ms=")) o.reconnect_delay_ms = toInt(a.substr(21), o.reconnect_delay_ms);
        else if (startsWith(a, "--max-reconnects=")) o.max_reconnects = toInt(a.substr(17), o.max_reconnects);
    }
}

uint64_t nowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

int connectTo(const Opts& o) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(o.port));
    if (inet_pton(AF_INET, o.host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool writeLine(int fd, const std::string& line) {
    std::string data = line + "\n";
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n > 0) off += static_cast<size_t>(n);
        else return false;
    }
    return true;
}

std::string makeSensorJson(const Opts& o, uint64_t seq, double t, double h, int rssi) {
    std::ostringstream oss;
    oss << "{"
        << "\"device_id\":" << o.device_id << ","
        << "\"device_name\":\"wifi-node-02\"," 
        << "\"event_type\":\"sensor_data\"," 
        << "\"timestamp\":" << nowMs() << ","
        << "\"temperature\":" << t << ","
        << "\"humidity\":" << h << ","
        << "\"voltage\":3.3,"
        << "\"status\":1,"
        << "\"link_type\":\"wifi\"," 
        << "\"wifi_rssi\":" << rssi << ","
        << "\"seq\":" << seq
        << "}";
    return oss.str();
}

std::string makeHeartbeatJson(const Opts& o, uint64_t seq, int rssi) {
    std::ostringstream oss;
    oss << "{"
        << "\"device_id\":" << o.device_id << ","
        << "\"device_name\":\"wifi-node-02\"," 
        << "\"event_type\":\"heartbeat\"," 
        << "\"timestamp\":" << nowMs() << ","
        << "\"link_type\":\"wifi\"," 
        << "\"wifi_rssi\":" << rssi << ","
        << "\"seq\":" << seq
        << "}";
    return oss.str();
}

std::string makeAckJson(const Opts& o, uint64_t seq, int rssi, int command_id, int result) {
    std::ostringstream oss;
    oss << "{"
        << "\"device_id\":" << o.device_id << ","
        << "\"device_name\":\"wifi-node-02\"," 
        << "\"event_type\":\"command_ack\"," 
        << "\"timestamp\":" << nowMs() << ","
        << "\"command_id\":" << command_id << ","
        << "\"command_result\":" << result << ","
        << "\"payload_summary\":\"cmd_id=" << command_id << ",result=" << result << "\"," 
        << "\"link_type\":\"wifi\"," 
        << "\"wifi_rssi\":" << rssi << ","
        << "\"seq\":" << seq
        << "}";
    return oss.str();
}

std::string getJsonString(const std::string& s, const std::string& key) {
    const std::string p = "\"" + key + "\"";
    const size_t k = s.find(p);
    if (k == std::string::npos) return "";
    const size_t c = s.find(':', k + p.size());
    if (c == std::string::npos) return "";
    const size_t q1 = s.find('"', c + 1);
    if (q1 == std::string::npos) return "";
    const size_t q2 = s.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return s.substr(q1 + 1, q2 - q1 - 1);
}

int getJsonInt(const std::string& s, const std::string& key, int defv = 0) {
    const std::string p = "\"" + key + "\"";
    const size_t k = s.find(p);
    if (k == std::string::npos) return defv;
    const size_t c = s.find(':', k + p.size());
    if (c == std::string::npos) return defv;
    size_t b = c + 1;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])) != 0) ++b;
    size_t e = b;
    if (e < s.size() && (s[e] == '-' || s[e] == '+')) ++e;
    while (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e])) != 0) ++e;
    if (e <= b) return defv;
    return toInt(s.substr(b, e - b), defv);
}

} // namespace

int main(int argc, char** argv) {
    Opts opts;
    parseArgs(argc, argv, opts);

    uint64_t seq = 1000;
    int rssi = -55;
    int reconnect_count = 0;

    while (true) {
        int fd = connectTo(opts);
        if (fd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(opts.reconnect_delay_ms));
            continue;
        }

        std::string pending;
        auto last_sensor = std::chrono::steady_clock::now() - std::chrono::milliseconds(opts.period_ms);
        auto last_hb = std::chrono::steady_clock::now() - std::chrono::seconds(5);
        auto start_conn = std::chrono::steady_clock::now();
        std::vector<std::string> delayed_acks;
        std::vector<std::chrono::steady_clock::time_point> delayed_due;

        while (true) {
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sensor).count() >= opts.period_ms) {
                const double phase = static_cast<double>(seq % 40) / 40.0;
                const double t = 26.0 + 4.0 * phase;
                const double h = 40.0 + 20.0 * (1.0 - phase);
                rssi = -70 + static_cast<int>(seq % 20);
                if (!writeLine(fd, makeSensorJson(opts, seq++, t, h, rssi))) break;
                last_sensor = now;
            }
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_hb).count() >= 5) {
                if (!writeLine(fd, makeHeartbeatJson(opts, seq++, rssi))) break;
                last_hb = now;
            }

            for (size_t i = 0; i < delayed_acks.size();) {
                if (now >= delayed_due[i]) {
                    if (!writeLine(fd, delayed_acks[i])) {
                        delayed_acks.clear();
                        delayed_due.clear();
                        break;
                    }
                    delayed_acks.erase(delayed_acks.begin() + static_cast<long>(i));
                    delayed_due.erase(delayed_due.begin() + static_cast<long>(i));
                } else {
                    ++i;
                }
            }

            char buf[1024] = {0};
            const ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n > 0) {
                pending.append(buf, static_cast<size_t>(n));
                size_t pos = 0;
                while (true) {
                    const size_t eol = pending.find('\n', pos);
                    if (eol == std::string::npos) {
                        pending = pending.substr(pos);
                        break;
                    }
                    const std::string line = pending.substr(pos, eol - pos);
                    pos = eol + 1;
                    if (line.empty()) continue;
                    const std::string type = getJsonString(line, "type");
                    if (type != "command") continue;
                    const std::string cmd = getJsonString(line, "command_type");
                    const int cmd_id = getJsonInt(line, "command_id", 0);
                    int result = 0;
                    if (cmd != "get_status" && cmd != "set_led" && cmd != "set_report_interval" && cmd != "set_log_level") {
                        result = 1;
                    }
                    if (opts.ack_mode == AckMode::FAIL) {
                        result = opts.ack_result;
                    }
                    if (opts.ack_mode == AckMode::OK || opts.ack_mode == AckMode::FAIL) {
                        if (!writeLine(fd, makeAckJson(opts, seq++, rssi, cmd_id, result))) {
                            break;
                        }
                    } else if (opts.ack_mode == AckMode::LATE) {
                        delayed_acks.push_back(makeAckJson(opts, seq++, rssi, cmd_id, 0));
                        delayed_due.push_back(now + std::chrono::milliseconds(opts.ack_delay_ms));
                    }
                }
            } else if (n == 0) {
                break;
            }

            if (opts.disconnect_after_sec > 0 &&
                std::chrono::duration_cast<std::chrono::seconds>(now - start_conn).count() >= opts.disconnect_after_sec) {
                ::close(fd);
                fd = -1;
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (fd >= 0) ::close(fd);

        reconnect_count++;
        if (!opts.reconnect) break;
        if (opts.max_reconnects >= 0 && reconnect_count > opts.max_reconnects) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(opts.reconnect_delay_ms));
    }

    return 0;
}
