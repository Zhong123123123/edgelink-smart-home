#include <mosquitto.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::optional<std::string> jsonGetString(const std::string& s, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    const std::size_t kp = s.find(pattern);
    if (kp == std::string::npos) return std::nullopt;
    const std::size_t colon = s.find(':', kp + pattern.size());
    if (colon == std::string::npos) return std::nullopt;
    const std::size_t q1 = s.find('"', colon + 1);
    if (q1 == std::string::npos) return std::nullopt;
    const std::size_t q2 = s.find('"', q1 + 1);
    if (q2 == std::string::npos) return std::nullopt;
    return s.substr(q1 + 1, q2 - q1 - 1);
}

std::optional<long long> jsonGetInt(const std::string& s, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    const std::size_t kp = s.find(pattern);
    if (kp == std::string::npos) return std::nullopt;
    const std::size_t colon = s.find(':', kp + pattern.size());
    if (colon == std::string::npos) return std::nullopt;
    std::size_t p = colon + 1;
    while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p])) != 0) ++p;
    std::size_t e = p;
    if (e < s.size() && (s[e] == '-' || s[e] == '+')) ++e;
    while (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e])) != 0) ++e;
    if (e == p || (e == p + 1 && (s[p] == '-' || s[p] == '+'))) return std::nullopt;
    try {
        return std::stoll(s.substr(p, e - p));
    } catch (...) {
        return std::nullopt;
    }
}

std::string jsonEscape(const std::string& s) {
    std::ostringstream oss;
    for (char c : s) {
        switch (c) {
            case '\\': oss << "\\\\"; break;
            case '"': oss << "\\\""; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default: oss << c; break;
        }
    }
    return oss.str();
}

std::string canonicalCommand(const std::string& cmd) {
    if (cmd == "led_set") return "set_led";
    if (cmd == "status_get") return "get_status";
    if (cmd == "report_interval_set") return "set_report_interval";
    return cmd;
}

std::uint16_t generatedCommandId() {
    using clock = std::chrono::steady_clock;
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            clock::now().time_since_epoch())
                            .count();
    return static_cast<std::uint16_t>(1000 + (now_ms % 50000));
}

struct TopicInfo {
    bool valid{false};
    int device_id{0};
    bool ota_start{false};
};

TopicInfo parseTopic(const std::string& topic) {
    TopicInfo info{};
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= topic.size()) {
        const std::size_t slash = topic.find('/', start);
        if (slash == std::string::npos) {
            parts.push_back(topic.substr(start));
            break;
        }
        parts.push_back(topic.substr(start, slash - start));
        start = slash + 1;
    }
    if (parts.size() < 6 || parts[0] != "gateway" || parts[2] != "device") {
        return info;
    }
    try {
        info.device_id = std::stoi(parts[3]);
    } catch (...) {
        return info;
    }
    if (parts[4] == "command" && parts[5] == "down") {
        info.valid = true;
        return info;
    }
    if (parts[4] == "ota" && parts[5] == "start") {
        info.valid = true;
        info.ota_start = true;
        return info;
    }
    return info;
}

std::string buildForwardJson(const TopicInfo& info, const std::string& payload) {
    std::string cmd = jsonGetString(payload, "cmd").value_or("");
    if (cmd.empty()) {
        cmd = jsonGetString(payload, "command_type").value_or("");
    }
    if (cmd.empty() && info.ota_start) {
        cmd = "ota_start";
    }
    cmd = canonicalCommand(cmd);
    if (cmd.empty()) {
        throw std::runtime_error("missing cmd/command_type");
    }

    std::uint16_t command_id = generatedCommandId();
    if (const auto cid = jsonGetInt(payload, "command_id"); cid.has_value() && *cid > 0 && *cid <= 65535) {
        command_id = static_cast<std::uint16_t>(*cid);
    }
    int timeout_ms = 3000;
    if (const auto t = jsonGetInt(payload, "timeout_ms"); t.has_value() && *t > 0 && *t < 10000) {
        timeout_ms = static_cast<int>(*t);
    }

    std::ostringstream oss;
    oss << "{"
        << "\"type\":\"command\","
        << "\"command\":\"mqtt_bridge\","
        << "\"device_id\":" << info.device_id << ","
        << "\"command_id\":" << command_id << ","
        << "\"command_type\":\"" << jsonEscape(cmd) << "\","
        << "\"timeout_ms\":" << timeout_ms;

    if (const auto rawCmd = jsonGetString(payload, "cmd"); rawCmd.has_value()) {
        oss << ",\"cmd\":\"" << jsonEscape(*rawCmd) << "\"";
    }
    if (const auto value = jsonGetInt(payload, "value"); value.has_value()) {
        oss << ",\"value\":" << *value;
    }
    if (const auto interval = jsonGetInt(payload, "interval_ms"); interval.has_value()) {
        oss << ",\"interval_ms\":" << *interval;
    }
    if (const auto url = jsonGetString(payload, "firmware_url"); url.has_value()) {
        oss << ",\"firmware_url\":\"" << jsonEscape(*url) << "\"";
    }
    if (const auto version = jsonGetString(payload, "version"); version.has_value()) {
        oss << ",\"version\":\"" << jsonEscape(*version) << "\"";
    }
    if (const auto crc32 = jsonGetString(payload, "crc32"); crc32.has_value()) {
        oss << ",\"crc32\":\"" << jsonEscape(*crc32) << "\"";
    }
    if (const auto size = jsonGetInt(payload, "size"); size.has_value()) {
        oss << ",\"size\":" << *size;
    }
    oss << "}";
    return oss.str();
}

bool forwardToCommandSocket(const std::string& host, int port, const std::string& line, std::string& resp) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return false;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }

    const std::string wire = line + "\n";
    if (::send(fd, wire.data(), wire.size(), 0) != static_cast<ssize_t>(wire.size())) {
        ::close(fd);
        return false;
    }

    char buf[512]{};
    const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        resp.assign(buf, buf + n);
    }
    ::close(fd);
    return true;
}

struct BridgeContext {
    std::string command_host;
    int command_port{9001};
};

void onMessage(struct mosquitto*, void* userdata, const struct mosquitto_message* msg) {
    auto* ctx = static_cast<BridgeContext*>(userdata);
    if (ctx == nullptr || msg == nullptr || msg->topic == nullptr || msg->payload == nullptr) {
        return;
    }

    const std::string topic = msg->topic;
    const std::string payload(static_cast<const char*>(msg->payload), static_cast<std::size_t>(msg->payloadlen));
    const TopicInfo info = parseTopic(topic);
    if (!info.valid) {
        return;
    }

    try {
        const std::string forward = buildForwardJson(info, payload);
        std::string resp;
        if (!forwardToCommandSocket(ctx->command_host, ctx->command_port, forward, resp)) {
            std::cerr << "forward failed topic=" << topic << "\n";
            return;
        }
        std::cout << "[bridge] topic=" << topic << " payload=" << payload << " resp=" << resp;
        if (!resp.empty() && resp.back() != '\n') {
            std::cout << "\n";
        }
    } catch (const std::exception& ex) {
        std::cerr << "bridge parse failed topic=" << topic << " err=" << ex.what() << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: mqtt_command_bridge <mqtt_host> <mqtt_port> <command_host> <command_port>\n";
        return 1;
    }

    const std::string mqtt_host = argv[1];
    const int mqtt_port = std::atoi(argv[2]);
    BridgeContext ctx;
    ctx.command_host = argv[3];
    ctx.command_port = std::atoi(argv[4]);

    mosquitto_lib_init();
    mosquitto* mosq = mosquitto_new("serial-gateway-mqtt-command-bridge", true, &ctx);
    if (mosq == nullptr) {
        std::cerr << "mosquitto_new failed\n";
        mosquitto_lib_cleanup();
        return 1;
    }

    mosquitto_message_callback_set(mosq, onMessage);

    if (mosquitto_connect(mosq, mqtt_host.c_str(), mqtt_port, 60) != MOSQ_ERR_SUCCESS) {
        std::cerr << "mqtt connect failed to " << mqtt_host << ':' << mqtt_port << "\n";
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    if (mosquitto_subscribe(mosq, nullptr, "gateway/+/device/+/command/down", 0) != MOSQ_ERR_SUCCESS ||
        mosquitto_subscribe(mosq, nullptr, "gateway/+/device/+/ota/start", 0) != MOSQ_ERR_SUCCESS) {
        std::cerr << "mqtt subscribe failed\n";
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    std::cout << "mqtt_command_bridge listening mqtt=" << mqtt_host << ":" << mqtt_port
              << " command=" << ctx.command_host << ":" << ctx.command_port << "\n";

    while (true) {
        const int rc = mosquitto_loop(mosq, 500, 1);
        if (rc == MOSQ_ERR_SUCCESS) {
            continue;
        }
        std::cerr << "mqtt loop rc=" << rc << ", retrying\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (mosquitto_reconnect(mosq) != MOSQ_ERR_SUCCESS) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    return 0;
}
