#include "app/app_controller.hpp"

#include "common/logger.hpp"
#include "common/mqtt_runtime.hpp"
#include "ota/esp32_ota_adapter.hpp"
#include "ota/firmware_store.hpp"
#include "ota/stm32_ota_adapter.hpp"
#include "ota/ota_task_manager.hpp"
#include "protocol/crc16.hpp"
#include "storage/history_store.hpp"
#include "storage/sqlite_store.hpp"
#include "runtime/resource_usage.hpp"
#include "uploader/uploader_factory.hpp"

#ifdef SG_HAVE_MOSQUITTO
#include <mosquitto.h>
#endif

#include <arpa/inet.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <functional>
#include <future>
#include <netinet/in.h>
#include <mutex>
#include <optional>
#include <sstream>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <climits>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <deque>

namespace sg {

namespace {
std::mutex g_ack_debug_mutex;
std::deque<std::string> g_ack_debug_lines;
std::deque<std::uint64_t> g_ack_rx_ts_ms;
std::deque<std::uint64_t> g_ack_tx_ok_ts_ms;
std::deque<std::uint64_t> g_ack_tx_fail_ts_ms;

void ackDebugAddLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_ack_debug_mutex);
    g_ack_debug_lines.push_back(line);
    while (g_ack_debug_lines.size() > 200) g_ack_debug_lines.pop_front();
}

void ackDebugPruneOld(std::deque<std::uint64_t>& q, std::uint64_t now_ms) {
    const std::uint64_t window_ms = 60000ULL;
    while (!q.empty() && now_ms > q.front() && (now_ms - q.front()) > window_ms) q.pop_front();
}

void ackDebugMarkRx(std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(g_ack_debug_mutex);
    g_ack_rx_ts_ms.push_back(now_ms);
    ackDebugPruneOld(g_ack_rx_ts_ms, now_ms);
}

void ackDebugMarkTx(std::uint64_t now_ms, bool ok) {
    std::lock_guard<std::mutex> lock(g_ack_debug_mutex);
    if (ok) {
        g_ack_tx_ok_ts_ms.push_back(now_ms);
        ackDebugPruneOld(g_ack_tx_ok_ts_ms, now_ms);
    } else {
        g_ack_tx_fail_ts_ms.push_back(now_ms);
        ackDebugPruneOld(g_ack_tx_fail_ts_ms, now_ms);
    }
}

std::string trim(const std::string& s) {
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])) != 0) {
        ++b;
    }
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) {
        --e;
    }
    return s.substr(b, e - b);
}

std::filesystem::path gatewayRootDir() {
    static const std::filesystem::path path = []() {
        std::array<char, 4096> buf{};
        const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
        if (n <= 0) {
            return std::filesystem::current_path();
        }
        std::filesystem::path exe_path(std::string(buf.data(), static_cast<std::size_t>(n)));
        std::filesystem::path dir = exe_path.parent_path();
        if (dir.filename() == "build") {
            dir = dir.parent_path();
        }
        return dir;
    }();
    return path;
}

std::string gatewayRuntimePath(const std::string& relative_path) {
    return (gatewayRootDir() / relative_path).string();
}

std::string otaTaskStorePath() {
    const std::filesystem::path new_path = gatewayRootDir() / "data/ota_tasks.db";
    const std::filesystem::path legacy_path = gatewayRootDir() / "data/ota_tasks.json";

    std::error_code ec;
    std::filesystem::create_directories(new_path.parent_path(), ec);
    if (!std::filesystem::exists(new_path, ec) && std::filesystem::exists(legacy_path, ec)) {
        std::filesystem::rename(legacy_path, new_path, ec);
        if (ec) {
            ec.clear();
            std::filesystem::copy_file(legacy_path, new_path, std::filesystem::copy_options::overwrite_existing, ec);
        }
    }
    return new_path.string();
}

bool parseHexByte(const std::string& s, std::uint8_t& out) {
    if (s.size() != 2) {
        return false;
    }
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    const int hi = hexVal(s[0]);
    const int lo = hexVal(s[1]);
    if (hi < 0 || lo < 0) {
        return false;
    }
    out = static_cast<std::uint8_t>((hi << 4) | lo);
    return true;
}

bool parseHexPayload(const std::string& raw, std::vector<std::uint8_t>& out) {
    out.clear();
    std::string compact;
    compact.reserve(raw.size());
    for (char c : raw) {
        if (std::isspace(static_cast<unsigned char>(c)) == 0) {
            compact.push_back(c);
        }
    }
    if (compact.empty() || (compact.size() % 2) != 0) {
        return false;
    }

    for (std::size_t i = 0; i < compact.size(); i += 2) {
        std::uint8_t b = 0;
        if (!parseHexByte(compact.substr(i, 2), b)) {
            return false;
        }
        out.push_back(b);
    }
    return true;
}

bool parseIntInRange(const std::string& s, int min_val, int max_val, int& out) {
    try {
        std::size_t pos = 0;
        const int v = std::stoi(s, &pos);
        if (pos != s.size()) {
            return false;
        }
        if (v < min_val || v > max_val) {
            return false;
        }
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

std::uint64_t unixMsNowLocal() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::vector<std::uint8_t> buildReportAckFrame(std::uint8_t device_id, std::uint32_t report_seq) {
    std::vector<std::uint8_t> frame;
    const std::uint8_t payload_len = 5U;
    frame.reserve(2 + 1 + 1 + payload_len + 2);
    frame.push_back(0xAAU);
    frame.push_back(0x55U);
    frame.push_back(static_cast<std::uint8_t>(payload_len + 1U));
    frame.push_back(static_cast<std::uint8_t>(FrameType::REPORT_ACK));
    frame.push_back(device_id);
    frame.push_back(static_cast<std::uint8_t>(report_seq & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>((report_seq >> 8U) & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>((report_seq >> 16U) & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>((report_seq >> 24U) & 0xFFU));
    const auto crc = crc16_modbus(frame.data() + 2, static_cast<std::size_t>(1 + frame[2]));
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xFFU));
    return frame;
}

ssize_t sendReportAckToSerial(SerialPort& serial, std::uint8_t device_id, std::uint32_t report_seq) {
    std::string err;
    const auto frame = buildReportAckFrame(device_id, report_seq);
    return serial.write(frame.data(), frame.size(), err);
}

ssize_t sendReportAckToFd(int fd, std::uint8_t device_id, std::uint32_t report_seq) {
    const auto frame = buildReportAckFrame(device_id, report_seq);
    std::size_t off = 0;
    while (off < frame.size()) {
        const ssize_t n = ::write(fd, frame.data() + off, frame.size() - off);
        if (n > 0) {
            off += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        return (off == 0) ? n : static_cast<ssize_t>(off);
    }
    return static_cast<ssize_t>(off);
}

struct TrackCommandRequest {
    bool tracked = false;
    bool use_text = false;
    std::uint8_t device_id = 0;
    std::uint16_t command_id = 0;
    std::string command_type;
    int timeout_ms = 0;
    std::vector<std::uint8_t> payload;
};

struct MqttTopicParts {
    bool valid = false;
    std::string gateway_id;
    std::uint8_t device_id = 0;
    std::string category;
    std::string action;
};

MqttTopicParts parseMqttDeviceTopic(const std::string& topic) {
    MqttTopicParts out{};
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
    if ((parts.size() != 5 && parts.size() != 6) || parts[0] != "gateway" || parts[2] != "device") {
        return out;
    }
    int device_id = 0;
    if (!parseIntInRange(parts[3], 0, 255, device_id)) {
        return out;
    }
    out.valid = true;
    out.gateway_id = parts[1];
    out.device_id = static_cast<std::uint8_t>(device_id);
    out.category = parts[4];
    out.action = (parts.size() == 6) ? parts[5] : "";
    return out;
}

bool buildMqttOtaStartPayload(const ota::Esp32OtaStart& req, std::string& payload);


bool skipJsonStringToken(const std::string& s, std::size_t& pos) {
    if (pos >= s.size() || s[pos] != '"') {
        return false;
    }
    ++pos;
    while (pos < s.size()) {
        if (s[pos] == '\\') {
            pos += (pos + 1 < s.size()) ? 2 : 1;
            continue;
        }
        if (s[pos] == '"') {
            ++pos;
            return true;
        }
        ++pos;
    }
    return false;
}

std::optional<std::string> decodeJsonStringToken(const std::string& s,
                                                 std::size_t begin,
                                                 std::size_t end) {
    if (end <= begin || s[begin] != '"' || s[end - 1] != '"') {
        return std::nullopt;
    }
    std::string out;
    out.reserve(end - begin - 2);
    for (std::size_t i = begin + 1; i + 1 < end; ++i) {
        if (s[i] != '\\') {
            out.push_back(s[i]);
            continue;
        }
        ++i;
        if (i >= end - 1) {
            return std::nullopt;
        }
        switch (s[i]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: return std::nullopt;
        }
    }
    return out;
}

void skipJsonWhitespace(const std::string& s, std::size_t& pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])) != 0) {
        ++pos;
    }
}

bool skipJsonValue(const std::string& s, std::size_t& pos) {
    skipJsonWhitespace(s, pos);
    if (pos >= s.size()) {
        return false;
    }
    if (s[pos] == '"') {
        return skipJsonStringToken(s, pos);
    }
    if (s[pos] == '{' || s[pos] == '[') {
        const char open = s[pos];
        const char close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (pos < s.size()) {
            if (s[pos] == '"') {
                if (!skipJsonStringToken(s, pos)) {
                    return false;
                }
                continue;
            }
            if (s[pos] == open) {
                ++depth;
            } else if (s[pos] == close) {
                --depth;
                ++pos;
                if (depth == 0) {
                    return true;
                }
                continue;
            }
            ++pos;
        }
        return false;
    }
    while (pos < s.size()) {
        const char ch = s[pos];
        if (ch == ',' || ch == '}' || ch == ']' || std::isspace(static_cast<unsigned char>(ch)) != 0) {
            break;
        }
        ++pos;
    }
    return true;
}

bool findJsonValueSpan(const std::string& s,
                       const std::string& key,
                       std::size_t& value_begin,
                       std::size_t& value_end) {
    std::size_t pos = 0;
    skipJsonWhitespace(s, pos);
    if (pos >= s.size() || s[pos] != '{') {
        return false;
    }
    ++pos;
    while (pos < s.size()) {
        skipJsonWhitespace(s, pos);
        if (pos >= s.size()) {
            return false;
        }
        if (s[pos] == '}') {
            return false;
        }
        const std::size_t key_begin = pos;
        if (!skipJsonStringToken(s, pos)) {
            return false;
        }
        const std::size_t key_end = pos;
        const auto parsed_key = decodeJsonStringToken(s, key_begin, key_end);
        if (!parsed_key.has_value()) {
            return false;
        }
        skipJsonWhitespace(s, pos);
        if (pos >= s.size() || s[pos] != ':') {
            return false;
        }
        ++pos;
        skipJsonWhitespace(s, pos);
        value_begin = pos;
        if (!skipJsonValue(s, pos)) {
            return false;
        }
        value_end = pos;
        if (*parsed_key == key) {
            return true;
        }
        skipJsonWhitespace(s, pos);
        if (pos < s.size() && s[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < s.size() && s[pos] == '}') {
            return false;
        }
    }
    return false;
}

std::optional<std::string> jsonGetString(const std::string& s, const std::string& key) {
    std::size_t value_begin = 0;
    std::size_t value_end = 0;
    if (!findJsonValueSpan(s, key, value_begin, value_end)) {
        return std::nullopt;
    }
    if (value_begin >= value_end || s[value_begin] != '"') {
        return std::nullopt;
    }
    return decodeJsonStringToken(s, value_begin, value_end);
}

std::optional<long long> jsonGetInt(const std::string& s, const std::string& key) {
    std::size_t value_begin = 0;
    std::size_t value_end = 0;
    if (!findJsonValueSpan(s, key, value_begin, value_end)) {
        return std::nullopt;
    }
    try {
        std::size_t pos = 0;
        const auto value = std::stoll(s.substr(value_begin, value_end - value_begin), &pos);
        if (pos != (value_end - value_begin)) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> jsonGetDouble(const std::string& s, const std::string& key) {
    std::size_t value_begin = 0;
    std::size_t value_end = 0;
    if (!findJsonValueSpan(s, key, value_begin, value_end)) {
        return std::nullopt;
    }
    try {
        std::size_t pos = 0;
        const auto value = std::stod(s.substr(value_begin, value_end - value_begin), &pos);
        if (pos != (value_end - value_begin)) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> jsonGetBool(const std::string& s, const std::string& key) {
    std::size_t value_begin = 0;
    std::size_t value_end = 0;
    if (!findJsonValueSpan(s, key, value_begin, value_end)) {
        return std::nullopt;
    }
    const std::string value = s.substr(value_begin, value_end - value_begin);
    if (value == "true" || value == "1") {
        return true;
    }
    if (value == "false" || value == "0") {
        return false;
    }
    return std::nullopt;
}

std::optional<std::string> jsonGetObject(const std::string& s, const std::string& key) {
    std::size_t value_begin = 0;
    std::size_t value_end = 0;
    if (!findJsonValueSpan(s, key, value_begin, value_end)) {
        return std::nullopt;
    }
    if (value_begin >= value_end || s[value_begin] != '{') {
        return std::nullopt;
    }
    return s.substr(value_begin, value_end - value_begin);
}

std::optional<std::string> jsonGetStringFromScope(const std::string& scope, const std::string& key) {
    const auto v = jsonGetString(scope, key);
    if (v.has_value()) return v;
    return std::nullopt;
}

std::optional<long long> jsonGetIntFromScope(const std::string& scope, const std::string& key) {
    return jsonGetInt(scope, key);
}

std::optional<double> jsonGetDoubleFromScope(const std::string& scope, const std::string& key) {
    return jsonGetDouble(scope, key);
}

std::optional<bool> jsonGetBoolFromScope(const std::string& scope, const std::string& key) {
    return jsonGetBool(scope, key);
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// 当前只支持简单 query 参数解析，未做完整 URL decode（如 %20、%26 等需调用方自行处理）
std::string queryParam(const std::string& full_path, const std::string& key) {
    const auto q = full_path.find('?');
    if (q == std::string::npos) return "";
    std::size_t pos = q + 1;
    while (pos < full_path.size()) {
        const auto amp = full_path.find('&', pos);
        const auto eq = full_path.find('=', pos);
        if (eq == std::string::npos || (amp != std::string::npos && eq > amp)) {
            pos = (amp == std::string::npos) ? full_path.size() : amp + 1;
            continue;
        }
        if (full_path.compare(pos, eq - pos, key) == 0) {
            const auto val_end = (amp == std::string::npos) ? full_path.size() : amp;
            return full_path.substr(eq + 1, val_end - eq - 1);
        }
        pos = (amp == std::string::npos) ? full_path.size() : amp + 1;
    }
    return "";
}

// 统一参数规范化函数：非数字/空/负数 → 默认值，再钳制到 [min_val, max_val]
// 负数返回默认值确保不会传给 SQLite
int normalizeQueryParam(const std::string& full_path, const std::string& key, int default_val, int min_val, int max_val) {
    const std::string s = queryParam(full_path, key);
    if (s.empty()) return default_val;
    try {
        int val = std::stoi(s);
        if (val < 0) return default_val;
        if (val < min_val) return min_val;
        if (val > max_val) return max_val;
        return val;
    } catch (...) {
        return default_val;
    }
}

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '/') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string pathWithoutQuery(const std::string& path) {
    const auto q = path.find('?');
    if (q == std::string::npos) return path;
    return path.substr(0, q);
}

std::string httpBody(const std::string& req) {
    const std::string sep = "\r\n\r\n";
    const auto p = req.find(sep);
    if (p == std::string::npos) return "";
    return req.substr(p + sep.size());
}

std::string toLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

constexpr std::size_t kHttpHeaderSizeLimit = 16 * 1024;
constexpr std::size_t kHttpBodySizeLimit = 128 * 1024;

std::optional<std::string> findHttpHeaderValue(const std::string& req,
                                               std::size_t header_end,
                                               const std::string& name) {
    std::size_t line_begin = 0;
    while (line_begin < header_end) {
        const std::size_t line_end = req.find("\r\n", line_begin);
        if (line_end == std::string::npos || line_end > header_end) {
            break;
        }
        const std::string line = req.substr(line_begin, line_end - line_begin);
        line_begin = line_end + 2;
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        if (toLowerAscii(trim(line.substr(0, colon))) == name) {
            return trim(line.substr(colon + 1));
        }
    }
    return std::nullopt;
}

bool parseHttpContentLength(const std::string& req,
                            std::size_t header_end,
                            std::size_t& content_length,
                            std::string& err) {
    content_length = 0;
    const auto transfer_encoding = findHttpHeaderValue(req, header_end, "transfer-encoding");
    if (transfer_encoding.has_value() && toLowerAscii(*transfer_encoding) != "identity") {
        err = "unsupported Transfer-Encoding";
        return false;
    }
    const auto value = findHttpHeaderValue(req, header_end, "content-length");
    if (value.has_value()) {
        if (value->empty()) {
            err = "empty Content-Length";
            return false;
        }
        try {
            std::size_t pos = 0;
            const unsigned long long parsed = std::stoull(*value, &pos);
            if (pos != value->size()) {
                err = "invalid Content-Length";
                return false;
            }
            content_length = static_cast<std::size_t>(parsed);
            if (content_length > kHttpBodySizeLimit) {
                err = "http body too large";
                return false;
            }
            return true;
        } catch (...) {
            err = "invalid Content-Length";
            return false;
        }
    }
    return true;
}

bool readHttpRequest(int fd,
                     std::string& req,
                     const std::atomic<bool>& running,
                     std::string& err) {
    req.clear();
    std::array<char, 2048> buf{};
    std::size_t header_end = std::string::npos;
    std::size_t expected_size = std::string::npos;
    int idle_loops = 0;

    while (running.load() && idle_loops < 500) {
        const ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n > 0) {
            idle_loops = 0;
            req.append(buf.data(), static_cast<std::size_t>(n));
            if (header_end == std::string::npos && req.size() > kHttpHeaderSizeLimit) {
                err = "http headers too large";
                return false;
            }
            if (header_end == std::string::npos) {
                const auto p = req.find("\r\n\r\n");
                if (p != std::string::npos) {
                    header_end = p + 4;
                    std::size_t content_length = 0;
                    if (!parseHttpContentLength(req, p, content_length, err)) {
                        return false;
                    }
                    expected_size = header_end + content_length;
                    if (expected_size > (kHttpHeaderSizeLimit + kHttpBodySizeLimit)) {
                        err = "http request too large";
                        return false;
                    }
                }
            }
            if (header_end != std::string::npos && req.size() >= expected_size) {
                return true;
            }
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            ++idle_loops;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        err = "http read failed: " + std::string(std::strerror(errno));
        return false;
    }

    if (req.empty()) {
        return true;
    }
    if (header_end == std::string::npos) {
        err = "incomplete http headers";
        return false;
    }
    if (req.size() < expected_size) {
        err = "incomplete http body expected=" + std::to_string(expected_size - header_end) +
              " actual=" + std::to_string(req.size() - header_end);
        return false;
    }
    return true;
}

bool isCommandSupportedByTransport(const ControlCommandRequest& req,
                                   const std::string& transport,
                                   std::string& err) {
    if (transport == "serial" || transport == "tcp_binary") {
        if (req.command_type == "set_led" ||
            req.command_type == "set_mode" ||
            req.command_type == "get_status" ||
            req.command_type == "set_threshold") {
            return true;
        }
    } else if (transport == "wifi") {
        if (req.command_type == "set_led" ||
            req.command_type == "set_mode" ||
            req.command_type == "get_status" ||
            req.command_type == "set_threshold" ||
            req.command_type == "set_report_interval" ||
            req.command_type == "set_log_level") {
            return true;
        }
    } else if (transport == "mqtt") {
        if (req.command_type == "set_led" ||
            req.command_type == "get_status" ||
            req.command_type == "set_report_interval" ||
            req.command_type == "set_log_level") {
            return true;
        }
    } else {
        err = "unknown transport: " + transport;
        return false;
    }
    err = "command_type " + req.command_type + " not supported on transport " + transport;
    return false;
}

std::string extractVersionFromFirmwareId(const std::string& firmware_id) {
    const auto p = firmware_id.rfind('-');
    if (p == std::string::npos || p + 1 >= firmware_id.size()) return firmware_id;
    return firmware_id.substr(p + 1);
}

std::vector<std::uint8_t> readBinaryFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct OtaRuntimeMetricsData {
    std::mutex mutex;
    std::uint64_t bytes_sent_total = 0;
    std::uint64_t retry_total = 0;
    std::uint64_t crc_error_total = 0;
    std::uint64_t duration_count = 0;
    double duration_total_sec = 0.0;
    std::unordered_map<std::string, std::uint64_t> task_start_ms;
};

OtaRuntimeMetricsData g_ota_rt_metrics;
std::mutex g_ota_cancel_mutex;
std::unordered_map<std::string, bool> g_ota_cancel_flags;
std::mutex g_esp32_ota_status_mutex;
std::condition_variable g_esp32_ota_status_cv;

struct Esp32OtaStatusSlot {
    ota::Esp32OtaStatus last_status{};
    bool has_status = false;
};

std::unordered_map<std::uint64_t, Esp32OtaStatusSlot> g_esp32_ota_status_map;
std::atomic<int> g_ota_active_tasks{0};
std::atomic<std::uint64_t> g_ota_rejected_tasks{0};

struct ControlCommandResult {
    bool ok = false;
    std::uint8_t device_id = 0;
    std::uint16_t command_id = 0;
    std::string command_type;
    std::string status;
    int latency_ms = 0;
    std::string error;
};

struct ControlCommandLogEntry {
    std::uint64_t ts_unix_ms = 0;
    std::string time_text;
    std::uint8_t device_id = 0;
    std::uint16_t command_id = 0;
    std::string command_type;
    std::string status;
    int latency_ms = 0;
    std::string error;
};

std::mutex g_control_command_log_mutex;
std::deque<ControlCommandLogEntry> g_control_command_logs;

std::string formatLocalTime(std::uint64_t unix_ms) {
    const std::time_t sec = static_cast<std::time_t>(unix_ms / 1000ULL);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &sec);
#else
    localtime_r(&sec, &tm);
#endif
    char buf[32] = {0};
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm) == 0) {
        return "-";
    }
    return buf;
}

void pushControlCommandLog(const ControlCommandResult& result) {
    ControlCommandLogEntry entry;
    entry.ts_unix_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    entry.time_text = formatLocalTime(entry.ts_unix_ms);
    entry.device_id = result.device_id;
    entry.command_id = result.command_id;
    entry.command_type = result.command_type;
    entry.status = result.status;
    entry.latency_ms = result.latency_ms;
    entry.error = result.error;

    std::lock_guard<std::mutex> lock(g_control_command_log_mutex);
    g_control_command_logs.push_back(std::move(entry));
    while (g_control_command_logs.size() > 100U) {
        g_control_command_logs.pop_front();
    }
}
constexpr int kMaxConcurrentOtaTasks = 8;
std::mutex g_ota_device_mode_mutex;
std::unordered_set<std::uint8_t> g_ota_mode_devices;
std::mutex g_ota_mute_mutex;
std::unordered_set<std::uint8_t> g_ota_business_muted_devices;
std::unordered_map<std::uint8_t, std::string> g_ota_transport_locked_devices;
std::mutex g_tcp_ota_frame_mutex;
std::condition_variable g_tcp_ota_frame_cv;
std::unordered_map<std::uint8_t, std::deque<std::vector<std::uint8_t>>> g_tcp_ota_frames;

constexpr std::uint8_t kTypeAdapterControl = 0x7DU;
constexpr std::uint8_t kAdapterControlEnterOta = 0x01U;
constexpr std::uint8_t kAdapterControlLeaveOta = 0x02U;

void otaDeviceEnter(std::uint8_t device_id) {
    std::lock_guard<std::mutex> lock(g_ota_device_mode_mutex);
    g_ota_mode_devices.insert(device_id);
}

void otaDeviceLeave(std::uint8_t device_id) {
    std::lock_guard<std::mutex> lock(g_ota_device_mode_mutex);
    g_ota_mode_devices.erase(device_id);
}

bool otaDeviceInProgress(std::uint8_t device_id) {
    std::lock_guard<std::mutex> lock(g_ota_device_mode_mutex);
    return g_ota_mode_devices.find(device_id) != g_ota_mode_devices.end();
}

void otaMuteBusiness(std::uint8_t device_id) {
    std::lock_guard<std::mutex> lock(g_ota_mute_mutex);
    g_ota_business_muted_devices.insert(device_id);
    Logger::instance().info("[OTA][MUTE] business muted device=" + std::to_string(static_cast<int>(device_id)));
}

void otaUnmuteBusiness(std::uint8_t device_id) {
    std::lock_guard<std::mutex> lock(g_ota_mute_mutex);
    g_ota_business_muted_devices.erase(device_id);
    Logger::instance().info("[OTA][MUTE] business unmuted device=" + std::to_string(static_cast<int>(device_id)));
}

bool otaBusinessMuted(std::uint8_t device_id) {
    std::lock_guard<std::mutex> lock(g_ota_mute_mutex);
    return g_ota_business_muted_devices.find(device_id) != g_ota_business_muted_devices.end();
}

void otaLockTransport(std::uint8_t device_id, const std::string& transport) {
    std::lock_guard<std::mutex> lock(g_ota_mute_mutex);
    g_ota_transport_locked_devices[device_id] = transport;
    Logger::instance().info("[OTA][LOCK] transport locked device=" +
                            std::to_string(static_cast<int>(device_id)) +
                            " transport=" + transport);
}

void otaUnlockTransport(std::uint8_t device_id) {
    std::lock_guard<std::mutex> lock(g_ota_mute_mutex);
    g_ota_transport_locked_devices.erase(device_id);
    Logger::instance().info("[OTA][LOCK] transport unlocked device=" +
                            std::to_string(static_cast<int>(device_id)));
}

bool otaTransportLocked(std::uint8_t device_id, std::string* locked_transport = nullptr) {
    std::lock_guard<std::mutex> lock(g_ota_mute_mutex);
    const auto it = g_ota_transport_locked_devices.find(device_id);
    if (it == g_ota_transport_locked_devices.end()) {
        return false;
    }
    if (locked_transport) *locked_transport = it->second;
    return true;
}

bool isBootloaderUplinkType(std::uint8_t type) {
    return type == 0x20U || type == 0x21U || type == 0x22U || type == 0x23U ||
           type == 0x24U || type == 0x25U || type == 0x26U || type == 0x27U;
}

std::vector<std::uint8_t> buildAdapterControlFrame(std::uint8_t command,
                                                   std::uint32_t session_id,
                                                   std::uint32_t timeout_ms) {
    std::vector<std::uint8_t> payload;
    payload.reserve(1 + 4 + 4);
    payload.push_back(command);
    payload.push_back(static_cast<std::uint8_t>(session_id & 0xFFU));
    payload.push_back(static_cast<std::uint8_t>((session_id >> 8U) & 0xFFU));
    payload.push_back(static_cast<std::uint8_t>((session_id >> 16U) & 0xFFU));
    payload.push_back(static_cast<std::uint8_t>((session_id >> 24U) & 0xFFU));
    payload.push_back(static_cast<std::uint8_t>(timeout_ms & 0xFFU));
    payload.push_back(static_cast<std::uint8_t>((timeout_ms >> 8U) & 0xFFU));
    payload.push_back(static_cast<std::uint8_t>((timeout_ms >> 16U) & 0xFFU));
    payload.push_back(static_cast<std::uint8_t>((timeout_ms >> 24U) & 0xFFU));

    std::vector<std::uint8_t> frame;
    frame.reserve(payload.size() + 8);
    frame.push_back(0xAAU);
    frame.push_back(0x55U);
    frame.push_back(static_cast<std::uint8_t>(payload.size() + 1U));
    frame.push_back(kTypeAdapterControl);
    frame.insert(frame.end(), payload.begin(), payload.end());
    const auto crc = crc16_modbus(frame.data() + 2, static_cast<std::size_t>(1 + frame[2]));
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xFFU));
    return frame;
}

void publishTcpBinaryOtaFrame(std::uint8_t device_id, const std::vector<std::uint8_t>& frame) {
    {
        std::lock_guard<std::mutex> lock(g_tcp_ota_frame_mutex);
        auto& q = g_tcp_ota_frames[device_id];
        q.push_back(frame);
        while (q.size() > 64U) q.pop_front();
    }
    g_tcp_ota_frame_cv.notify_all();
}

void clearTcpBinaryOtaFrames(std::uint8_t device_id) {
    std::lock_guard<std::mutex> lock(g_tcp_ota_frame_mutex);
    g_tcp_ota_frames.erase(device_id);
}

bool waitTcpBinaryOtaFrame(std::uint8_t device_id,
                           int timeout_ms,
                           std::vector<std::uint8_t>& out,
                           const std::function<bool()>& should_stop) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(g_tcp_ota_frame_mutex);
    while (std::chrono::steady_clock::now() < deadline) {
        if (should_stop && should_stop()) return false;
        auto it = g_tcp_ota_frames.find(device_id);
        if (it != g_tcp_ota_frames.end() && !it->second.empty()) {
            out = std::move(it->second.front());
            it->second.pop_front();
            return true;
        }
        g_tcp_ota_frame_cv.wait_for(lock, std::chrono::milliseconds(100));
    }
    return false;
}

std::uint64_t makeEsp32OtaStatusKey(std::uint8_t device_id, std::uint32_t command_id) {
    return (static_cast<std::uint64_t>(device_id) << 32U) | static_cast<std::uint64_t>(command_id);
}

void publishEsp32OtaStatus(const ota::Esp32OtaStatus& st) {
    {
        std::lock_guard<std::mutex> lock(g_esp32_ota_status_mutex);
        auto& slot = g_esp32_ota_status_map[makeEsp32OtaStatusKey(st.device_id, st.command_id)];
        slot.last_status = st;
        slot.has_status = true;
    }
    g_esp32_ota_status_cv.notify_all();
}

bool waitEsp32OtaStatus(std::uint8_t device_id,
                        std::uint32_t command_id,
                        int timeout_ms,
                        ota::Esp32OtaStatus& out,
                        const std::function<bool()>& should_stop) {
    const auto key = makeEsp32OtaStatusKey(device_id, command_id);
    auto hasTerminal = [&](const ota::Esp32OtaStatus& st) {
        return st.ota_state == "success" || st.ota_state == "failed";
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(g_esp32_ota_status_mutex);
    while (std::chrono::steady_clock::now() < deadline) {
        if (should_stop && should_stop()) {
            g_esp32_ota_status_map.erase(key);
            return false;
        }
        const auto it = g_esp32_ota_status_map.find(key);
        if (it != g_esp32_ota_status_map.end() && it->second.has_status) {
            out = it->second.last_status;
            if (hasTerminal(out)) {
                g_esp32_ota_status_map.erase(it);
                return true;
            }
        }
        g_esp32_ota_status_cv.wait_for(lock, std::chrono::milliseconds(200));
    }
    g_esp32_ota_status_map.erase(key);
    return false;
}

void clearEsp32OtaStatus(std::uint8_t device_id, std::uint32_t command_id) {
    std::lock_guard<std::mutex> lock(g_esp32_ota_status_mutex);
    g_esp32_ota_status_map.erase(makeEsp32OtaStatusKey(device_id, command_id));
}

std::uint64_t unixMsNowGlobal() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void otaMetricsMarkStart(const std::string& task_uuid) {
    std::lock_guard<std::mutex> lock(g_ota_rt_metrics.mutex);
    g_ota_rt_metrics.task_start_ms[task_uuid] = unixMsNowGlobal();
}

void otaMetricsMarkSuccess(const std::string& task_uuid, std::uint64_t bytes_sent) {
    std::lock_guard<std::mutex> lock(g_ota_rt_metrics.mutex);
    g_ota_rt_metrics.bytes_sent_total += bytes_sent;
    auto it = g_ota_rt_metrics.task_start_ms.find(task_uuid);
    if (it != g_ota_rt_metrics.task_start_ms.end()) {
        const std::uint64_t now = unixMsNowGlobal();
        if (now >= it->second) {
            g_ota_rt_metrics.duration_total_sec += static_cast<double>(now - it->second) / 1000.0;
            g_ota_rt_metrics.duration_count++;
        }
        g_ota_rt_metrics.task_start_ms.erase(it);
    }
}

void otaMetricsMarkFail(const std::string& task_uuid, const std::string& reason) {
    std::lock_guard<std::mutex> lock(g_ota_rt_metrics.mutex);
    if (reason.find("crc") != std::string::npos || reason.find("CRC") != std::string::npos) {
        g_ota_rt_metrics.crc_error_total++;
    }
    g_ota_rt_metrics.task_start_ms.erase(task_uuid);
}

void otaMetricsMarkRetry() {
    std::lock_guard<std::mutex> lock(g_ota_rt_metrics.mutex);
    g_ota_rt_metrics.retry_total++;
}

void otaCancelSet(const std::string& task_uuid, bool v) {
    std::lock_guard<std::mutex> lock(g_ota_cancel_mutex);
    g_ota_cancel_flags[task_uuid] = v;
}

bool otaCancelRequested(const std::string& task_uuid) {
    std::lock_guard<std::mutex> lock(g_ota_cancel_mutex);
    auto it = g_ota_cancel_flags.find(task_uuid);
    return it != g_ota_cancel_flags.end() && it->second;
}

void otaCancelClear(const std::string& task_uuid) {
    std::lock_guard<std::mutex> lock(g_ota_cancel_mutex);
    g_ota_cancel_flags.erase(task_uuid);
}

void executeOtaTask(std::string task_uuid,
                    std::string firmware_id,
                    std::string device_type,
                    std::string transport,
                    std::string target,
                    std::uint8_t device_id,
                    std::string firmware_url,
                    std::function<bool(std::uint8_t, const std::string&)> wifi_sender,
                    std::function<bool(std::uint8_t, const std::string&, std::string&)> mqtt_sender,
                    std::function<bool(std::uint8_t, const std::vector<std::uint8_t>&)> tcp_frame_sender) {
    if (g_ota_active_tasks.fetch_add(1) >= kMaxConcurrentOtaTasks) {
        g_ota_active_tasks.fetch_sub(1);
        g_ota_rejected_tasks.fetch_add(1);
        storage::PersistentStore store(otaTaskStorePath());
        ota::OtaTaskManager mgr(store);
        std::string err;
        (void)mgr.fail(task_uuid, "too many concurrent ota tasks", err);
        otaMetricsMarkFail(task_uuid, "too many concurrent ota tasks");
        otaCancelClear(task_uuid);
        return;
    }
    struct OtaTaskCounterGuard {
        ~OtaTaskCounterGuard() { g_ota_active_tasks.fetch_sub(1); }
    } counter_guard;
        storage::PersistentStore store(otaTaskStorePath());
        ota::OtaTaskManager mgr(store);
        std::string err;
        otaMetricsMarkStart(task_uuid);
        otaCancelSet(task_uuid, false);
        otaDeviceEnter(device_id);
        struct OtaModeGuard {
            explicit OtaModeGuard(std::uint8_t id) : device_id(id) {}
            ~OtaModeGuard() { otaDeviceLeave(device_id); }
            std::uint8_t device_id;
        } ota_mode_guard(device_id);
        struct OtaMuteLockGuard {
            explicit OtaMuteLockGuard(std::uint8_t id) : device_id(id) {}
            ~OtaMuteLockGuard() {
                otaUnlockTransport(device_id);
                otaUnmuteBusiness(device_id);
            }
            std::uint8_t device_id;
        } ota_mute_lock_guard(device_id);
        auto canceled = [&]() { return otaCancelRequested(task_uuid); };
        auto onCanceled = [&]() {
            std::string ce;
            Logger::instance().warn("[OTA] task canceled task_uuid=" + task_uuid +
                                    " device_id=" + std::to_string(static_cast<int>(device_id)));
            mgr.cancel(task_uuid, ce);
            otaMetricsMarkFail(task_uuid, "canceled");
            otaCancelClear(task_uuid);
        };

        if (canceled()) { onCanceled(); return; }

        mgr.updateState(task_uuid, ota::OtaTaskState::WAIT_DEVICE_ONLINE, "device online", err);
        if (canceled()) { onCanceled(); return; }
        mgr.updateState(task_uuid, ota::OtaTaskState::PRECHECK, "precheck ok", err);
        if (canceled()) { onCanceled(); return; }

        ota::FirmwareStore fw_store(gatewayRuntimePath("data/firmware"));
        auto fw = fw_store.get(firmware_id);
        if (!fw) {
            mgr.fail(task_uuid, "firmware not found", err);
            otaMetricsMarkFail(task_uuid, "firmware not found");
            otaCancelClear(task_uuid);
            return;
        }

        std::string host = "127.0.0.1";
        int port = 19090;
        const auto p = target.find(':');
        if (p != std::string::npos) {
            host = target.substr(0, p);
            try {
                port = std::stoi(target.substr(p + 1));
            } catch (...) {
                port = 19090;
            }
        }

        mgr.updateState(task_uuid, ota::OtaTaskState::PREPARE_DEVICE, "prepare ota", err);
        if (canceled()) { onCanceled(); return; }
        mgr.updateState(task_uuid, ota::OtaTaskState::TRANSFERRING, "start transfer", err);
        if (canceled()) { onCanceled(); return; }

        bool ok = false;
        std::uint64_t sent_bytes = 0;
        if (device_type.find("stm32") != std::string::npos) {
            const std::string transport_mode =
                (transport == "tcp_binary" || transport == "serial") ? transport : "serial";
            Logger::instance().info("[OTA] device=" + std::to_string(static_cast<int>(device_id)) +
                                    " transport=" + transport_mode +
                                    " target=" + target);
            otaMuteBusiness(device_id);
            otaLockTransport(device_id, transport_mode);
            auto image = readBinaryFile(fw->image_path);
            if (image.empty()) {
                mgr.fail(task_uuid, "firmware image is empty", err);
                otaMetricsMarkFail(task_uuid, "firmware image is empty");
                otaCancelClear(task_uuid);
                return;
            }
            std::string crc_err;
            const std::uint32_t image_crc = ota::FirmwareStore::crc32File(fw->image_path, crc_err);
            if (!crc_err.empty()) {
                mgr.fail(task_uuid, crc_err, err);
                otaMetricsMarkFail(task_uuid, crc_err);
                otaCancelClear(task_uuid);
                return;
            }

            ota::Stm32OtaAdapter adapter;
            ota::Stm32OtaOptions opt;
            opt.host = host;
            opt.port = port;
            opt.chunk_size = 192;
            opt.timeout_ms = 45000;
            opt.max_retries = 5;
            opt.wait_boot_hello = true;
            opt.hello_timeout_ms = 60000;
            opt.prepare_ack_timeout_ms = 45000;
            opt.data_ack_timeout_ms = 30000;
            opt.verify_ack_timeout_ms = 8000;
            opt.commit_ack_timeout_ms = 8000;
            opt.inter_chunk_delay_ms = 35;
            if (!fw->manifest.entry_addr.empty()) {
                try {
                    opt.target_base = static_cast<std::uint32_t>(std::stoul(fw->manifest.entry_addr, nullptr, 0));
                } catch (...) {
                    opt.target_base = 0U;
                }
            }
            opt.image_version = fw->manifest.image_version;
            std::uint32_t session_id = static_cast<std::uint32_t>(unixMsNowGlobal() & 0xFFFFFFFFULL);
            constexpr std::uint32_t kOtaModeTimeoutMs = 600000U;
            struct OtaControlGuard {
                std::uint8_t device_id = 0;
                std::uint32_t session_id = 0;
                std::function<bool(std::uint8_t, const std::vector<std::uint8_t>&)> sender;
                bool armed = false;
                std::string leave_reason = "fail";
                ~OtaControlGuard() {
                    if (!armed || !sender) return;
                    Logger::instance().info("[OTA][CTRL] leave ota mode reason=" + leave_reason +
                                            " session=" + std::to_string(session_id));
                    (void)sender(device_id, buildAdapterControlFrame(kAdapterControlLeaveOta, session_id, 0));
                }
            } control_guard;
            if (transport_mode == "tcp_binary") {
                if (!tcp_frame_sender) {
                    mgr.fail(task_uuid, "tcp_binary sender unavailable", err);
                    otaMetricsMarkFail(task_uuid, "tcp_binary sender unavailable");
                    otaCancelClear(task_uuid);
                    return;
                }
                control_guard.device_id = device_id;
                control_guard.session_id = session_id;
                control_guard.sender = tcp_frame_sender;
                control_guard.armed = true;
                opt.send_raw_frame = [device_id, tcp_frame_sender](const std::vector<std::uint8_t>& frame, std::string& out_err) {
                    if (!tcp_frame_sender || !tcp_frame_sender(device_id, frame)) {
                        out_err = "tcp_binary send failed";
                        return false;
                    }
                    return true;
                };
                opt.recv_raw_frame =
                    [device_id, canceled](std::vector<std::uint8_t>& frame, int timeout_ms, std::string& out_err) {
                        if (!waitTcpBinaryOtaFrame(device_id, timeout_ms, frame, canceled)) {
                            out_err = canceled() ? "ota canceled" : "ack timeout";
                            return false;
                        }
                        if (frame.size() >= 8U && frame[3] == 0x23U) {
                            const std::uint16_t seq =
                                static_cast<std::uint16_t>(frame[4]) |
                                (static_cast<std::uint16_t>(frame[5]) << 8U);
                            std::uint16_t err_code = 0U;
                            if (frame.size() >= 10U) {
                                err_code = static_cast<std::uint16_t>(frame[6]) |
                                           (static_cast<std::uint16_t>(frame[7]) << 8U);
                            }
                            Logger::instance().warn("[OTA][RX] type=0x23 seq=" + std::to_string(seq) +
                                                    " err=" + std::to_string(err_code));
                        }
                        return true;
                    };
            }
            if (transport_mode == "tcp_binary") {
                const int hello_retry_max = 3;
                const int hello_wait_ms = 60000;
                const int hello_reenter_interval_ms = 5000;
                const int hello_reenter_max = 3;

                Logger::instance().info("[OTA][HELLO] on_enter WAIT_BOOT_HELLO retry=1/" +
                                        std::to_string(hello_retry_max));
                clearTcpBinaryOtaFrames(device_id);
                if (!tcp_frame_sender(device_id,
                                      buildAdapterControlFrame(kAdapterControlEnterOta, session_id, kOtaModeTimeoutMs))) {
                    err = "failed to enter adapter ota mode";
                    ok = false;
                } else {
                    bool got_hello = false;
                    int reenter_count = 0;
                    int elapsed_ms = 0;
                    while (elapsed_ms < hello_wait_ms && !canceled()) {
                        std::vector<std::uint8_t> rx_frame;
                        if (waitTcpBinaryOtaFrame(device_id, 500, rx_frame, canceled)) {
                            if (rx_frame.size() >= 4U) {
                                const std::uint8_t rx_type = rx_frame[3];
                                char rx_type_hex[5]{};
                                std::snprintf(rx_type_hex, sizeof(rx_type_hex), "%02X",
                                              static_cast<unsigned int>(rx_type));
                                if (rx_type == 0x20U) {
                                    Logger::instance().info("[OTA][HELLO] on_rx type=0x" +
                                                            std::string(rx_type_hex) +
                                                            " len=" + std::to_string(rx_frame.size()) +
                                                            " accepted=1");
                                    got_hello = true;
                                    break;
                                }
                                if (rx_type == 0x01U || rx_type == 0x03U ||
                                    rx_type == 0x06U || rx_type == 0x26U) {
                                    Logger::instance().info("[OTA][HELLO] on_rx type=0x" +
                                                            std::string(rx_type_hex) +
                                                            " len=" + std::to_string(rx_frame.size()) +
                                                            " ignored=1");
                                } else {
                                    Logger::instance().info("[OTA][HELLO] on_rx type=0x" +
                                                            std::string(rx_type_hex) +
                                                            " len=" + std::to_string(rx_frame.size()) +
                                                            " ignored=1");
                                }
                            }
                        }
                        elapsed_ms += 500;
                        if ((elapsed_ms % hello_reenter_interval_ms) == 0 && reenter_count < hello_reenter_max) {
                            ++reenter_count;
                            Logger::instance().warn("[OTA][HELLO] on_timeout WAIT_BOOT_HELLO retry=" +
                                                    std::to_string(reenter_count) + "/" +
                                                    std::to_string(hello_reenter_max));
                            (void)tcp_frame_sender(device_id,
                                                   buildAdapterControlFrame(kAdapterControlEnterOta,
                                                                            session_id,
                                                                            kOtaModeTimeoutMs));
                        }
                    }
                    if (!got_hello) {
                        err = canceled() ? "ota canceled" : "boot hello timeout";
                        Logger::instance().warn("[OTA][HELLO] retry exhausted, fail+leave");
                        ok = false;
                    } else {
                        std::atomic<bool> keepalive_run{true};
                        std::thread keepalive_thread([&, device_id, session_id]() {
                            while (keepalive_run.load()) {
                                std::this_thread::sleep_for(std::chrono::seconds(120));
                                if (!keepalive_run.load() || canceled()) break;
                                (void)tcp_frame_sender(device_id,
                                                       buildAdapterControlFrame(kAdapterControlEnterOta,
                                                                                session_id,
                                                                                kOtaModeTimeoutMs));
                                Logger::instance().info("[OTA][CTRL] renew ota mode session=" +
                                                        std::to_string(session_id));
                            }
                        });
                        opt.wait_boot_hello = false;
                        ok = adapter.run(image, image_crc, opt, err, [&](int pct) {
                            std::string ie;
                            mgr.updateState(task_uuid, ota::OtaTaskState::TRANSFERRING,
                                            "progress=" + std::to_string(pct), ie);
                        }, canceled);
                        keepalive_run.store(false);
                        if (keepalive_thread.joinable()) keepalive_thread.join();
                    }
                }
            } else {
                ok = adapter.run(image, image_crc, opt, err, [&](int pct) {
                    std::string ie;
                    mgr.updateState(task_uuid, ota::OtaTaskState::TRANSFERRING, "progress=" + std::to_string(pct), ie);
                }, canceled);
            }
            sent_bytes = image.size();
            if (ok) {
                control_guard.leave_reason = "success";
            } else if (canceled()) {
                control_guard.leave_reason = "user_cancel";
            } else {
                control_guard.leave_reason = "fail";
                if (err == "boot hello timeout") {
                    Logger::instance().warn("[OTA][HELLO] retry exhausted, fail+leave");
                }
            }
            clearTcpBinaryOtaFrames(device_id);
        } else if (device_type.find("esp32") != std::string::npos) {
            if (transport == "mqtt" && !mqtt_sender) {
                mgr.fail(task_uuid, "esp32 mqtt sender unavailable", err);
                otaMetricsMarkFail(task_uuid, "esp32 mqtt sender unavailable");
                otaCancelClear(task_uuid);
                return;
            }
            if (transport != "mqtt" && !wifi_sender) {
                mgr.fail(task_uuid, "esp32 sender unavailable", err);
                otaMetricsMarkFail(task_uuid, "esp32 sender unavailable");
                otaCancelClear(task_uuid);
                return;
            }
            ota::Esp32OtaStart req{};
            req.device_id = device_id;
            req.command_id = static_cast<std::uint32_t>(unixMsNowGlobal() & 0x7FFFFFFF);
            req.firmware_url = firmware_url.empty() ? ("http://gateway:9080/fw/" + firmware_id + "/app.bin") : firmware_url;
            req.version = fw->manifest.version;
            req.size = static_cast<std::uint32_t>(fw->manifest.image_size);
            req.crc32 = fw->manifest.crc32;
            req.force = false;
            clearEsp32OtaStatus(req.device_id, req.command_id);
            std::string cmd;
            std::string mqtt_err;
            if (transport == "mqtt") {
                buildMqttOtaStartPayload(req, cmd);
            } else {
                cmd = ota::Esp32OtaAdapter::buildOtaStartJson(req) + "\n";
            }
            Logger::instance().info("esp32 ota start sent device_id=" +
                                    std::to_string(static_cast<int>(req.device_id)) +
                                    " command_id=" + std::to_string(req.command_id) +
                                    " firmware_id=" + firmware_id);
            const bool ota_send_ok = (transport == "mqtt")
                                         ? mqtt_sender(req.device_id, cmd, mqtt_err)
                                         : wifi_sender(req.device_id, cmd);
            if (!ota_send_ok) {
                mgr.fail(task_uuid,
                         transport == "mqtt"
                             ? ("esp32 mqtt send ota_start failed: " + mqtt_err)
                             : "esp32 send ota_start failed",
                         err);
                otaMetricsMarkFail(task_uuid, "esp32 send ota_start failed");
                otaCancelClear(task_uuid);
                return;
            }
            ota::Esp32OtaStatus st{};
            const bool got_terminal = waitEsp32OtaStatus(req.device_id, req.command_id, 120000, st, canceled);
            if (!got_terminal) {
                err = canceled() ? "ota canceled" : "esp32 ota timeout";
                ok = false;
            } else if (st.ota_state == "success") {
                ok = true;
            } else {
                err = "esp32 ota failed error_code=" + std::to_string(st.error_code);
                ok = false;
            }
            std::string ie;
            mgr.updateState(task_uuid, ota::OtaTaskState::TRANSFERRING, "esp32:" + st.ota_state + ":" + std::to_string(st.progress), ie);
            sent_bytes = fw->manifest.image_size;
        } else {
            mgr.fail(task_uuid, "unsupported device_type for auto executor", err);
            otaMetricsMarkFail(task_uuid, "unsupported device_type");
            otaCancelClear(task_uuid);
            return;
        }

        if (canceled()) { onCanceled(); return; }
        if (!ok) {
            if (err == "commit ack timeout" && device_type.find("stm32") != std::string::npos) {
                Logger::instance().warn("[OTA] commit ack timeout, treat as committed success task=" + task_uuid);
                ok = true;
            }
        }
        if (!ok) {
            if (err == "ota canceled") {
                onCanceled();
            } else {
                mgr.fail(task_uuid, err, err);
                otaMetricsMarkFail(task_uuid, err);
                otaCancelClear(task_uuid);
            }
            return;
        }

        mgr.updateState(task_uuid, ota::OtaTaskState::VERIFYING, "verify ok", err);
        if (canceled()) { onCanceled(); return; }
        mgr.updateState(task_uuid, ota::OtaTaskState::COMMITTING, "commit ok", err);
        if (canceled()) { onCanceled(); return; }
        mgr.updateState(task_uuid, ota::OtaTaskState::REBOOTING, "rebooting", err);
        if (canceled()) { onCanceled(); return; }
        mgr.updateState(task_uuid, ota::OtaTaskState::VERSION_CHECK, "version checked", err);
        if (canceled()) { onCanceled(); return; }
        mgr.updateState(task_uuid, ota::OtaTaskState::HEALTH_CONFIRM, "health confirmed", err);
        if (canceled()) { onCanceled(); return; }
        mgr.updateState(task_uuid, ota::OtaTaskState::SUCCESS, "ota success", err);
        otaMetricsMarkSuccess(task_uuid, sent_bytes);
        otaCancelClear(task_uuid);
}

bool parseWifiJsonEvent(const std::string& line, SensorData& out, std::string& err) {
    const auto device_id = jsonGetInt(line, "device_id");
    const auto event_type = jsonGetString(line, "event_type");
    if (!device_id.has_value() || !event_type.has_value()) {
        err = "missing device_id or event_type";
        return false;
    }
    if (*device_id < 0 || *device_id > 255) {
        err = "invalid device_id";
        return false;
    }
    out = SensorData{};
    out.device_id = static_cast<std::uint8_t>(*device_id);
    out.frame_type = FrameType::UNKNOWN;
    if (*event_type == "sensor_data") out.frame_type = FrameType::SENSOR_DATA;
    if (*event_type == "heartbeat") out.frame_type = FrameType::HEARTBEAT;
    if (*event_type == "command_ack") out.frame_type = FrameType::COMMAND_ACK;
    if (*event_type == "status") out.frame_type = FrameType::DEVICE_STATUS;
    if (out.frame_type == FrameType::UNKNOWN) {
        err = "unsupported event_type";
        return false;
    }

    out.timestamp_unix_ms = unixMsNowLocal();
    const auto ts = jsonGetInt(line, "timestamp");
    // Accept only plausible unix-ms timestamps; otherwise fallback to gateway time.
    constexpr long long kMinUnixMs = 1600000000000LL;
    if (ts.has_value() && *ts >= kMinUnixMs) {
        out.timestamp_unix_ms = static_cast<std::uint64_t>(*ts);
    }
    const auto name = jsonGetString(line, "device_name");
    if (name.has_value()) out.device_name = *name;
    const auto lt = jsonGetString(line, "link_type");
    out.link_type = lt.has_value() ? *lt : "wifi";
    out.wifi_connected = true;
    out.wifi_last_seen_ms = out.timestamp_unix_ms;
    const auto rssi = jsonGetInt(line, "wifi_rssi");
    if (rssi.has_value()) out.wifi_rssi = static_cast<int>(*rssi);
    const auto seq = jsonGetInt(line, "seq");
    if (seq.has_value() && *seq >= 0) out.seq = static_cast<std::uint64_t>(*seq);
    const auto mq2_alarm = jsonGetInt(line, "mq2_alarm");
    if (mq2_alarm.has_value()) out.mq2_alarm = static_cast<int>(*mq2_alarm);
    const auto ld2402_presence = jsonGetInt(line, "ld2402_presence");
    if (ld2402_presence.has_value()) out.ld2402_presence = static_cast<int>(*ld2402_presence);
    const auto led_on = jsonGetInt(line, "led_on");
    if (led_on.has_value()) out.led_on = static_cast<int>(*led_on);
    const auto alarm_on = jsonGetInt(line, "alarm_on");
    if (alarm_on.has_value()) out.alarm_on = static_cast<int>(*alarm_on);
    const auto sensor_valid = jsonGetInt(line, "sensor_valid");
    if (sensor_valid.has_value()) out.sensor_valid = static_cast<int>(*sensor_valid);
    const auto auto_mode = jsonGetInt(line, "auto_mode");
    if (auto_mode.has_value()) out.auto_mode = static_cast<int>(*auto_mode);

    if (out.frame_type == FrameType::SENSOR_DATA) {
        const auto t = jsonGetDouble(line, "temperature");
        const auto h = jsonGetDouble(line, "humidity");
        const auto v = jsonGetDouble(line, "voltage");
        const auto l = jsonGetDouble(line, "light");
        const auto ll = jsonGetDouble(line, "light_lux");
        const auto st = jsonGetInt(line, "status");
        if (t.has_value()) out.temperature = *t;
        if (h.has_value()) out.humidity = *h;
        if (v.has_value()) out.voltage = *v;
        if (ll.has_value()) out.light = *ll;
        else if (l.has_value()) out.light = *l;
        if (st.has_value()) out.status = static_cast<std::uint8_t>(*st);
        out.payload_summary = "temp=" + std::to_string(out.temperature) +
                              ",hum=" + std::to_string(out.humidity) +
                              ",voltage=" + std::to_string(out.voltage) +
                              ",light=" + std::to_string(out.light);
    } else if (out.frame_type == FrameType::HEARTBEAT) {
        out.payload_summary = "heartbeat";
    } else if (out.frame_type == FrameType::COMMAND_ACK) {
        const auto cmd_id = jsonGetInt(line, "command_id");
        const auto cmd_result = jsonGetInt(line, "command_result");
        if (!cmd_id.has_value() || !cmd_result.has_value()) {
            err = "missing command_ack fields";
            return false;
        }
        out.command_id = static_cast<std::uint16_t>(*cmd_id);
        out.command_result = static_cast<std::uint8_t>(*cmd_result);
        const auto ps = jsonGetString(line, "payload_summary");
        if (ps.has_value()) {
            out.payload_summary = *ps;
        } else {
            out.payload_summary = "cmd_id=" + std::to_string(out.command_id) +
                                  ",result=" + std::to_string(static_cast<int>(out.command_result));
        }
    } else if (out.frame_type == FrameType::DEVICE_STATUS) {
        const auto t = jsonGetDouble(line, "temperature");
        const auto h = jsonGetDouble(line, "humidity");
        const auto v = jsonGetDouble(line, "voltage");
        const auto st = jsonGetInt(line, "status");
        if (t.has_value()) out.temperature = *t;
        if (h.has_value()) out.humidity = *h;
        if (v.has_value()) out.voltage = *v;
        if (st.has_value()) out.status = static_cast<std::uint8_t>(*st);

        const auto rgb_mode = jsonGetInt(line, "rgb_mode");
        const auto report_interval_ms = jsonGetInt(line, "report_interval_ms");
        const auto gw_connected = jsonGetInt(line, "gateway_connected");
        const auto fw_ver = jsonGetString(line, "firmware_version");

        std::ostringstream oss;
        oss << "status";
        if (fw_ver.has_value()) oss << ",fw=" << *fw_ver;
        if (gw_connected.has_value()) oss << ",gw=" << (*gw_connected != 0 ? "1" : "0");
        if (report_interval_ms.has_value()) oss << ",report_ms=" << *report_interval_ms;
        if (rgb_mode.has_value()) oss << ",rgb_mode=" << *rgb_mode;
        if (st.has_value()) oss << ",status=" << static_cast<int>(out.status);
        out.payload_summary = oss.str();
    }
    return true;
}

bool parseTrackCommand(const std::string& line, TrackCommandRequest& out, std::string& err) {
    out = TrackCommandRequest{};
    std::istringstream iss(line);
    std::string op;
    if (!(iss >> op)) {
        err = "empty command";
        return false;
    }

    if (op == "HEX" || op == "TEXT") {
        out.tracked = false;
        out.use_text = (op == "TEXT");
        std::string payload;
        std::getline(iss, payload);
        payload = trim(payload);
        if (payload.empty()) {
            err = "missing payload";
            return false;
        }
        if (out.use_text) {
            out.payload.assign(payload.begin(), payload.end());
            return true;
        }
        if (!parseHexPayload(payload, out.payload)) {
            err = "invalid HEX";
            return false;
        }
        return true;
    }

    if (op != "TRACK_HEX" && op != "TRACK_TEXT") {
        err = "unknown command";
        return false;
    }

    std::string s_device;
    std::string s_cmd_id;
    std::string cmd_type;
    std::string s_timeout;
    if (!(iss >> s_device >> s_cmd_id >> cmd_type >> s_timeout)) {
        err = "usage: TRACK_HEX <device_id> <command_id> <command_type> <timeout_ms> <payload>";
        return false;
    }

    int parsed = 0;
    if (!parseIntInRange(s_device, 0, 255, parsed)) {
        err = "invalid device_id";
        return false;
    }
    out.device_id = static_cast<std::uint8_t>(parsed);
    if (!parseIntInRange(s_cmd_id, 0, 65535, parsed)) {
        err = "invalid command_id";
        return false;
    }
    out.command_id = static_cast<std::uint16_t>(parsed);
    if (!parseIntInRange(s_timeout, 10, 60000, parsed)) {
        err = "invalid timeout_ms";
        return false;
    }
    out.timeout_ms = parsed;
    out.command_type = cmd_type;
    out.tracked = true;
    out.use_text = (op == "TRACK_TEXT");

    std::string payload;
    std::getline(iss, payload);
    payload = trim(payload);
    if (payload.empty()) {
        err = "missing payload";
        return false;
    }
    if (out.use_text) {
        out.payload.assign(payload.begin(), payload.end());
        return true;
    }
    if (!parseHexPayload(payload, out.payload)) {
        err = "invalid TRACK_HEX payload";
        return false;
    }
    return true;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char ch : s) {
        switch (ch) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    out += '?';
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

std::string buildUpstreamSystemEventJson(const GatewayConfig& config,
                                         const std::string& event_type,
                                         int device_id,
                                         const std::string& detail) {
    std::ostringstream oss;
    oss << "{"
        << "\"type\":\"event\","
        << "\"source\":\"gateway\","
        << "\"gateway_id\":\"" << jsonEscape(config.heartbeat.gateway_id) << "\","
        << "\"event_type\":\"" << jsonEscape(event_type) << "\","
        << "\"device_id\":" << device_id << ","
        << "\"detail\":\"" << jsonEscape(detail) << "\","
        << "\"timestamp\":" << unixMsNowLocal()
        << "}";
    return oss.str();
}

std::string buildUpstreamOtaStatusJson(const GatewayConfig& config, const ota::Esp32OtaStatus& st) {
    std::ostringstream oss;
    oss << "{"
        << "\"type\":\"ota_status\","
        << "\"source\":\"gateway\","
        << "\"gateway_id\":\"" << jsonEscape(config.heartbeat.gateway_id) << "\","
        << "\"device_id\":" << static_cast<int>(st.device_id) << ","
        << "\"command_id\":" << st.command_id << ","
        << "\"ota_state\":\"" << jsonEscape(st.ota_state) << "\","
        << "\"progress\":" << st.progress << ","
        << "\"version_from\":\"" << jsonEscape(st.version_from) << "\","
        << "\"version_to\":\"" << jsonEscape(st.version_to) << "\","
        << "\"error_code\":" << st.error_code << ","
        << "\"timestamp\":" << unixMsNowLocal()
        << "}";
    return oss.str();
}

std::string buildUpstreamFrameEventJson(const SensorData& data) {
    std::ostringstream oss;
    const std::string fallback_name = "sensor-" + std::to_string(static_cast<int>(data.device_id));
    const std::string& name = data.device_name.empty() ? fallback_name : data.device_name;
    oss << "{"
        << "\"type\":\"event\","
        << "\"device_id\":" << static_cast<int>(data.device_id) << ","
        << "\"device_name\":\"" << jsonEscape(name) << "\","
        << "\"event_type\":\"" << jsonEscape(frameTypeName(data.frame_type)) << "\","
        << "\"timestamp\":" << data.timestamp_unix_ms << ","
        << "\"status\":" << static_cast<int>(data.status) << ","
        << "\"command_id\":" << data.command_id << ","
        << "\"command_result\":" << static_cast<int>(data.command_result) << ","
        << "\"link_type\":\"" << jsonEscape(data.link_type.empty() ? "serial" : data.link_type) << "\","
        << "\"payload_summary\":\"" << jsonEscape(data.payload_summary) << "\"";
    if (!data.topic_suffix.empty()) {
        oss << ",\"topic_suffix\":\"" << jsonEscape(data.topic_suffix) << "\"";
    }
    oss << "}";
    return oss.str();
}

std::uint16_t scaleToU16x10(double value) {
    if (value <= 0.0) {
        return 0U;
    }
    long scaled = static_cast<long>(value * 10.0 + 0.5);
    if (scaled < 0) {
        scaled = 0;
    }
    if (scaled > 65535L) {
        scaled = 65535L;
    }
    return static_cast<std::uint16_t>(scaled);
}

bool buildControlPayload(const ControlCommandRequest& req,
                         std::vector<std::uint8_t>& payload,
                         std::string& err) {
    payload.clear();
    payload.push_back(req.device_id);
    payload.push_back(static_cast<std::uint8_t>(req.command_id & 0xFFU));
    payload.push_back(static_cast<std::uint8_t>((req.command_id >> 8U) & 0xFFU));

    if (req.command_type == "set_led") {
        if (!req.has_on) {
            err = "set_led requires args.on";
            return false;
        }
        payload.push_back(0x01U);
        payload.push_back(req.on ? 1U : 0U);
    } else if (req.command_type == "set_mode") {
        if (!req.has_mode) {
            err = "set_mode requires args.mode";
            return false;
        }
        payload.push_back(0x03U);
        payload.push_back(req.mode_auto ? 1U : 0U);
    } else if (req.command_type == "get_status") {
        payload.push_back(0x04U);
    } else if (req.command_type == "set_threshold") {
        if (!req.has_threshold) {
            err = "set_threshold requires args.temperature and args.humidity";
            return false;
        }
        payload.push_back(0x05U);
        const std::uint16_t temp = scaleToU16x10(req.threshold_temp);
        const std::uint16_t humi = scaleToU16x10(req.threshold_humi);
        payload.push_back(static_cast<std::uint8_t>(temp & 0xFFU));
        payload.push_back(static_cast<std::uint8_t>((temp >> 8U) & 0xFFU));
        payload.push_back(static_cast<std::uint8_t>(humi & 0xFFU));
        payload.push_back(static_cast<std::uint8_t>((humi >> 8U) & 0xFFU));
    } else {
        err = "unsupported command_type: " + req.command_type;
        return false;
    }

    return true;
}

std::string buildWifiCommandLine(const ControlCommandRequest& req, std::uint16_t command_id) {
    std::ostringstream oss;
    oss << "{"
        << "\"type\":\"command\","
        << "\"device_id\":" << static_cast<int>(req.device_id) << ","
        << "\"command_id\":" << command_id << ","
        << "\"command_type\":\"" << jsonEscape(req.command_type) << "\","
        << "\"params\":{";

    if (req.command_type == "set_led") {
        oss << "\"value\":" << (req.on ? 1 : 0);
    } else if (req.command_type == "set_mode") {
        oss << "\"value\":" << (req.mode_auto ? 1 : 0);
    } else if (req.command_type == "set_threshold") {
        oss << "\"temperature\":" << req.threshold_temp << ","
            << "\"humidity\":" << req.threshold_humi;
    } else if (req.command_type == "set_report_interval") {
        oss << "\"interval_ms\":" << req.report_interval_ms;
    } else if (req.command_type == "set_log_level") {
        oss << "\"level\":\"" << jsonEscape(req.log_level) << "\"";
    }

    oss << "},"
        << "\"timeout_ms\":" << req.timeout_ms
        << "}";
    return oss.str();
}

bool parseControlCommandRequest(const std::string& body, ControlCommandRequest& out, std::string& err) {
    out = ControlCommandRequest{};
    const auto device_id = jsonGetInt(body, "device_id");
    const auto command_type = jsonGetString(body, "command_type");
    const auto command_alias = jsonGetString(body, "command");
    if (!device_id.has_value() || !command_type.has_value()) {
        if (!device_id.has_value() || !command_alias.has_value()) {
            err = "missing device_id or command_type";
            return false;
        }
    }
    if (*device_id < 0 || *device_id > 255) {
        err = "invalid device_id";
        return false;
    }
    out.device_id = static_cast<std::uint8_t>(*device_id);
    out.command_type = command_type.has_value() ? *command_type : *command_alias;

    const auto command_id = jsonGetInt(body, "command_id");
    if (command_id.has_value()) {
        if (*command_id <= 0 || *command_id > 65535) {
            err = "invalid command_id";
            return false;
        }
        out.command_id = static_cast<std::uint16_t>(*command_id);
        out.has_command_id = true;
    }

    const auto args = jsonGetObject(body, "args");
    const std::string& scope = args.has_value() ? *args : body;

    const auto timeout_ms = jsonGetInt(body, "timeout_ms");
    if (timeout_ms.has_value()) {
        if (*timeout_ms <= 0) {
            err = "invalid timeout_ms";
            return false;
        }
        out.timeout_ms = static_cast<int>(*timeout_ms);
        if (out.timeout_ms > 10000) {
            out.timeout_ms = 10000;
        }
    }

    if (out.command_type == "set_led") {
        const auto on_bool = jsonGetBoolFromScope(scope, "on");
        if (on_bool.has_value()) {
            out.has_on = true;
            out.on = *on_bool;
        } else {
            const auto value = jsonGetIntFromScope(scope, "value");
            if (!value.has_value()) {
                err = "set_led requires args.on or args.value";
                return false;
            }
            if (*value != 0 && *value != 1) {
                err = "set_led value must be 0 or 1";
                return false;
            }
            out.has_on = true;
            out.on = (*value != 0);
        }
    } else if (out.command_type == "set_mode") {
        const auto mode_str = jsonGetStringFromScope(scope, "mode");
        if (mode_str.has_value()) {
            out.has_mode = true;
            if (*mode_str == "auto" || *mode_str == "1") {
                out.mode_auto = true;
            } else if (*mode_str == "manual" || *mode_str == "0") {
                out.mode_auto = false;
            } else {
                err = "set_mode mode must be auto/manual or 0/1";
                return false;
            }
        } else {
            const auto mode_value = jsonGetIntFromScope(scope, "mode");
            if (!mode_value.has_value()) {
                const auto auto_mode = jsonGetBoolFromScope(scope, "auto_mode");
                if (!auto_mode.has_value()) {
                    err = "set_mode requires args.mode";
                    return false;
                }
                out.has_mode = true;
                out.mode_auto = *auto_mode;
            } else {
                if (*mode_value != 0 && *mode_value != 1) {
                    err = "set_mode value must be 0 or 1";
                    return false;
                }
                out.has_mode = true;
                out.mode_auto = (*mode_value != 0);
            }
        }
    } else if (out.command_type == "set_threshold") {
        const auto temp = jsonGetDoubleFromScope(scope, "temperature");
        const auto humi = jsonGetDoubleFromScope(scope, "humidity");
        const auto temp_alt = jsonGetDoubleFromScope(scope, "temp");
        const auto humi_alt = jsonGetDoubleFromScope(scope, "humi");
        if (temp.has_value() && humi.has_value()) {
            out.has_threshold = true;
            out.threshold_temp = *temp;
            out.threshold_humi = *humi;
        } else if (temp_alt.has_value() && humi_alt.has_value()) {
            out.has_threshold = true;
            out.threshold_temp = *temp_alt;
            out.threshold_humi = *humi_alt;
        } else {
            err = "set_threshold requires args.temperature/args.humidity";
            return false;
        }
    } else if (out.command_type == "set_report_interval") {
        const auto interval_ms = jsonGetIntFromScope(scope, "interval_ms");
        const auto value = jsonGetIntFromScope(scope, "value");
        const long long interval = interval_ms.has_value() ? *interval_ms : (value.has_value() ? *value : -1);
        if (interval <= 0 || interval > 86400000LL) {
            err = "set_report_interval requires positive interval_ms/value";
            return false;
        }
        out.has_report_interval = true;
        out.report_interval_ms = static_cast<int>(interval);
    } else if (out.command_type == "set_log_level") {
        const auto level = jsonGetStringFromScope(scope, "level");
        const auto value = jsonGetStringFromScope(scope, "value");
        const auto int_level = jsonGetIntFromScope(scope, "level");
        const auto int_value = jsonGetIntFromScope(scope, "value");
        if (level.has_value()) {
            out.has_log_level = true;
            out.log_level = *level;
        } else if (value.has_value()) {
            out.has_log_level = true;
            out.log_level = *value;
        } else if (int_level.has_value()) {
            out.has_log_level = true;
            out.log_level = std::to_string(*int_level);
        } else if (int_value.has_value()) {
            out.has_log_level = true;
            out.log_level = std::to_string(*int_value);
        } else {
            err = "set_log_level requires args.level or args.value";
            return false;
        }
    } else if (out.command_type != "get_status") {
        err = "unsupported command_type: " + out.command_type;
        return false;
    }

    return true;
}

std::string buildCommandJsonResponse(const ControlCommandResult& result) {
    std::ostringstream oss;
    oss << "{"
        << "\"ok\":" << (result.ok ? "true" : "false") << ","
        << "\"device_id\":" << static_cast<int>(result.device_id) << ","
        << "\"command_id\":" << result.command_id << ","
        << "\"command_type\":\"" << jsonEscape(result.command_type) << "\","
        << "\"status\":\"" << jsonEscape(result.status) << "\","
        << "\"latency_ms\":" << result.latency_ms << ","
        << "\"error\":\"" << jsonEscape(result.error) << "\""
        << "}";
    return oss.str();
}

bool buildMqttCommandPayload(const ControlCommandRequest& req,
                             std::uint16_t command_id,
                             std::string& payload,
                             std::string& err) {
    std::ostringstream oss;
    oss << "{"
        << "\"command_id\":" << command_id << ",";
    if (req.command_type == "set_led") {
        oss << "\"cmd\":\"led_set\","
            << "\"command_type\":\"set_led\","
            << "\"value\":" << (req.on ? 1 : 0);
    } else if (req.command_type == "get_status") {
        oss << "\"cmd\":\"status_get\","
            << "\"command_type\":\"get_status\"";
    } else if (req.command_type == "set_report_interval") {
        oss << "\"cmd\":\"report_interval_set\","
            << "\"command_type\":\"set_report_interval\","
            << "\"interval_ms\":" << req.report_interval_ms;
    } else if (req.command_type == "set_log_level") {
        oss << "\"cmd\":\"set_log_level\","
            << "\"command_type\":\"set_log_level\","
            << "\"level\":\"" << jsonEscape(req.log_level) << "\"";
    } else {
        err = "unsupported mqtt command_type: " + req.command_type;
        return false;
    }
    oss << "}";
    payload = oss.str();
    return true;
}

bool buildMqttOtaStartPayload(const ota::Esp32OtaStart& req, std::string& payload) {
    std::ostringstream oss;
    oss << "{"
        << "\"command_id\":" << req.command_id << ","
        << "\"version\":\"" << jsonEscape(req.version) << "\","
        << "\"firmware_url\":\"" << jsonEscape(req.firmware_url) << "\","
        << "\"size\":" << req.size << ","
        << "\"crc32\":\"" << jsonEscape(req.crc32) << "\""
        << "}";
    payload = oss.str();
    return true;
}

std::string upstreamTopicBase(const UploaderConfig& cfg) {
    if (cfg.mqtt_topic.empty()) {
        return "upstream";
    }
    const auto pos = cfg.mqtt_topic.rfind('/');
    if (pos == std::string::npos) {
        return cfg.mqtt_topic;
    }
    return cfg.mqtt_topic.substr(0, pos);
}

std::string resolveUpstreamControlTopic(const UploaderConfig& cfg,
                                        const std::string& configured,
                                        const std::string& suffix) {
    if (!configured.empty()) {
        return configured;
    }
    return upstreamTopicBase(cfg) + suffix;
}

bool writeAll(int fd, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n > 0) {
            off += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        return false;
    }
    return true;
}

std::string makeHttpResponse(const std::string& status, const std::string& content_type, const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "Cache-Control: no-store\r\n"
        << "\r\n"
        << body;
    return oss.str();
}

} // namespace

#ifdef SG_HAVE_MOSQUITTO
void mqttIngressOnConnect(struct mosquitto*, void* userdata, int rc) {
    auto* app = static_cast<AppController*>(userdata);
    if (app != nullptr) {
        app->handleMqttIngressConnect(rc);
    }
}

void mqttIngressOnDisconnect(struct mosquitto*, void* userdata, int rc) {
    auto* app = static_cast<AppController*>(userdata);
    if (app != nullptr) {
        app->handleMqttIngressDisconnect(rc);
    }
}

void mqttIngressOnMessage(struct mosquitto*, void* userdata, const struct mosquitto_message* msg) {
    auto* app = static_cast<AppController*>(userdata);
    if (app == nullptr || msg == nullptr || msg->topic == nullptr || msg->payload == nullptr) {
        return;
    }
    app->handleMqttIngressMessage(
        std::string(msg->topic),
        std::string(static_cast<const char*>(msg->payload), static_cast<std::size_t>(msg->payloadlen)));
}

void upstreamControlOnConnect(struct mosquitto*, void* userdata, int rc) {
    auto* app = static_cast<AppController*>(userdata);
    if (app != nullptr) {
        app->handleUpstreamControlConnect(rc);
    }
}

void upstreamControlOnDisconnect(struct mosquitto*, void* userdata, int rc) {
    auto* app = static_cast<AppController*>(userdata);
    if (app != nullptr) {
        app->handleUpstreamControlDisconnect(rc);
    }
}

void upstreamControlOnMessage(struct mosquitto*, void* userdata, const struct mosquitto_message* msg) {
    auto* app = static_cast<AppController*>(userdata);
    if (app == nullptr || msg == nullptr || msg->topic == nullptr || msg->payload == nullptr) {
        return;
    }
    app->handleUpstreamControlMessage(
        std::string(msg->topic),
        std::string(static_cast<const char*>(msg->payload), static_cast<std::size_t>(msg->payloadlen)));
}
#endif

AppController::AppController(const GatewayConfig& config)
    : config_(config),
      raw_queue_(config.runtime.queue_capacity),
      data_queue_(config.runtime.queue_capacity),
      uploader_(UploaderFactory::create(config.uploader, stats_)) {
    initDeviceRegistry();
    refreshDeviceMetrics();
}

AppController::~AppController() {
    stop();
}

void AppController::reapFinishedOtaTasksLocked() {
    auto it = ota_tasks_.begin();
    while (it != ota_tasks_.end()) {
        if (it->second.valid() &&
            it->second.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            try {
                it->second.get();
            } catch (const std::exception& ex) {
                Logger::instance().error("[OTA] async task exception task_uuid=" + it->first +
                                         " err=" + ex.what());
            } catch (...) {
                Logger::instance().error("[OTA] async task exception task_uuid=" + it->first);
            }
            it = ota_tasks_.erase(it);
            continue;
        }
        ++it;
    }
}

void AppController::launchOtaTask(std::string task_uuid,
                                  std::string firmware_id,
                                  std::string device_type,
                                  std::string transport,
                                  std::string target,
                                  std::uint8_t device_id,
                                  std::string firmware_url) {
    std::lock_guard<std::mutex> lock(ota_tasks_mutex_);
    reapFinishedOtaTasksLocked();
    active_ota_task_ids_.insert(task_uuid);
    ota_tasks_.emplace_back(
        task_uuid,
        std::async(std::launch::async,
                   [this,
                    task_uuid,
                    firmware_id = std::move(firmware_id),
                    device_type = std::move(device_type),
                    transport = std::move(transport),
                    target = std::move(target),
                    device_id,
                    firmware_url = std::move(firmware_url)]() mutable {
                       auto clear_task = [&]() {
                           std::lock_guard<std::mutex> task_lock(ota_tasks_mutex_);
                           active_ota_task_ids_.erase(task_uuid);
                       };
                       try {
                           Logger::instance().info("[OTA] task thread start task_uuid=" + task_uuid +
                                                   " device_id=" + std::to_string(static_cast<int>(device_id)));
                           executeOtaTask(
                               task_uuid,
                               std::move(firmware_id),
                               std::move(device_type),
                               std::move(transport),
                               std::move(target),
                               device_id,
                               std::move(firmware_url),
                               [this](std::uint8_t dev_id, const std::string& line) {
                                   if (!running_.load()) {
                                       Logger::instance().warn("[OTA] wifi sender aborted: app stopping");
                                       return false;
                                   }
                                   return sendWifiCommandLine(dev_id, line);
                               },
                               [this](std::uint8_t dev_id, const std::string& payload, std::string& out_err) {
                                   if (!running_.load()) {
                                       out_err = "app stopping";
                                       Logger::instance().warn("[OTA] mqtt sender aborted: app stopping");
                                       return false;
                                   }
                                   return sendMqttDevicePayload(dev_id, "ota/start", payload, out_err);
                               },
                               [this](std::uint8_t dev_id, const std::vector<std::uint8_t>& frame) {
                                   if (!running_.load()) {
                                       Logger::instance().warn("[OTA] tcp_binary sender aborted: app stopping");
                                       return false;
                                   }
                                   return sendTcpBinaryFrame(dev_id, frame);
                               });
                           Logger::instance().info("[OTA] task thread exit task_uuid=" + task_uuid);
                       } catch (const std::exception& ex) {
                           clear_task();
                           Logger::instance().error("[OTA] task thread exception task_uuid=" + task_uuid +
                                                    " err=" + ex.what());
                           throw;
                       } catch (...) {
                           clear_task();
                           Logger::instance().error("[OTA] task thread exception task_uuid=" + task_uuid);
                           throw;
                       }
                       clear_task();
                   }));
}

void AppController::cancelActiveOtaTasks() {
    std::vector<std::string> task_ids;
    {
        std::lock_guard<std::mutex> lock(ota_tasks_mutex_);
        for (const auto& task_id : active_ota_task_ids_) {
            task_ids.push_back(task_id);
        }
    }
    for (const auto& task_id : task_ids) {
        Logger::instance().warn("[OTA] cancel requested due to app stop task_uuid=" + task_id);
        otaCancelSet(task_id, true);
    }
}

void AppController::waitForOtaTasks() {
    std::vector<std::pair<std::string, std::future<void>>> tasks;
    {
        std::lock_guard<std::mutex> lock(ota_tasks_mutex_);
        tasks.swap(ota_tasks_);
    }
    if (!tasks.empty()) {
        Logger::instance().info("[OTA] waiting for " + std::to_string(tasks.size()) + " ota task(s) to exit");
    }
    for (auto& task : tasks) {
        if (!task.second.valid()) {
            continue;
        }
        try {
            task.second.get();
        } catch (const std::exception& ex) {
            Logger::instance().error("[OTA] wait task exception task_uuid=" + task.first +
                                     " err=" + ex.what());
        } catch (...) {
            Logger::instance().error("[OTA] wait task exception task_uuid=" + task.first);
        }
    }
}

bool AppController::start() {
    history_store_ = std::make_unique<storage::HistoryStore>();

    std::string err;
    serial_available_ = serial_.open(config_.serial, err);
    if (!serial_available_) {
        Logger::instance().warn(
            "serial open failed, continuing without serial transport: " + err);
    }
    const bool has_any_ingress = serial_available_ ||
                                 config_.wifi_device_server.enabled ||
                                 config_.mqtt_device_ingress.enabled ||
                                 config_.tcp_binary.enabled;
    if (!has_any_ingress) {
        Logger::instance().error("app start failed: no active ingress transport available");
        return false;
    }
    if (uploader_ == nullptr) {
        Logger::instance().error("uploader init failed");
        return false;
    }

    running_.store(true);
    start_time_ = std::chrono::steady_clock::now();
    if (serial_available_) {
        serial_thread_ = std::thread(&AppController::serialLoop, this);
    }
    parser_thread_ = std::thread(&AppController::parserLoop, this);
    uploader_thread_ = std::thread(&AppController::uploaderLoop, this);
    stats_thread_ = std::thread(&AppController::statsLoop, this);
    if (config_.wifi_device_server.enabled) {
        wifi_thread_ = std::thread(&AppController::wifiDeviceServerLoop, this);
    }
    if (config_.mqtt_device_ingress.enabled) {
        mqtt_thread_ = std::thread(&AppController::mqttDeviceLoop, this);
    }
    if (config_.uploader.type == "mqtt" && config_.uploader.mqtt_control_enabled) {
        upstream_control_thread_ = std::thread(&AppController::upstreamControlLoop, this);
    }
    if (config_.tcp_binary.enabled) {
        tcp_binary_thread_ = std::thread(&AppController::tcpBinaryLoop, this);
    }
    if (config_.command.enabled) {
        command_thread_ = std::thread(&AppController::commandLoop, this);
    }
    if (config_.heartbeat.enabled) {
        heartbeat_thread_ = std::thread(&AppController::heartbeatLoop, this);
    }
    if (config_.monitor.enabled) {
        monitor_thread_ = std::thread(&AppController::monitorLoop, this);
    }

    Logger::instance().info("app started, instance=" + config_.serial.instance_name +
                            " serial=" + config_.serial.device +
                            " serial_available=" + std::string(serial_available_ ? "true" : "false") +
                            " uploader.type=" + config_.uploader.type);
    return true;
}

void AppController::stop() {
    const bool was_running = running_.exchange(false);
    if (!was_running) {
        return;
    }

    raw_queue_.stop();
    data_queue_.stop();
    command_cv_.notify_all();
    cancelActiveOtaTasks();
    if (uploader_ != nullptr) {
        uploader_->close();
    }
#ifdef SG_HAVE_MOSQUITTO
    {
        std::lock_guard<std::mutex> lock(mqtt_device_mutex_);
        if (mqtt_device_mosq_ != nullptr) {
            mosquitto_disconnect(static_cast<mosquitto*>(mqtt_device_mosq_));
        }
    }
    {
        std::lock_guard<std::mutex> lock(upstream_control_mutex_);
        if (upstream_control_mosq_ != nullptr) {
            mosquitto_disconnect(static_cast<mosquitto*>(upstream_control_mosq_));
        }
    }
#endif
    serial_.close();
    waitForOtaTasks();

    if (serial_thread_.joinable()) serial_thread_.join();
    if (parser_thread_.joinable()) parser_thread_.join();
    if (uploader_thread_.joinable()) uploader_thread_.join();
    if (stats_thread_.joinable()) stats_thread_.join();
    if (command_thread_.joinable()) command_thread_.join();
    if (wifi_thread_.joinable()) wifi_thread_.join();
    if (mqtt_thread_.joinable()) mqtt_thread_.join();
    if (upstream_control_thread_.joinable()) upstream_control_thread_.join();
    if (tcp_binary_thread_.joinable()) tcp_binary_thread_.join();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    if (monitor_thread_.joinable()) monitor_thread_.join();

    Logger::instance().info("app stopped");
}

void AppController::serialLoop() {
    std::vector<std::uint8_t> buf(config_.serial.read_chunk_size);

    while (running_.load()) {
        std::string err;
        const ssize_t n = serial_.read(buf.data(), buf.size(), err);
        if (n < 0) {
            Logger::instance().warn(err);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        std::vector<std::uint8_t> chunk(buf.begin(), buf.begin() + n);
        if (!raw_queue_.push(std::move(chunk))) {
            return;
        }
    }
}

void AppController::handleControlCommandRequestJson(const std::string& req_body,
                                                    std::string& http_status,
                                                    std::string& response_body) {
    ControlCommandRequest cmd_req;
    std::string parse_err;
    if (!parseControlCommandRequest(req_body, cmd_req, parse_err)) {
        http_status = "400 Bad Request";
        response_body = "{\"ok\":false,\"device_id\":0,\"command_id\":0,\"command_type\":\"\",\"status\":\"error\",\"latency_ms\":0,\"error\":\"" +
                        jsonEscape(parse_err) + "\"}";
        return;
    }
    executeControlCommand(std::move(cmd_req), http_status, response_body);
}

void AppController::executeControlCommand(ControlCommandRequest cmd_req,
                                          std::string& http_status,
                                          std::string& response_body) {
    ControlCommandResult result;
    result.device_id = cmd_req.device_id;
    result.command_type = cmd_req.command_type;

    bool device_found = false;
    bool device_online = false;
    const auto devices = device_registry_.snapshot();
    for (const auto& d : devices) {
        if (d.device_id == cmd_req.device_id) {
            device_found = true;
            device_online = d.online;
            break;
        }
    }

    if (!device_found) {
        http_status = "404 Not Found";
        response_body = "{\"ok\":false,\"device_id\":" + std::to_string(static_cast<int>(cmd_req.device_id)) +
                        ",\"command_id\":0,\"command_type\":\"" + jsonEscape(cmd_req.command_type) +
                        "\",\"status\":\"not_found\",\"latency_ms\":0,\"error\":\"device not found\"}";
        return;
    }
    if (!device_online) {
        http_status = "409 Conflict";
        response_body = "{\"ok\":false,\"device_id\":" + std::to_string(static_cast<int>(cmd_req.device_id)) +
                        ",\"command_id\":0,\"command_type\":\"" + jsonEscape(cmd_req.command_type) +
                        "\",\"status\":\"offline\",\"latency_ms\":0,\"error\":\"device offline or unavailable\"}";
        return;
    }

    std::uint16_t command_id = 0;
    if (cmd_req.has_command_id) {
        command_id = cmd_req.command_id;
        const auto key = makeCommandKey(cmd_req.device_id, command_id);
        std::lock_guard<std::mutex> lock(command_mutex_);
        if (pending_commands_.find(key) != pending_commands_.end()) {
            http_status = "409 Conflict";
            response_body = "{\"ok\":false,\"device_id\":" + std::to_string(static_cast<int>(cmd_req.device_id)) +
                            ",\"command_id\":" + std::to_string(command_id) +
                            ",\"command_type\":\"" + jsonEscape(cmd_req.command_type) +
                            "\",\"status\":\"error\",\"latency_ms\":0,\"error\":\"duplicate command_id\"}";
            return;
        }
        PendingCommand cmd;
        cmd.device_id = cmd_req.device_id;
        cmd.command_id = command_id;
        cmd.command_type = cmd_req.command_type;
        cmd.enqueue_ms = unixMsNow();
        pending_commands_[key] = cmd;
        stats_.addCommandSubmitCount(1);
    } else if (!reservePendingCommand(cmd_req.device_id, cmd_req.command_type, command_id)) {
        http_status = "500 Internal Server Error";
        response_body = "{\"ok\":false,\"device_id\":" + std::to_string(static_cast<int>(cmd_req.device_id)) +
                        ",\"command_id\":0,\"command_type\":\"" + jsonEscape(cmd_req.command_type) +
                        "\",\"status\":\"error\",\"latency_ms\":0,\"error\":\"unable to allocate command id\"}";
        return;
    }

    cmd_req.command_id = command_id;
    const auto start = std::chrono::steady_clock::now();
    result.command_id = command_id;

    const std::string active_transport = activeTransportFor(cmd_req.device_id);
    std::string send_err;
    if (!isCommandSupportedByTransport(cmd_req, active_transport, send_err)) {
        Logger::instance().warn("[CMD] unsupported command device_id=" +
                                std::to_string(static_cast<int>(cmd_req.device_id)) +
                                " transport=" + active_transport +
                                " command_type=" + cmd_req.command_type);
        http_status = "400 Bad Request";
        result.status = "failed";
        result.error = send_err;
    }
    bool send_ok = false;
    if (result.error.empty() && active_transport == "wifi") {
        const std::string line = buildWifiCommandLine(cmd_req, command_id) + "\n";
        send_ok = sendWifiCommandLine(cmd_req.device_id, line);
        if (!send_ok) {
            result.status = "failed";
            result.error = "wifi device not connected";
            http_status = "409 Conflict";
        }
    } else if (result.error.empty() && active_transport == "mqtt") {
        std::string payload;
        if (!buildMqttCommandPayload(cmd_req, command_id, payload, send_err)) {
            http_status = "400 Bad Request";
            result.status = "failed";
            result.error = send_err;
        } else {
            send_ok = sendMqttDevicePayload(cmd_req.device_id, "command/down", payload, send_err);
            if (!send_ok) {
                result.status = "failed";
                result.error = send_err.empty() ? "mqtt publish failed" : send_err;
                http_status = "409 Conflict";
            }
        }
    } else if (result.error.empty() && (active_transport == "serial" || active_transport == "tcp_binary")) {
        std::vector<std::uint8_t> payload;
        if (!buildControlPayload(cmd_req, payload, send_err)) {
            http_status = "400 Bad Request";
            result.status = "failed";
            result.error = send_err;
        } else if (active_transport == "tcp_binary") {
            send_ok = sendTcpBinaryFrame(cmd_req.device_id, payload);
            if (!send_ok) {
                result.status = "failed";
                result.error = "tcp_binary device not connected";
                http_status = "409 Conflict";
            }
        } else {
            if (!serial_available_) {
                result.status = "failed";
                result.error = "serial transport unavailable";
                http_status = "409 Conflict";
            } else {
                std::string serial_err;
                const ssize_t wn = serial_.write(payload.data(), payload.size(), serial_err);
                send_ok = wn >= 0;
                if (!send_ok) {
                    result.status = "failed";
                    result.error = serial_err.empty() ? "serial write failed" : serial_err;
                    http_status = "409 Conflict";
                }
            }
        }
    } else if (result.error.empty()) {
        result.status = "failed";
        result.error = "device transport unavailable";
        http_status = "409 Conflict";
    }

    if (!send_ok) {
        std::lock_guard<std::mutex> lock(command_mutex_);
        pending_commands_.erase(makeCommandKey(cmd_req.device_id, command_id));
        result.ok = false;
        result.latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count());
        if (result.status.empty()) {
            result.status = "failed";
        }
        if (response_body.empty()) {
            response_body = buildCommandJsonResponse(result);
        }
        pushControlCommandLog(result);
        return;
    }

    std::uint8_t ack_result = 0xFFU;
    const bool acked = waitCommandAck(cmd_req.device_id, command_id, cmd_req.timeout_ms, ack_result);
    result.latency_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
    if (acked) {
        if (ack_result == 0) {
            result.ok = true;
            result.status = "acked";
            result.error.clear();
        } else {
            result.ok = false;
            result.status = "failed";
            result.error = "device returned result=" + std::to_string(static_cast<int>(ack_result));
        }
    } else {
        result.ok = false;
        result.status = "timeout";
        result.latency_ms = cmd_req.timeout_ms;
        result.error = "command ack timeout";
    }
    response_body = buildCommandJsonResponse(result);
    pushControlCommandLog(result);
}

void AppController::handleOtaTaskRequestJson(const std::string& req_body,
                                             std::string& http_status,
                                             std::string& response_body) {
    storage::PersistentStore store(otaTaskStorePath());
    ota::OtaTaskManager mgr(store);
    std::ostringstream oss;
    std::string err;

    const auto device_id = jsonGetInt(req_body, "device_id");
    auto device_type = jsonGetString(req_body, "device_type");
    const auto firmware_id = jsonGetString(req_body, "firmware_id");
    const auto transport = jsonGetString(req_body, "transport");
    const auto target = jsonGetString(req_body, "target");
    const auto firmware_url = jsonGetString(req_body, "firmware_url");

    if (!device_id.has_value() || !firmware_id.has_value()) {
        http_status = "400 Bad Request";
        response_body = "{\"error\":\"missing device_id/firmware_id\"}";
        return;
    }
    if (*device_id < 0 || *device_id > 255) {
        http_status = "400 Bad Request";
        response_body = "{\"error\":\"invalid device_id\"}";
        return;
    }

    if (!device_type.has_value() || device_type->empty()) {
        const std::string active_t = activeTransportFor(static_cast<std::uint8_t>(*device_id));
        if (active_t == "mqtt" || active_t == "wifi") {
            device_type = std::string("esp32");
        } else if (active_t == "tcp_binary") {
            device_type = std::string("stm32");
        } else {
            device_type = std::string("stm32");
        }
    }

    const auto t = mgr.create(static_cast<std::uint8_t>(*device_id), *device_type, *firmware_id, err);
    if (!err.empty()) {
        http_status = "500 Internal Server Error";
        response_body = "{\"error\":\"" + jsonEscape(err) + "\"}";
        return;
    }

    std::string run_transport = transport.value_or("");
    if (run_transport.empty()) {
        const std::string active_t = activeTransportFor(static_cast<std::uint8_t>(*device_id));
        if (active_t == "tcp_binary") {
            run_transport = "tcp_binary";
        } else if (active_t == "mqtt") {
            run_transport = "mqtt";
        } else {
            run_transport = "serial";
        }
    }
    if (run_transport != "serial" && run_transport != "tcp_binary" && run_transport != "mqtt") {
        http_status = "400 Bad Request";
        response_body = "{\"error\":\"invalid transport, expected serial|tcp_binary|mqtt\"}";
        return;
    }

    const std::string run_target = target.value_or("127.0.0.1:19090");
    launchOtaTask(
        t.task_uuid,
        t.firmware_id,
        t.device_type,
        run_transport,
        run_target,
        t.device_id,
        firmware_url.value_or(""));

    oss << "{"
        << "\"task_uuid\":\"" << jsonEscape(t.task_uuid) << "\","
        << "\"device_id\":" << static_cast<int>(t.device_id) << ","
        << "\"device_type\":\"" << jsonEscape(t.device_type) << "\","
        << "\"firmware_id\":\"" << jsonEscape(t.firmware_id) << "\","
        << "\"state\":\"" << ota::toString(t.state) << "\","
        << "\"transport\":\"" << jsonEscape(run_transport) << "\","
        << "\"target\":\"" << jsonEscape(run_target) << "\""
        << "}";
    response_body = oss.str();
}

void AppController::parserLoop() {
    while (running_.load()) {
        std::vector<std::uint8_t> chunk;
        if (!raw_queue_.pop(chunk, std::chrono::milliseconds(100))) {
            continue;
        }

        parser_.append(chunk.data(), chunk.size());
        ParseCounters counters{};
        auto data_items = parser_.extract(counters);
        stats_.addReceivedFrames(counters.received_frames);
        stats_.addParsedOk(counters.parsed_ok);
        stats_.addCrcErrors(counters.crc_errors);
        stats_.addUnknownTypeFrames(counters.unknown_types);
        stats_.addDroppedBytes(counters.dropped_bytes);

        if (counters.crc_errors > 0 && history_store_) {
            history_store_->insertSystemEvent("crc_error", 0,
                "count=" + std::to_string(counters.crc_errors));
        }
        if (counters.unknown_types > 0 && history_store_) {
            history_store_->insertSystemEvent("unknown_frame", 0,
                "count=" + std::to_string(counters.unknown_types));
        }

        for (auto& item : data_items) {
            if (otaBusinessMuted(item.device_id)) {
                const std::uint8_t drop_type = static_cast<std::uint8_t>(item.frame_type);
                char drop_hex[5]{};
                std::snprintf(drop_hex, sizeof(drop_hex), "%02X", static_cast<unsigned int>(drop_type));
                Logger::instance().info("[OTA][DROP] non-ota frame dropped type=0x" +
                                        std::string(drop_hex) + " device=" +
                                        std::to_string(static_cast<int>(item.device_id)) +
                                        " transport=serial");
                continue;
            }
            if (item.frame_type == FrameType::SENSOR_DATA && item.seq > 0) {
                const auto now_ms = unixMsNowLocal();
                ackDebugMarkRx(now_ms);
                Logger::instance().info("rx report seq=" + std::to_string(item.seq));
                const auto wn = sendReportAckToSerial(serial_, item.device_id, static_cast<std::uint32_t>(item.seq));
                const bool ok = wn == 11;
                ackDebugMarkTx(now_ms, ok);
                ackDebugAddLine("serial rx report seq=" + std::to_string(item.seq));
                Logger::instance().info("tx ack seq=" + std::to_string(item.seq));
                Logger::instance().info("ack write bytes=" + std::to_string(wn));
                ackDebugAddLine("serial tx ack seq=" + std::to_string(item.seq) +
                                " ack write bytes=" + std::to_string(wn));
            }
            if (!data_queue_.push(std::move(item))) {
                return;
            }
        }
    }
}

void AppController::uploaderLoop() {
    while (running_.load()) {
        SensorData data;
        if (!data_queue_.pop(data, std::chrono::milliseconds(100))) {
            continue;
        }

        if (data.frame_type == FrameType::COMMAND_ACK) {
            completeCommandAck(data.device_id, data.command_id, data.command_result);
        }

        const auto it = device_map_.find(data.device_id);
        if (it != device_map_.end()) {
            if (!it->second.enabled) {
                device_registry_.markDeviceError(data.device_id, "device disabled by config", data.timestamp_unix_ms);
                continue;
            }
            data.device_name = it->second.name;
            data.topic_suffix = it->second.topic_suffix;
            if (data.link_type.empty()) {
                data.link_type = it->second.link_type;
            }
        } else {
            if (config_.runtime.drop_unknown_devices) {
                if (unknown_device_warned_.insert(data.device_id).second) {
                    Logger::instance().warn(
                        "drop unknown device_id=" + std::to_string(static_cast<int>(data.device_id)));
                }
                device_registry_.markDeviceError(data.device_id, "unknown device dropped", data.timestamp_unix_ms);
                if (history_store_) {
                    history_store_->insertSystemEvent("device_dropped",
                        static_cast<int>(data.device_id), "unknown device");
                }
                (void)publishUpstreamSystemEvent("device_dropped",
                                                 static_cast<int>(data.device_id),
                                                 "unknown device");
                continue;
            }
            data.device_name = "sensor-" + std::to_string(static_cast<int>(data.device_id));
            if (data.link_type.empty()) {
                data.link_type = "serial";
            }
        }

        if (!shouldAcceptTransportData(data.device_id, data.link_type)) {
            continue;
        }
        bindActiveTransport(data.device_id, data.link_type, transportPriority(data.link_type));
        const auto update_result = device_registry_.updateOnEvent(data);
        if (update_result.became_online) {
            Logger::instance().info("instance=" + config_.serial.instance_name + " device online device_id=" +
                                    std::to_string(static_cast<int>(data.device_id)) +
                                    " name=" + update_result.state.device_name);
            if (history_store_) {
                history_store_->insertSystemEvent("device_online",
                    static_cast<int>(data.device_id), update_result.state.device_name);
            }
            (void)publishUpstreamSystemEvent("device_online",
                                             static_cast<int>(data.device_id),
                                             update_result.state.device_name);
        }
        refreshDeviceMetrics();
        pushRecentSample(data);
        if (history_store_) {
            history_store_->insertSensorEvent(data);
        }
        bool uploaded = false;
        if (uploader_ != nullptr) {
            switch (data.frame_type) {
                case FrameType::SENSOR_DATA:
                    uploaded = uploader_->upload(data);
                    break;
                case FrameType::DEVICE_STATUS:
                case FrameType::HEARTBEAT:
                    uploaded = uploader_->uploadStatus(data);
                    break;
                case FrameType::COMMAND_ACK:
                    uploaded = uploader_->uploadCommandAck(data);
                    break;
                case FrameType::ALARM_EVENT:
                case FrameType::REPORT_ACK:
                case FrameType::UNKNOWN:
                    uploaded = uploader_->uploadEvent(buildUpstreamFrameEventJson(data));
                    break;
            }
        }
        if (!uploaded) {
            Logger::instance().warn("instance=" + config_.serial.instance_name +
                                    " upload failed, data cached for retry type=" +
                                    std::string(frameTypeName(data.frame_type)) + " device_id=" +
                                    std::to_string(static_cast<int>(data.device_id)));
            if (history_store_) {
                history_store_->insertSystemEvent("upload_failed",
                    static_cast<int>(data.device_id),
                    std::string(frameTypeName(data.frame_type)));
            }
            (void)publishUpstreamSystemEvent("upload_failed",
                                             static_cast<int>(data.device_id),
                                             std::string(frameTypeName(data.frame_type)));
        }
    }
}

void AppController::statsLoop() {
    const auto interval = std::chrono::seconds(config_.runtime.stats_interval_sec);
    while (running_.load()) {
        std::this_thread::sleep_for(interval);
        if (!running_.load()) {
            break;
        }

        const std::uint64_t now_ms = unixMsNow();
        const std::uint64_t timeout_ms =
            static_cast<std::uint64_t>(config_.runtime.device_offline_timeout_sec) * 1000ULL;
        const auto transitions = device_registry_.markOfflineByTimeout(now_ms, timeout_ms);
        for (const auto& t : transitions) {
            Logger::instance().warn("instance=" + config_.serial.instance_name +
                                    " device offline device_id=" +
                                    std::to_string(static_cast<int>(t.device_id)) +
                                    " name=" + t.device_name);
            if (history_store_) {
                history_store_->insertSystemEvent("device_offline",
                    static_cast<int>(t.device_id), t.device_name);
            }
            (void)publishUpstreamSystemEvent("device_offline",
                                             static_cast<int>(t.device_id),
                                             t.device_name);
        }
        refreshDeviceMetrics();

        const auto snap = stats_.snapshot();
        if (config_.runtime.cache_backlog_warn_threshold > 0 &&
            snap.cache_backlog >= config_.runtime.cache_backlog_warn_threshold) {
            Logger::instance().warn("instance=" + config_.serial.instance_name +
                                    " cache backlog high backlog=" + std::to_string(snap.cache_backlog));
            if (history_store_) {
                history_store_->insertSystemEvent("cache_backlog", 0,
                    "backlog=" + std::to_string(snap.cache_backlog));
            }
            (void)publishUpstreamSystemEvent("cache_backlog",
                                             0,
                                             "backlog=" + std::to_string(snap.cache_backlog));
        }

        const auto ru = ResourceUsageReader::read();
        Logger::instance().info("instance=" + config_.serial.instance_name + " " +
                                stats_.snapshotLine() + " " + ResourceUsageReader::toLine(ru));
    }
}

void AppController::commandLoop() {
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        Logger::instance().error("command socket create failed");
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(config_.command.port));
    if (inet_pton(AF_INET, config_.command.bind_host.c_str(), &addr.sin_addr) != 1) {
        Logger::instance().error("command bind_host invalid: " + config_.command.bind_host);
        ::close(listen_fd);
        return;
    }

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        Logger::instance().error("command bind failed: " + std::string(std::strerror(errno)));
        ::close(listen_fd);
        return;
    }
    if (listen(listen_fd, 4) != 0) {
        Logger::instance().error("command listen failed");
        ::close(listen_fd);
        return;
    }

    const int flags = fcntl(listen_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
    }

    Logger::instance().info("instance=" + config_.serial.instance_name +
                            " command server listening on " + config_.command.bind_host + ":" +
                            std::to_string(config_.command.port));

    while (running_.load()) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        const int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            continue;
        }

        const int cflags = fcntl(client_fd, F_GETFL, 0);
        if (cflags >= 0) {
            fcntl(client_fd, F_SETFL, cflags | O_NONBLOCK);
        }

        std::string pending;
        std::array<char, 512> buf{};
        while (running_.load()) {
            const ssize_t n = ::read(client_fd, buf.data(), buf.size());
            if (n == 0) {
                break;
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(config_.command.client_timeout_ms));
                    continue;
                }
                break;
            }

            pending.append(buf.data(), static_cast<std::size_t>(n));
            std::size_t pos = 0;
            while (true) {
                const std::size_t eol = pending.find('\n', pos);
                if (eol == std::string::npos) {
                    pending = pending.substr(pos);
                    break;
                }

                const std::string line = trim(pending.substr(pos, eol - pos));
                pos = eol + 1;
                if (line.empty()) {
                    continue;
                }
                TrackCommandRequest req;
                std::string parse_err;
                if (!parseTrackCommand(line, req, parse_err)) {
                    // JSON command line for WiFi nodes.
                    if (line.find("\"type\"") == std::string::npos || line.find("\"command\"") == std::string::npos) {
                        const std::string resp = "ERR " + parse_err + "\n";
                        ::write(client_fd, resp.c_str(), resp.size());
                        continue;
                    }
                    const auto device_id = jsonGetInt(line, "device_id");
                    const auto command_id = jsonGetInt(line, "command_id");
                    const auto command_type = jsonGetString(line, "command_type");
                    const auto timeout_ms = jsonGetInt(line, "timeout_ms");
                    if (!device_id.has_value() || !command_id.has_value() || !command_type.has_value() ||
                        !timeout_ms.has_value()) {
                        const std::string resp = "ERR invalid json command\n";
                        ::write(client_fd, resp.c_str(), resp.size());
                        continue;
                    }
                    ControlCommandRequest validated_req;
                    std::string validate_err;
                    if (!parseControlCommandRequest(line, validated_req, validate_err)) {
                        const std::string resp = "ERR " + validate_err + "\n";
                        ::write(client_fd, resp.c_str(), resp.size());
                        continue;
                    }
                    if (otaDeviceInProgress(static_cast<std::uint8_t>(*device_id))) {
                        const std::string resp = "ERR device_in_ota\n";
                        ::write(client_fd, resp.c_str(), resp.size());
                        continue;
                    }
                    const std::string link_type = activeTransportFor(static_cast<std::uint8_t>(*device_id));
                    if (!isCommandSupportedByTransport(validated_req, link_type, validate_err)) {
                        Logger::instance().warn("[CMD] reject unsupported tracked json command device_id=" +
                                                std::to_string(*device_id) +
                                                " transport=" + link_type +
                                                " command_type=" + validated_req.command_type);
                        const std::string resp = "ERR " + validate_err + "\n";
                        ::write(client_fd, resp.c_str(), resp.size());
                        continue;
                    }
                    registerPendingCommand(static_cast<std::uint8_t>(*device_id),
                                           static_cast<std::uint16_t>(*command_id),
                                           *command_type);
                    Logger::instance().info("[CMD] routed device_id=" + std::to_string(*device_id) +
                                            " link_type=" + link_type +
                                            " command_id=" + std::to_string(*command_id) +
                                            " type=" + *command_type);
                    if (link_type == "wifi") {
                        if (!sendWifiCommandLine(static_cast<std::uint8_t>(*device_id), line + "\n")) {
                            std::lock_guard<std::mutex> lock(command_mutex_);
                            pending_commands_.erase(
                                makeCommandKey(static_cast<std::uint8_t>(*device_id), static_cast<std::uint16_t>(*command_id)));
                            const std::string resp = "ERR wifi device not connected\n";
                            ::write(client_fd, resp.c_str(), resp.size());
                            continue;
                        }
                    } else if (link_type == "mqtt") {
                        std::string mqtt_err;
                        const std::string mqtt_topic =
                            (*command_type == "ota_start") ? "ota/start" : "command/down";
                        if (!sendMqttDevicePayload(static_cast<std::uint8_t>(*device_id), mqtt_topic, line, mqtt_err)) {
                            std::lock_guard<std::mutex> lock(command_mutex_);
                            pending_commands_.erase(
                                makeCommandKey(static_cast<std::uint8_t>(*device_id), static_cast<std::uint16_t>(*command_id)));
                            const std::string resp = "ERR " + mqtt_err + "\n";
                            ::write(client_fd, resp.c_str(), resp.size());
                            continue;
                        }
                    } else if (link_type == "tcp_binary") {
                        std::lock_guard<std::mutex> lock(command_mutex_);
                        pending_commands_.erase(
                            makeCommandKey(static_cast<std::uint8_t>(*device_id), static_cast<std::uint16_t>(*command_id)));
                        const std::string resp = "ERR tcp_binary path requires TRACK_HEX/TEXT\n";
                        ::write(client_fd, resp.c_str(), resp.size());
                        continue;
                    } else {
                        std::lock_guard<std::mutex> lock(command_mutex_);
                        pending_commands_.erase(
                            makeCommandKey(static_cast<std::uint8_t>(*device_id), static_cast<std::uint16_t>(*command_id)));
                        const std::string resp = "ERR serial path requires TRACK_HEX/TEXT\n";
                        ::write(client_fd, resp.c_str(), resp.size());
                        continue;
                    }
                    std::uint8_t ack_result = 0xFF;
                    const bool acked = waitCommandAck(static_cast<std::uint8_t>(*device_id),
                                                      static_cast<std::uint16_t>(*command_id),
                                                      static_cast<int>(*timeout_ms),
                                                      ack_result);
                    if (acked) {
                        const std::string resp = "OK ack device_id=" + std::to_string(*device_id) +
                                                 " command_id=" + std::to_string(*command_id) +
                                                 " result=" + std::to_string(static_cast<int>(ack_result)) + "\n";
                        ::write(client_fd, resp.c_str(), resp.size());
                        Logger::instance().info("[CMD] tracked acked device_id=" + std::to_string(*device_id) +
                                                " command_id=" + std::to_string(*command_id) +
                                                " result=" + std::to_string(static_cast<int>(ack_result)));
                    } else {
                        const std::string resp = "ERR ack timeout device_id=" + std::to_string(*device_id) +
                                                 " command_id=" + std::to_string(*command_id) + "\n";
                        ::write(client_fd, resp.c_str(), resp.size());
                        Logger::instance().warn("[CMD] tracked timeout device_id=" + std::to_string(*device_id) +
                                                " command_id=" + std::to_string(*command_id));
                    }
                    continue;
                }

                const std::string active_transport = activeTransportFor(req.device_id);
                if (otaDeviceInProgress(req.device_id)) {
                    const std::string resp = "ERR device_in_ota\n";
                    ::write(client_fd, resp.c_str(), resp.size());
                    continue;
                }
                if (req.tracked) {
                    if (active_transport == "wifi") {
                        const std::string resp = "ERR wifi device requires JSON command\n";
                        ::write(client_fd, resp.c_str(), resp.size());
                        continue;
                    }
                    std::string validate_err;
                    ControlCommandRequest transport_req;
                    transport_req.device_id = req.device_id;
                    transport_req.command_id = req.command_id;
                    transport_req.has_command_id = true;
                    transport_req.command_type = req.command_type;
                    if (!isCommandSupportedByTransport(transport_req, active_transport, validate_err)) {
                        Logger::instance().warn("[CMD] reject unsupported tracked raw command device_id=" +
                                                std::to_string(static_cast<int>(req.device_id)) +
                                                " transport=" + active_transport +
                                                " command_type=" + req.command_type);
                        const std::string resp = "ERR " + validate_err + "\n";
                        ::write(client_fd, resp.c_str(), resp.size());
                        continue;
                    }
                    registerPendingCommand(req.device_id, req.command_id, req.command_type);
                    Logger::instance().info("instance=" + config_.serial.instance_name +
                                            " command tracked submit device_id=" +
                                            std::to_string(static_cast<int>(req.device_id)) +
                                            " command_id=" + std::to_string(req.command_id) +
                                            " command_type=" + req.command_type +
                                            " timeout_ms=" + std::to_string(req.timeout_ms));
                }

                ssize_t wn = 0;
                if (active_transport == "tcp_binary") {
                    if (!sendTcpBinaryFrame(req.device_id, req.payload)) {
                        const std::string resp = "ERR tcp_binary session not alive\n";
                        ::write(client_fd, resp.c_str(), resp.size());
                        if (req.tracked) {
                            std::lock_guard<std::mutex> lock(command_mutex_);
                            pending_commands_.erase(makeCommandKey(req.device_id, req.command_id));
                        }
                        continue;
                    }
                    wn = static_cast<ssize_t>(req.payload.size());
                } else {
                    if (!serial_available_) {
                        const std::string resp = "ERR serial transport unavailable\n";
                        ::write(client_fd, resp.c_str(), resp.size());
                        if (req.tracked) {
                            std::lock_guard<std::mutex> lock(command_mutex_);
                            pending_commands_.erase(makeCommandKey(req.device_id, req.command_id));
                        }
                        continue;
                    }
                    std::string err;
                    wn = serial_.write(req.payload.data(), req.payload.size(), err);
                    if (wn < 0) {
                        const std::string resp = "ERR " + err + "\n";
                        ::write(client_fd, resp.c_str(), resp.size());
                        if (req.tracked) {
                            std::lock_guard<std::mutex> lock(command_mutex_);
                            pending_commands_.erase(makeCommandKey(req.device_id, req.command_id));
                        }
                        continue;
                    }
                }

                if (!req.tracked) {
                    const std::string resp = "OK wrote=" + std::to_string(wn) + "\n";
                    ::write(client_fd, resp.c_str(), resp.size());
                    Logger::instance().info("instance=" + config_.serial.instance_name +
                                            " command write bytes=" + std::to_string(wn));
                    continue;
                }

                std::uint8_t ack_result = 0xFF;
                const bool acked = waitCommandAck(req.device_id, req.command_id, req.timeout_ms, ack_result);
                if (acked) {
                    const std::string resp = "OK ack device_id=" +
                                             std::to_string(static_cast<int>(req.device_id)) +
                                             " command_id=" + std::to_string(req.command_id) +
                                             " result=" + std::to_string(static_cast<int>(ack_result)) +
                                             " wrote=" + std::to_string(wn) + "\n";
                    ::write(client_fd, resp.c_str(), resp.size());
                    Logger::instance().info("instance=" + config_.serial.instance_name +
                                            " command tracked acked device_id=" +
                                            std::to_string(static_cast<int>(req.device_id)) +
                                            " command_id=" + std::to_string(req.command_id) +
                                            " result=" + std::to_string(static_cast<int>(ack_result)));
                } else {
                    const std::string resp = "ERR ack timeout device_id=" +
                                             std::to_string(static_cast<int>(req.device_id)) +
                                             " command_id=" + std::to_string(req.command_id) + "\n";
                    ::write(client_fd, resp.c_str(), resp.size());
                    Logger::instance().warn("instance=" + config_.serial.instance_name +
                                            " command tracked timeout device_id=" +
                                            std::to_string(static_cast<int>(req.device_id)) +
                                            " command_id=" + std::to_string(req.command_id));
                }
            }
        }

        ::close(client_fd);
    }

    ::close(listen_fd);
}

bool AppController::sendWifiCommandLine(std::uint8_t device_id, const std::string& line) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(wifi_mutex_);
        const auto it = wifi_device_fds_.find(device_id);
        if (it == wifi_device_fds_.end()) {
            return false;
        }
        fd = ::dup(it->second);
    }
    if (fd < 0) {
        Logger::instance().warn("[WIFI] dup fd failed device_id=" +
                                std::to_string(static_cast<int>(device_id)) +
                                " err=" + std::string(std::strerror(errno)));
        return false;
    }
    const bool ok = writeAll(fd, line);
    ::close(fd);
    return ok;
}

bool AppController::sendTcpBinaryFrame(std::uint8_t device_id, const std::vector<std::uint8_t>& frame) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(wifi_mutex_);
        const auto it = tcp_binary_device_fds_.find(device_id);
        if (it == tcp_binary_device_fds_.end()) {
            return false;
        }
        fd = ::dup(it->second);
    }
    if (fd < 0) {
        Logger::instance().warn("[TCPB] dup fd failed device_id=" +
                                std::to_string(static_cast<int>(device_id)) +
                                " err=" + std::string(std::strerror(errno)));
        return false;
    }
    std::size_t off = 0;
    while (off < frame.size()) {
        const ssize_t n = ::write(fd, frame.data() + off, frame.size() - off);
        if (n > 0) {
            off += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        ::close(fd);
        return false;
    }
    ::close(fd);
    return true;
}

bool AppController::sendMqttDevicePayload(std::uint8_t device_id,
                                          const std::string& subtopic,
                                          const std::string& payload,
                                          std::string& err) {
#ifndef SG_HAVE_MOSQUITTO
    (void)device_id;
    (void)subtopic;
    (void)payload;
    err = "mqtt device ingress not available";
    return false;
#else
    std::lock_guard<std::mutex> lock(mqtt_device_mutex_);
    if (mqtt_device_mosq_ == nullptr || !mqtt_device_connected_) {
        err = "mqtt device ingress not connected";
        return false;
    }
    std::ostringstream topic;
    topic << config_.mqtt_device_ingress.topic_prefix
          << "/" << config_.mqtt_device_ingress.gateway_id
          << "/device/" << static_cast<int>(device_id)
          << "/" << subtopic;
    const int rc = mosquitto_publish(static_cast<mosquitto*>(mqtt_device_mosq_),
                                     nullptr,
                                     topic.str().c_str(),
                                     static_cast<int>(payload.size()),
                                     payload.data(),
                                     0,
                                     false);
    if (rc != MOSQ_ERR_SUCCESS) {
        err = "mosquitto_publish failed rc=" + std::to_string(rc);
        return false;
    }
    return true;
#endif
}

void AppController::handleMqttIngressConnect(int rc) {
#ifndef SG_HAVE_MOSQUITTO
    (void)rc;
#else
    {
        std::lock_guard<std::mutex> lock(mqtt_device_mutex_);
        mqtt_device_connected_ = (rc == MOSQ_ERR_SUCCESS);
    }
    if (rc != MOSQ_ERR_SUCCESS) {
        Logger::instance().warn("mqtt device ingress connect failed rc=" + std::to_string(rc));
        return;
    }

    const std::string base = config_.mqtt_device_ingress.topic_prefix + "/" +
                             config_.mqtt_device_ingress.gateway_id + "/device/+/";
    std::lock_guard<std::mutex> lock(mqtt_device_mutex_);
    mosquitto_subscribe(static_cast<mosquitto*>(mqtt_device_mosq_), nullptr, (base + "telemetry").c_str(), 0);
    mosquitto_subscribe(static_cast<mosquitto*>(mqtt_device_mosq_), nullptr, (base + "status").c_str(), 0);
    mosquitto_subscribe(static_cast<mosquitto*>(mqtt_device_mosq_), nullptr, (base + "event").c_str(), 0);
    mosquitto_subscribe(static_cast<mosquitto*>(mqtt_device_mosq_), nullptr, (base + "command/ack").c_str(), 0);
    mosquitto_subscribe(static_cast<mosquitto*>(mqtt_device_mosq_), nullptr, (base + "ota/status").c_str(), 0);
    Logger::instance().info("mqtt device ingress connected and subscribed");
#endif
}

void AppController::handleMqttIngressDisconnect(int rc) {
#ifndef SG_HAVE_MOSQUITTO
    (void)rc;
#else
    std::lock_guard<std::mutex> lock(mqtt_device_mutex_);
    mqtt_device_connected_ = false;
    Logger::instance().warn("mqtt device ingress disconnected rc=" + std::to_string(rc));
#endif
}

void AppController::handleMqttIngressMessage(const std::string& topic, const std::string& payload) {
#ifndef SG_HAVE_MOSQUITTO
    (void)topic;
    (void)payload;
#else
    const auto parts = parseMqttDeviceTopic(topic);
    if (!parts.valid) {
        return;
    }

    if (parts.category == "ota" && parts.action == "status") {
        if (auto ota_st = ota::Esp32OtaAdapter::parseOtaStatusJson(payload); ota_st.has_value()) {
            Logger::instance().info("mqtt ota status rx device_id=" +
                                    std::to_string(static_cast<int>(ota_st->device_id)) +
                                    " command_id=" + std::to_string(ota_st->command_id) +
                                    " state=" + ota_st->ota_state);
            publishEsp32OtaStatus(*ota_st);
            (void)publishUpstreamOtaStatus(*ota_st);
        }
        bindActiveTransport(parts.device_id, "mqtt", transportPriority("mqtt"));
        device_registry_.markTransportOnline(parts.device_id, "mqtt", unixMsNow(), "", "mqtt ota status");
        refreshDeviceMetrics();
        return;
    }

    const auto presence = jsonGetString(payload, "status");
    const auto event_type = jsonGetString(payload, "event_type");
    const auto device_name = jsonGetString(payload, "device_name").value_or("");

    if (!event_type.has_value() && presence.has_value()) {
        if (*presence == "offline") {
            device_registry_.markTransportDisconnected(parts.device_id, "mqtt", unixMsNow(), "mqtt offline");
            clearActiveTransport(parts.device_id, "mqtt_offline");
            refreshDeviceMetrics();
            Logger::instance().warn("mqtt device offline device_id=" + std::to_string(static_cast<int>(parts.device_id)));
            return;
        }
        if (*presence == "online") {
            bindActiveTransport(parts.device_id, "mqtt", transportPriority("mqtt"));
            const auto update = device_registry_.markTransportOnline(parts.device_id, "mqtt", unixMsNow(), device_name, "presence=online");
            if (update.became_online) {
                Logger::instance().info("mqtt device online device_id=" + std::to_string(static_cast<int>(parts.device_id)));
            }
            refreshDeviceMetrics();
            return;
        }
    }

    SensorData data;
    std::string err;
    if (!parseWifiJsonEvent(payload, data, err)) {
        Logger::instance().warn("mqtt device parse failed topic=" + topic + " err=" + err);
        return;
    }
    data.link_type = "mqtt";
    bindActiveTransport(data.device_id, "mqtt", transportPriority("mqtt"));
    if (!data_queue_.push(std::move(data))) {
        Logger::instance().warn("mqtt device data_queue push failed");
    }
#endif
}

void AppController::handleUpstreamControlConnect(int rc) {
#ifndef SG_HAVE_MOSQUITTO
    (void)rc;
#else
    {
        std::lock_guard<std::mutex> lock(upstream_control_mutex_);
        upstream_control_connected_ = (rc == MOSQ_ERR_SUCCESS);
    }
    if (rc != MOSQ_ERR_SUCCESS) {
        Logger::instance().warn("upstream mqtt control connect failed rc=" + std::to_string(rc));
        return;
    }
    const std::string cmd_topic =
        resolveUpstreamControlTopic(config_.uploader, config_.uploader.mqtt_command_down_topic, "/command/down");
    const std::string ota_topic =
        resolveUpstreamControlTopic(config_.uploader, config_.uploader.mqtt_ota_start_topic, "/ota/start");
    mosquitto_subscribe(static_cast<mosquitto*>(upstream_control_mosq_), nullptr, cmd_topic.c_str(), 0);
    mosquitto_subscribe(static_cast<mosquitto*>(upstream_control_mosq_), nullptr, ota_topic.c_str(), 0);
    Logger::instance().info("upstream mqtt control subscribed command_topic=" + cmd_topic +
                            " ota_topic=" + ota_topic);
#endif
}

void AppController::handleUpstreamControlDisconnect(int rc) {
#ifndef SG_HAVE_MOSQUITTO
    (void)rc;
#else
    std::lock_guard<std::mutex> lock(upstream_control_mutex_);
    upstream_control_connected_ = false;
    Logger::instance().warn("upstream mqtt control disconnected rc=" + std::to_string(rc));
#endif
}

void AppController::handleUpstreamControlMessage(const std::string& topic, const std::string& payload) {
    const std::string cmd_topic =
        resolveUpstreamControlTopic(config_.uploader, config_.uploader.mqtt_command_down_topic, "/command/down");
    const std::string ota_topic =
        resolveUpstreamControlTopic(config_.uploader, config_.uploader.mqtt_ota_start_topic, "/ota/start");
    std::string http_status;
    std::string response_body;
    if (topic == cmd_topic) {
        handleControlCommandRequestJson(payload, http_status, response_body);
        Logger::instance().info("upstream mqtt control handled topic=" + topic +
                                " status=" + http_status +
                                " response=" + response_body);
        return;
    }
    if (topic == ota_topic) {
        handleOtaTaskRequestJson(payload, http_status, response_body);
        Logger::instance().info("upstream mqtt ota handled topic=" + topic +
                                " status=" + http_status +
                                " response=" + response_body);
    }
}

void AppController::bindActiveTransport(std::uint8_t device_id, const std::string& transport, int priority) {
    std::string locked_transport;
    if (otaTransportLocked(device_id, &locked_transport) && locked_transport != transport) {
        Logger::instance().info("[OTA][DROP] non-ota frame dropped type=0x00 device=" +
                                std::to_string(static_cast<int>(device_id)) +
                                " transport=" + transport + " reason=transport_locked");
        return;
    }
    std::lock_guard<std::mutex> lock(transport_mutex_);
    const auto pit = active_transport_priority_.find(device_id);
    if (pit != active_transport_priority_.end() && pit->second > priority) {
        Logger::instance().warn("[DEVICE] duplicate transport ignored device=" +
                                std::to_string(static_cast<int>(device_id)) +
                                " active=" + active_transport_[device_id] +
                                " incoming=" + transport);
        return;
    }
    const auto old_it = active_transport_.find(device_id);
    if (old_it == active_transport_.end() || old_it->second != transport) {
        if (old_it == active_transport_.end()) {
            Logger::instance().info("[DEVICE] active transport device=" +
                                    std::to_string(static_cast<int>(device_id)) +
                                    " transport=" + transport);
        } else {
            Logger::instance().info("[DEVICE] transport switched device=" +
                                    std::to_string(static_cast<int>(device_id)) +
                                    " from=" + old_it->second + " to=" + transport +
                                    " reason=priority");
        }
    }
    active_transport_[device_id] = transport;
    active_transport_priority_[device_id] = priority;
}

std::string AppController::activeTransportFor(std::uint8_t device_id) {
    std::lock_guard<std::mutex> lock(transport_mutex_);
    const auto it = active_transport_.find(device_id);
    if (it == active_transport_.end()) {
        return "serial";
    }
    return it->second;
}

int AppController::transportPriority(const std::string& transport) const {
    if (transport == "tcp_binary") return config_.tcp_binary.priority;
    if (transport == "mqtt") return 90;
    if (transport == "wifi") return 80;
    return 50;
}

bool AppController::shouldAcceptTransportData(std::uint8_t device_id, const std::string& incoming_transport) {
    std::string locked_transport;
    if (otaTransportLocked(device_id, &locked_transport) && locked_transport != incoming_transport) {
        Logger::instance().warn("[OTA][LOCK] transport mismatch drop device=" +
                                std::to_string(static_cast<int>(device_id)) +
                                " locked=" + locked_transport +
                                " incoming=" + incoming_transport);
        return false;
    }
    std::lock_guard<std::mutex> lock(transport_mutex_);
    const auto it = active_transport_.find(device_id);
    if (it == active_transport_.end()) {
        return true;
    }
    const int incoming_p = transportPriority(incoming_transport);
    const int active_p = transportPriority(it->second);
    if (incoming_p < active_p && it->second != incoming_transport) {
        Logger::instance().warn("[DEVICE] duplicate transport ignored device=" +
                                std::to_string(static_cast<int>(device_id)) +
                                " active=" + it->second +
                                " incoming=" + incoming_transport);
        return false;
    }
    return true;
}

void AppController::clearActiveTransport(std::uint8_t device_id, const std::string& reason) {
    if (otaTransportLocked(device_id, nullptr)) {
        Logger::instance().warn("[OTA][LOCK] keep transport lock on disconnect device=" +
                                std::to_string(static_cast<int>(device_id)) +
                                " reason=" + reason);
        return;
    }
    std::lock_guard<std::mutex> lock(transport_mutex_);
    auto it = active_transport_.find(device_id);
    if (it == active_transport_.end()) {
        return;
    }
    if (it->second == "tcp_binary") {
        Logger::instance().warn("[DEVICE] transport switched device=" +
                                std::to_string(static_cast<int>(device_id)) +
                                " from=tcp_binary to=serial reason=" + reason);
    }
    active_transport_.erase(it);
    active_transport_priority_.erase(device_id);
}

void AppController::wifiDeviceServerLoop() {
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        Logger::instance().error("wifi server socket create failed");
        return;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(config_.wifi_device_server.listen_port));
    if (inet_pton(AF_INET, config_.wifi_device_server.listen_host.c_str(), &addr.sin_addr) != 1) {
        Logger::instance().error("wifi bind_host invalid: " + config_.wifi_device_server.listen_host);
        ::close(listen_fd);
        return;
    }
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        Logger::instance().error("wifi bind failed: " + std::string(std::strerror(errno)));
        ::close(listen_fd);
        return;
    }
    if (listen(listen_fd, 8) != 0) {
        Logger::instance().error("wifi listen failed");
        ::close(listen_fd);
        return;
    }
    const int flags = fcntl(listen_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
    }

    struct Session {
        int fd = -1;
        std::string remote_ip;
        std::string pending;
        std::uint8_t device_id = 0;
        bool has_device_id = false;
    };
    std::unordered_map<int, Session> sessions;
    const auto closeSession = [&](int fd, const char* reason, bool mark_replace) {
        auto it = sessions.find(fd);
        if (it == sessions.end()) {
            return;
        }
        Session& s = it->second;
        if (s.has_device_id) {
            {
                std::lock_guard<std::mutex> lock(wifi_mutex_);
                const auto fit = wifi_device_fds_.find(s.device_id);
                if (fit != wifi_device_fds_.end() && fit->second == s.fd) {
                    wifi_device_fds_.erase(fit);
                }
                wifi_clients_connected_ = wifi_device_fds_.size();
            }
            device_registry_.markWifiDisconnected(s.device_id, unixMsNow(), reason);
            Logger::instance().warn("[WIFI] client disconnected device_id=" +
                                    std::to_string(static_cast<int>(s.device_id)));
            Logger::instance().warn("[WIFI] device offline device_id=" +
                                    std::to_string(static_cast<int>(s.device_id)) +
                                    " reason=" + reason);
        } else {
            std::lock_guard<std::mutex> lock(wifi_mutex_);
            wifi_clients_connected_ = sessions.size() > 0 ? sessions.size() - 1 : 0;
        }
        if (mark_replace) {
            device_registry_.markWifiDisconnected(s.device_id, unixMsNow(), "tcp_replaced");
        }
        ::close(s.fd);
        sessions.erase(it);
        refreshDeviceMetrics();
    };

    const int epfd = ::epoll_create1(0);
    if (epfd < 0) {
        Logger::instance().error("wifi epoll create failed");
        ::close(listen_fd);
        return;
    }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    if (::epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) != 0) {
        Logger::instance().error("wifi epoll add listen fd failed");
        ::close(epfd);
        ::close(listen_fd);
        return;
    }
    std::array<epoll_event, 64> events{};
    Logger::instance().info("wifi device server listening on " + config_.wifi_device_server.listen_host + ":" +
                            std::to_string(config_.wifi_device_server.listen_port));

    while (running_.load()) {
        const int nfds = ::epoll_wait(epfd, events.data(), static_cast<int>(events.size()), 100);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            continue;
        }
        for (int eidx = 0; eidx < nfds; ++eidx) {
            const epoll_event& e = events[static_cast<std::size_t>(eidx)];
            if (e.data.fd == listen_fd) {
                while (running_.load()) {
                    sockaddr_in client_addr{};
                    socklen_t addr_len = sizeof(client_addr);
                    const int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        break;
                    }
                    const int cflags = fcntl(client_fd, F_GETFL, 0);
                    if (cflags >= 0) fcntl(client_fd, F_SETFL, cflags | O_NONBLOCK);
                    char ip[INET_ADDRSTRLEN] = {0};
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
                    sessions.emplace(client_fd, Session{client_fd, ip, "", 0, false});
                    epoll_event cev{};
                    cev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
                    cev.data.fd = client_fd;
                    if (::epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev) != 0) {
                        ::close(client_fd);
                        sessions.erase(client_fd);
                        continue;
                    }
                    {
                        std::lock_guard<std::mutex> lock(wifi_mutex_);
                        wifi_clients_connected_ = sessions.size();
                    }
                    refreshDeviceMetrics();
                    Logger::instance().info(std::string("[WIFI] client connected remote=") + ip);
                }
                continue;
            }

            auto sit = sessions.find(e.data.fd);
            if (sit == sessions.end()) {
                continue;
            }
            Session& s = sit->second;
            if ((e.events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) != 0) {
                closeSession(s.fd, "tcp_disconnect", false);
                continue;
            }

            std::array<char, 1024> buf{};
            while (running_.load()) {
                const ssize_t n = ::read(s.fd, buf.data(), buf.size());
                if (n == 0) {
                    closeSession(s.fd, "tcp_disconnect", false);
                    break;
                }
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    closeSession(s.fd, "tcp_disconnect", false);
                    break;
                }
                s.pending.append(buf.data(), static_cast<std::size_t>(n));
                std::size_t pos = 0;
                while (true) {
                    const std::size_t eol = s.pending.find('\n', pos);
                    if (eol == std::string::npos) {
                        s.pending = s.pending.substr(pos);
                        break;
                    }
                    const std::string line = trim(s.pending.substr(pos, eol - pos));
                    pos = eol + 1;
                    if (line.empty()) continue;
                    if (auto ota_st = ota::Esp32OtaAdapter::parseOtaStatusJson(line); ota_st.has_value()) {
                        Logger::instance().info("esp32 ota status rx device_id=" +
                                                std::to_string(static_cast<int>(ota_st->device_id)) +
                                                " command_id=" + std::to_string(ota_st->command_id) +
                                                " state=" + ota_st->ota_state +
                                                " progress=" + std::to_string(ota_st->progress) +
                                                " err=" + std::to_string(ota_st->error_code));
                        publishEsp32OtaStatus(*ota_st);
                        (void)publishUpstreamOtaStatus(*ota_st);
                        continue;
                    }
                    if (line.find("ota_status") != std::string::npos) {
                        Logger::instance().warn("esp32 ota status parse failed raw=" + line);
                        continue;
                    }
                    SensorData data;
                    std::string err;
                    if (!parseWifiJsonEvent(line, data, err)) {
                        stats_.addUnknownTypeFrames(1);
                        stats_.addWifiJsonParseFail(1);
                        Logger::instance().warn("wifi json parse failed: " + err + " line=" + line);
                        continue;
                    }
                    stats_.addWifiEventsReceived(1);
                    stats_.addWifiJsonParseOk(1);
                    const auto it = device_map_.find(data.device_id);
                    if (it == device_map_.end()) {
                        stats_.addWifiUnknownDevice(1);
                        Logger::instance().warn("drop unknown wifi device_id=" +
                                                std::to_string(static_cast<int>(data.device_id)));
                        device_registry_.markDeviceError(data.device_id, "unknown wifi device", data.timestamp_unix_ms);
                        continue;
                    }
                    s.device_id = data.device_id;
                    s.has_device_id = true;
                    int replaced_old_fd = -1;
                    {
                        std::lock_guard<std::mutex> lock(wifi_mutex_);
                        const auto old_it = wifi_device_fds_.find(data.device_id);
                        if (old_it != wifi_device_fds_.end() && old_it->second != s.fd) {
                            replaced_old_fd = old_it->second;
                            wifi_device_fds_.erase(old_it);
                        }
                        wifi_device_fds_[data.device_id] = s.fd;
                        wifi_clients_connected_ = wifi_device_fds_.size();
                    }
                    if (replaced_old_fd >= 0) {
                        closeSession(replaced_old_fd, "tcp_replaced", true);
                    }
                    refreshDeviceMetrics();
                    if (!data_queue_.push(std::move(data))) {
                        ::close(epfd);
                        ::close(listen_fd);
                        return;
                    }
                }
            }
        }
    }

    for (auto& kv : sessions) {
        ::close(kv.second.fd);
    }
    ::close(epfd);
    ::close(listen_fd);
}

void AppController::mqttDeviceLoop() {
#ifndef SG_HAVE_MOSQUITTO
    Logger::instance().warn("mqtt device ingress requested but libmosquitto is not linked");
    return;
#else
    if (!mqttRuntimeAcquire()) {
        Logger::instance().error("mqtt device ingress init failed: mosquitto runtime acquire failed");
        return;
    }
    const std::string client_id =
        config_.mqtt_device_ingress.client_id.empty() ? "serial-gateway-device-ingress"
                                                      : config_.mqtt_device_ingress.client_id;
    mosquitto* mosq = mosquitto_new(client_id.c_str(), config_.mqtt_device_ingress.clean_session, this);
    if (mosq == nullptr) {
        Logger::instance().error("mqtt device ingress init failed");
        mqttRuntimeRelease();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mqtt_device_mutex_);
        mqtt_device_mosq_ = mosq;
        mqtt_device_connected_ = false;
    }

    mosquitto_connect_callback_set(mosq, mqttIngressOnConnect);
    mosquitto_disconnect_callback_set(mosq, mqttIngressOnDisconnect);
    mosquitto_message_callback_set(mosq, mqttIngressOnMessage);
    if (!config_.mqtt_device_ingress.username.empty()) {
        mosquitto_username_pw_set(
            mosq,
            config_.mqtt_device_ingress.username.c_str(),
            config_.mqtt_device_ingress.password.empty() ? nullptr : config_.mqtt_device_ingress.password.c_str());
    }
    mosquitto_reconnect_delay_set(
        mosq,
        std::max(1, config_.mqtt_device_ingress.reconnect_initial_ms / 1000),
        std::max(1, config_.mqtt_device_ingress.reconnect_max_ms / 1000),
        true);

    const int rc = mosquitto_connect_async(
        mosq,
        config_.mqtt_device_ingress.host.c_str(),
        config_.mqtt_device_ingress.port,
        config_.mqtt_device_ingress.keepalive_sec);
    if (rc != MOSQ_ERR_SUCCESS) {
        Logger::instance().error("mqtt device ingress connect_async failed rc=" + std::to_string(rc));
        std::lock_guard<std::mutex> lock(mqtt_device_mutex_);
        mosquitto_destroy(mosq);
        mqtt_device_mosq_ = nullptr;
        mqtt_device_connected_ = false;
        mqttRuntimeRelease();
        return;
    }

    const int loop_rc = mosquitto_loop_start(mosq);
    if (loop_rc != MOSQ_ERR_SUCCESS) {
        Logger::instance().error("mqtt device ingress loop_start failed rc=" + std::to_string(loop_rc));
        std::lock_guard<std::mutex> lock(mqtt_device_mutex_);
        mosquitto_destroy(mosq);
        mqtt_device_mosq_ = nullptr;
        mqtt_device_connected_ = false;
        mqttRuntimeRelease();
        return;
    }

    Logger::instance().info(
        "mqtt device ingress listening host=" + config_.mqtt_device_ingress.host +
        " port=" + std::to_string(config_.mqtt_device_ingress.port) +
        " gateway_id=" + config_.mqtt_device_ingress.gateway_id);

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    mosquitto_loop_stop(mosq, true);
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    {
        std::lock_guard<std::mutex> lock(mqtt_device_mutex_);
        mqtt_device_mosq_ = nullptr;
        mqtt_device_connected_ = false;
    }
    mqttRuntimeRelease();
#endif
}

void AppController::upstreamControlLoop() {
#ifndef SG_HAVE_MOSQUITTO
    Logger::instance().warn("upstream mqtt control requested but libmosquitto is not linked");
    return;
#else
    if (!mqttRuntimeAcquire()) {
        Logger::instance().error("upstream mqtt control init failed: mosquitto runtime acquire failed");
        return;
    }
    const std::string client_id =
        config_.uploader.mqtt_control_client_id.empty() ? "serial-gateway-upstream-control"
                                                        : config_.uploader.mqtt_control_client_id;
    mosquitto* mosq = mosquitto_new(client_id.c_str(), config_.uploader.mqtt_clean_session, this);
    if (mosq == nullptr) {
        Logger::instance().error("upstream mqtt control init failed");
        mqttRuntimeRelease();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(upstream_control_mutex_);
        upstream_control_mosq_ = mosq;
        upstream_control_connected_ = false;
    }
    mosquitto_connect_callback_set(mosq, upstreamControlOnConnect);
    mosquitto_disconnect_callback_set(mosq, upstreamControlOnDisconnect);
    mosquitto_message_callback_set(mosq, upstreamControlOnMessage);
    if (!config_.uploader.mqtt_username.empty()) {
        mosquitto_username_pw_set(
            mosq,
            config_.uploader.mqtt_username.c_str(),
            config_.uploader.mqtt_password.empty() ? nullptr : config_.uploader.mqtt_password.c_str());
    }
    mosquitto_reconnect_delay_set(
        mosq,
        std::max(1, config_.uploader.reconnect_initial_ms / 1000),
        std::max(1, config_.uploader.reconnect_max_ms / 1000),
        true);

    const int rc = mosquitto_connect_async(
        mosq,
        config_.uploader.host.c_str(),
        config_.uploader.port,
        config_.uploader.mqtt_keepalive_sec);
    if (rc != MOSQ_ERR_SUCCESS) {
        Logger::instance().error("upstream mqtt control connect_async failed rc=" + std::to_string(rc));
        std::lock_guard<std::mutex> lock(upstream_control_mutex_);
        mosquitto_destroy(mosq);
        upstream_control_mosq_ = nullptr;
        upstream_control_connected_ = false;
        mqttRuntimeRelease();
        return;
    }

    const int loop_rc = mosquitto_loop_start(mosq);
    if (loop_rc != MOSQ_ERR_SUCCESS) {
        Logger::instance().error("upstream mqtt control loop_start failed rc=" + std::to_string(loop_rc));
        std::lock_guard<std::mutex> lock(upstream_control_mutex_);
        mosquitto_destroy(mosq);
        upstream_control_mosq_ = nullptr;
        upstream_control_connected_ = false;
        mqttRuntimeRelease();
        return;
    }

    Logger::instance().info("upstream mqtt control listening host=" + config_.uploader.host +
                            " port=" + std::to_string(config_.uploader.port));
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    mosquitto_loop_stop(mosq, true);
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    {
        std::lock_guard<std::mutex> lock(upstream_control_mutex_);
        upstream_control_mosq_ = nullptr;
        upstream_control_connected_ = false;
    }
    mqttRuntimeRelease();
#endif
}

void AppController::tcpBinaryLoop() {
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        Logger::instance().error("tcp_binary socket create failed");
        return;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(config_.tcp_binary.port));
    if (inet_pton(AF_INET, config_.tcp_binary.listen_host.c_str(), &addr.sin_addr) != 1) {
        Logger::instance().error("tcp_binary listen_host invalid: " + config_.tcp_binary.listen_host);
        ::close(listen_fd);
        return;
    }
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listen_fd, 8) != 0) {
        Logger::instance().error("tcp_binary bind/listen failed: " + std::string(std::strerror(errno)));
        ::close(listen_fd);
        return;
    }
    const int flags = fcntl(listen_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
    Logger::instance().info("tcp_binary listening on " + config_.tcp_binary.listen_host + ":" +
                            std::to_string(config_.tcp_binary.port));

    struct Session {
        int fd = -1;
        std::string pending;
        FrameParser parser;
        std::uint64_t last_hb_ms = 0;
        std::uint8_t last_device = 0;
        bool has_device = false;
    };
    std::unordered_map<int, Session> sessions;
    const int epfd = ::epoll_create1(0);
    if (epfd < 0) {
        ::close(listen_fd);
        return;
    }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    if (::epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) != 0) {
        ::close(epfd);
        ::close(listen_fd);
        return;
    }
    std::array<epoll_event, 32> events{};
    while (running_.load()) {
        const int nfds = ::epoll_wait(epfd, events.data(), static_cast<int>(events.size()), 100);
        const std::uint64_t now_ms = unixMsNow();
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (it->second.last_hb_ms > 0 &&
                now_ms > it->second.last_hb_ms + static_cast<std::uint64_t>(config_.tcp_binary.heartbeat_timeout_ms)) {
                if (it->second.has_device) {
                    std::string locked_transport;
                    if (otaTransportLocked(it->second.last_device, &locked_transport) &&
                        locked_transport == "tcp_binary") {
                        Logger::instance().warn("[OTA][LOCK] skip tcp_timeout reclaim during ota device=" +
                                                std::to_string(static_cast<int>(it->second.last_device)));
                        ++it;
                        continue;
                    }
                }
                if (it->second.has_device) {
                    std::lock_guard<std::mutex> lock(wifi_mutex_);
                    tcp_binary_device_fds_.erase(it->second.last_device);
                }
                if (it->second.has_device) {
                    clearActiveTransport(it->second.last_device, "tcp_timeout");
                }
                ::close(it->second.fd);
                it = sessions.erase(it);
                continue;
            }
            ++it;
        }
        if (nfds <= 0) {
            continue;
        }
        for (int i = 0; i < nfds; ++i) {
            const int fd = events[static_cast<std::size_t>(i)].data.fd;
            if (fd == listen_fd) {
                sockaddr_in client_addr{};
                socklen_t addr_len = sizeof(client_addr);
                const int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
                if (client_fd >= 0) {
                    const int cflags = fcntl(client_fd, F_GETFL, 0);
                    if (cflags >= 0) fcntl(client_fd, F_SETFL, cflags | O_NONBLOCK);
                    epoll_event cev{};
                    cev.events = EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLRDHUP;
                    cev.data.fd = client_fd;
                    if (::epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev) == 0) {
                        sessions.emplace(client_fd, Session{client_fd, "", {}, now_ms, 0, false});
                    } else {
                        ::close(client_fd);
                    }
                }
                continue;
            }
            auto sit = sessions.find(fd);
            if (sit == sessions.end()) continue;
            if ((events[static_cast<std::size_t>(i)].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) != 0) {
                if (sit->second.has_device) {
                    std::lock_guard<std::mutex> lock(wifi_mutex_);
                    tcp_binary_device_fds_.erase(sit->second.last_device);
                    clearActiveTransport(sit->second.last_device, "tcp_disconnect");
                }
                ::close(fd);
                sessions.erase(sit);
                continue;
            }
            std::array<std::uint8_t, 1024> buf{};
            const ssize_t n = ::read(fd, buf.data(), buf.size());
            if (n <= 0) {
                continue;
            }
            Session& s = sit->second;
            s.pending.append(reinterpret_cast<const char*>(buf.data()), static_cast<std::size_t>(n));
            while (s.pending.size() >= 6) {
                if (static_cast<std::uint8_t>(s.pending[0]) != 0xAA || static_cast<std::uint8_t>(s.pending[1]) != 0x55) {
                    s.pending.erase(0, 1);
                    continue;
                }
                const std::uint8_t len = static_cast<std::uint8_t>(s.pending[2]);
                const std::size_t total = 2 + 1 + static_cast<std::size_t>(len) + 2;
                if (s.pending.size() < total) break;
                std::vector<std::uint8_t> frame(total, 0);
                for (std::size_t k = 0; k < total; ++k) frame[k] = static_cast<std::uint8_t>(s.pending[k]);
                const std::uint16_t got_crc = static_cast<std::uint16_t>(frame[total - 2]) |
                                              (static_cast<std::uint16_t>(frame[total - 1]) << 8U);
                const std::uint16_t calc_crc = crc16_modbus(frame.data() + 2, 1 + len);
                if (got_crc != calc_crc) {
                    Logger::instance().warn("tcp_binary crc error");
                    s.pending.erase(0, 1);
                    continue;
                }
                const std::uint8_t type = frame[3];
                if (type == 0x7E) {
                    s.last_hb_ms = now_ms;
                    s.pending.erase(0, total);
                    continue;
                }
                if (type == kTypeAdapterControl) {
                    s.last_hb_ms = now_ms;
                    s.pending.erase(0, total);
                    continue;
                }
                if (isBootloaderUplinkType(type)) {
                    std::uint8_t ota_device = 0;
                    bool have_ota_device = s.has_device;
                    if (have_ota_device) {
                        ota_device = s.last_device;
                    } else {
                        std::lock_guard<std::mutex> lock(wifi_mutex_);
                        for (const auto& kv : tcp_binary_device_fds_) {
                            if (kv.second == s.fd) {
                                ota_device = kv.first;
                                have_ota_device = true;
                                break;
                            }
                        }
                    }
                    if (have_ota_device) {
                        s.last_hb_ms = now_ms;
                        s.last_device = ota_device;
                        s.has_device = true;
                        publishTcpBinaryOtaFrame(ota_device, frame);
                        s.pending.erase(0, total);
                        continue;
                    }
                    Logger::instance().warn("tcp_binary ota uplink dropped: unknown device binding");
                }
                if (s.has_device && otaBusinessMuted(s.last_device)) {
                    char drop_hex[5]{};
                    std::snprintf(drop_hex, sizeof(drop_hex), "%02X", static_cast<unsigned int>(type));
                    Logger::instance().info("[OTA][DROP] non-ota frame dropped type=0x" +
                                            std::string(drop_hex) + " device=" +
                                            std::to_string(static_cast<int>(s.last_device)) +
                                            " transport=tcp_binary");
                    s.pending.erase(0, total);
                    continue;
                }
                s.last_hb_ms = now_ms;
                s.parser.append(frame.data(), frame.size());
                ParseCounters counters{};
                auto items = s.parser.extract(counters);
                for (auto& data : items) {
                    if (otaBusinessMuted(data.device_id)) {
                        char drop_hex[5]{};
                        std::snprintf(drop_hex, sizeof(drop_hex), "%02X",
                                      static_cast<unsigned int>(frame[3]));
                        Logger::instance().info("[OTA][DROP] non-ota frame dropped type=0x" +
                                                std::string(drop_hex) + " device=" +
                                                std::to_string(static_cast<int>(data.device_id)) +
                                                " transport=tcp_binary");
                        continue;
                    }
                    data.link_type = "tcp_binary";
                    s.last_device = data.device_id;
                    s.has_device = true;
                    if (data.frame_type == FrameType::SENSOR_DATA && data.seq > 0) {
                        const auto now_ms2 = unixMsNowLocal();
                        ackDebugMarkRx(now_ms2);
                        Logger::instance().info("rx report seq=" + std::to_string(data.seq));
                        const auto wn = sendReportAckToFd(s.fd, data.device_id, static_cast<std::uint32_t>(data.seq));
                        const bool ok = wn == 11;
                        ackDebugMarkTx(now_ms2, ok);
                        ackDebugAddLine("tcp_binary rx report seq=" + std::to_string(data.seq));
                        Logger::instance().info("tx ack seq=" + std::to_string(data.seq));
                        Logger::instance().info("ack write bytes=" + std::to_string(wn));
                        ackDebugAddLine("tcp_binary tx ack seq=" + std::to_string(data.seq) +
                                        " ack write bytes=" + std::to_string(wn));
                    }
                    {
                        std::lock_guard<std::mutex> lock(wifi_mutex_);
                        tcp_binary_device_fds_[data.device_id] = s.fd;
                    }
                    if (!data_queue_.push(std::move(data))) {
                        break;
                    }
                }
                s.pending.erase(0, total);
            }
        }
    }
    for (auto& kv : sessions) {
        ::close(kv.second.fd);
    }
    ::close(epfd);
    ::close(listen_fd);
}

void AppController::heartbeatLoop() {
    const auto interval = std::chrono::seconds(config_.heartbeat.interval_sec);
    while (running_.load()) {
        std::this_thread::sleep_for(interval);
        if (!running_.load()) {
            break;
        }

        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        const auto uptime_sec =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time_).count();
        const auto snap = stats_.snapshot();
        const auto ru = ResourceUsageReader::read();

        std::ostringstream oss;
        oss << "{"
            << "\"type\":\"heartbeat\","
            << "\"gateway_id\":\"" << config_.heartbeat.gateway_id << "\","
            << "\"timestamp\":" << now_ms << ","
            << "\"uptime_sec\":" << uptime_sec << ","
            << "\"received_frames\":" << snap.received_frames << ","
            << "\"parsed_ok\":" << snap.parsed_ok << ","
            << "\"crc_errors\":" << snap.crc_errors << ","
            << "\"unknown_type_frames\":" << snap.unknown_type_frames << ","
            << "\"dropped_bytes\":" << snap.dropped_bytes << ","
            << "\"upload_success\":" << snap.upload_success << ","
            << "\"upload_failed\":" << snap.upload_failed << ","
            << "\"cache_backlog\":" << snap.cache_backlog << ","
            << "\"reconnects\":" << snap.reconnects << ","
            << "\"last_reconnect_ms\":" << snap.last_reconnect_unix_ms << ","
            << "\"devices_online\":" << snap.device_online << ","
            << "\"devices_offline\":" << snap.device_offline << ","
            << "\"rss_kb\":" << ru.rss_kb << ","
            << "\"vms_kb\":" << ru.vms_kb << ","
            << "\"user_cpu_ms\":" << ru.user_cpu_ms << ","
            << "\"sys_cpu_ms\":" << ru.sys_cpu_ms << ","
            << "\"threads\":" << ru.threads
            << "}";

        if (uploader_ == nullptr || !uploader_->uploadHeartbeat(oss.str())) {
            Logger::instance().warn("heartbeat upload failed");
        }
    }
}

void AppController::pushRecentSample(const SensorData& data) {
    if (!config_.monitor.enabled || config_.monitor.recent_capacity == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(recent_mutex_);
    if (recent_samples_.size() >= config_.monitor.recent_capacity) {
        recent_samples_.pop_front();
    }
    recent_samples_.push_back(data);
}

void AppController::monitorLoop() {
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        Logger::instance().error("monitor socket create failed");
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(config_.monitor.port));
    if (inet_pton(AF_INET, config_.monitor.bind_host.c_str(), &addr.sin_addr) != 1) {
        Logger::instance().error("monitor bind_host invalid: " + config_.monitor.bind_host);
        ::close(listen_fd);
        return;
    }

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        Logger::instance().error("monitor bind failed: " + std::string(std::strerror(errno)));
        ::close(listen_fd);
        return;
    }
    if (listen(listen_fd, 8) != 0) {
        Logger::instance().error("monitor listen failed");
        ::close(listen_fd);
        return;
    }

    const int flags = fcntl(listen_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
    }

    Logger::instance().info("instance=" + config_.serial.instance_name +
                            " monitor http listening on " + config_.monitor.bind_host + ":" +
                            std::to_string(config_.monitor.port));

    auto runControlCommand = [&](ControlCommandRequest cmd_req, std::string& http_status, std::string& response_body) {
        executeControlCommand(std::move(cmd_req), http_status, response_body);
    };

    while (running_.load()) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        const int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            continue;
        }

        const int cflags = fcntl(client_fd, F_GETFL, 0);
        if (cflags >= 0) {
            fcntl(client_fd, F_SETFL, cflags | O_NONBLOCK);
        }

        std::string req;
        std::string read_err;
        if (!readHttpRequest(client_fd, req, running_, read_err)) {
            Logger::instance().warn("http request read failed: " + read_err);
            const std::string resp = makeHttpResponse("400 Bad Request",
                                                      "text/plain; charset=utf-8",
                                                      read_err + "\n");
            (void)writeAll(client_fd, resp);
            ::close(client_fd);
            continue;
        }
        if (req.empty()) {
            ::close(client_fd);
            continue;
        }

        std::string method = "GET";
        std::string path = "/";
        {
            std::istringstream iss(req);
            iss >> method >> path;
        }
        const std::string raw_path = pathWithoutQuery(path);
        const std::string req_body = httpBody(req);

        std::string body;
        std::string content_type = "text/plain; charset=utf-8";
        std::string status = "200 OK";

        if (method != "GET" && method != "POST") {
            status = "405 Method Not Allowed";
            body = "method not allowed\n";
        } else if (method == "GET" && (raw_path == "/" || raw_path == "/index.html")) {
            content_type = "text/html; charset=utf-8";
            body = R"HTML(<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Serial Gateway Monitor</title>
  <style>
    :root{
      --bg:#f4f7f2;
      --bg2:#e8f0e5;
      --card:#ffffff;
      --ink:#13211b;
      --muted:#4b6559;
      --line:#d7e2d2;
      --ok:#1f8f5f;
      --warn:#d38b22;
      --bad:#d14545;
      --accent:#2f6f54;
    }
    *{box-sizing:border-box}
    body{
      margin:0;
      color:var(--ink);
      font-family:"IBM Plex Sans","Segoe UI",Tahoma,sans-serif;
      background:
        radial-gradient(1000px 350px at 20% -10%, #d6ead4 0%, transparent 70%),
        radial-gradient(1000px 350px at 100% -10%, #d8e7f4 0%, transparent 65%),
        linear-gradient(145deg,var(--bg),var(--bg2));
    }
    .wrap{max-width:1200px;margin:0 auto;padding:22px 16px 40px}
    .top{display:flex;justify-content:space-between;align-items:flex-end;gap:10px;flex-wrap:wrap}
    .title{margin:0;font-size:30px;letter-spacing:.2px}
    .subtitle{color:var(--muted);margin-top:6px}
    .pulse{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--ok);box-shadow:0 0 0 rgba(31,143,95,.7);animation:pulse 1.7s infinite}
    @keyframes pulse{0%{box-shadow:0 0 0 0 rgba(31,143,95,.7)}70%{box-shadow:0 0 0 12px rgba(31,143,95,0)}100%{box-shadow:0 0 0 0 rgba(31,143,95,0)}}
    .grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px;margin-top:14px}
    .stat{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:12px}
    .k{font-size:13px;color:var(--muted)}
    .v{font-size:28px;font-weight:700;margin-top:3px}
    .panel{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:12px;margin-top:12px}
    .panel h2{margin:2px 0 10px 0;font-size:16px}
    .toolbar{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:10px}
    .btn{border:1px solid var(--line);background:#f5faf3;color:var(--ink);padding:6px 10px;border-radius:10px;cursor:pointer;font-size:12px}
    .btn.active{background:var(--accent);border-color:var(--accent);color:#fff}
    table{width:100%;border-collapse:collapse;font-size:13px}
    th,td{border-bottom:1px solid #e6eee2;padding:8px 6px;text-align:left;vertical-align:top}
    th{color:var(--muted);font-weight:600}
    .tag{display:inline-block;padding:2px 8px;border-radius:999px;font-size:12px;font-weight:600}
    .tag-ok{background:#dcf4e7;color:#1d7d56}
    .tag-bad{background:#fae0e0;color:#a33636}
    .tag-warn{background:#fcefd7;color:#a26a19}
    .mono{font-family:"IBM Plex Mono","JetBrains Mono",Consolas,monospace}
    .raw{background:#f5f8f3;border:1px solid #e6eee2;border-radius:10px;padding:10px;white-space:pre-wrap;font-size:12px;overflow:auto;max-height:260px}
    .loading{color:var(--muted)}
    @media (max-width:980px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}}
    @media (max-width:620px){.grid{grid-template-columns:1fr}.title{font-size:24px}table{font-size:12px}}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="top">
      <div>
        <h1 class="title">Serial Gateway Monitor</h1>
        <div class="subtitle"><span class="pulse"></span> Live telemetry and command health</div>
      </div>
      <div class="subtitle mono" id="stamp">loading...</div>
    </div>

    <div class="grid">
      <div class="stat"><div class="k">Devices Online</div><div class="v" id="k_devices_online">-</div></div>
      <div class="stat"><div class="k">WiFi Clients</div><div class="v" id="k_wifi_clients">-</div></div>
      <div class="stat"><div class="k">Parsed Frames</div><div class="v" id="k_parsed_ok">-</div></div>
      <div class="stat"><div class="k">Command Ack Rate</div><div class="v" id="k_ack_rate">-</div></div>
    </div>
    <div class="grid">
      <div class="stat"><div class="k">ACK Rx (1m)</div><div class="v" id="k_ack_rx_1m">-</div></div>
      <div class="stat"><div class="k">ACK Tx Success (1m)</div><div class="v" id="k_ack_tx_ok_1m">-</div></div>
      <div class="stat"><div class="k">ACK Tx Fail (1m)</div><div class="v" id="k_ack_tx_fail_1m">-</div></div>
      <div class="stat"><div class="k">ACK Success (1m)</div><div class="v" id="k_ack_succ_1m">-</div></div>
    </div>

    <div class="panel">
      <h2>Devices</h2>
      <div id="devices" class="loading">loading...</div>
    </div>
    <div class="panel">
      <h2>Recent Samples</h2>
      <div class="toolbar">
        <button class="btn active" id="flt_all" type="button">All</button>
        <button class="btn" id="flt_1" type="button">device_id=1</button>
        <button class="btn" id="flt_2" type="button">device_id=2</button>
        <button class="btn" id="flt_issues" type="button">Only Issues</button>
      </div>
      <div id="recent" class="loading">loading...</div>
    </div>
    <div class="panel">
      <h2>Debug ACK Log</h2>
      <pre id="acklog" class="raw loading">loading...</pre>
    </div>
    <div class="panel">
      <h2>Status JSON</h2>
      <pre id="status" class="raw loading">loading...</pre>
    </div>
    <div class="panel">
      <h2>Prometheus Metrics</h2>
      <pre id="metrics" class="raw loading">loading...</pre>
    </div>
  </div>
  <script>
    function esc(s){return String(s??'').replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;"}[c]))}
    function tag(ok,text,kind){
      const cls = ok ? 'tag-ok' : (kind==='warn' ? 'tag-warn' : 'tag-bad');
      return '<span class="tag '+cls+'">'+esc(text)+'</span>';
    }
    function num(v,d='-'){return (v===undefined||v===null)?d:v}
    function pct(n){return (Math.round(n*10)/10).toFixed(1)+'%'}
    function renderDevices(devices){
      if(!Array.isArray(devices)||devices.length===0){return '<div class="loading">no devices</div>'}
      const rows=devices.map(d=>'<tr>'
        +'<td class="mono">'+num(d.device_id)+'</td>'
        +'<td>'+esc(d.device_name||'')+'</td>'
        +'<td>'+esc(d.link_type||'')+'</td>'
        +'<td>'+(d.online?tag(true,'online'):tag(false,'offline'))+'</td>'
        +'<td>'+((d.link_type==='wifi')?(d.wifi_connected?tag(true,'wifi ok'):tag(false,'wifi down','warn')):'-')+'</td>'
        +'<td class="mono">'+num(d.wifi_rssi,'-')+'</td>'
        +'<td class="mono">'+num(d.report_count)+'</td>'
        +'<td class="mono">'+esc(d.last_summary||'')+'</td>'
      +'</tr>').join('');
      return '<table><thead><tr><th>ID</th><th>Name</th><th>Link</th><th>Online</th><th>WiFi</th><th>RSSI</th><th>Reports</th><th>Summary</th></tr></thead><tbody>'+rows+'</tbody></table>';
    }
    let recentFilter = -1;
    let onlyIssues = false;
    function setFilter(v){
      recentFilter = v;
      const ids=[['flt_all',-1],['flt_1',1],['flt_2',2]];
      ids.forEach(([id,val])=>{
        const el=document.getElementById(id);
        if(!el)return;
        if(val===recentFilter) el.classList.add('active');
        else el.classList.remove('active');
      });
    }
    function setOnlyIssues(v){
      onlyIssues = !!v;
      const el=document.getElementById('flt_issues');
      if(!el)return;
      if(onlyIssues) el.classList.add('active');
      else el.classList.remove('active');
    }
    function renderRecent(items){
      if(!Array.isArray(items)||items.length===0){return '<div class="loading">no samples</div>'}
      let filtered=(recentFilter<0)?items:items.filter(x=>Number(x.device_id)===recentFilter);
      if(onlyIssues){
        filtered=filtered.filter(x=>{
          const evt=String(x.event_type||'');
          const summary=String(x.payload_summary||'').toLowerCase();
          return evt==='unknown' || evt==='command_ack' || summary.includes('unknown') || summary.includes('fail') || summary.includes('timeout');
        });
      }
      if(filtered.length===0){return '<div class="loading">no samples for current filter</div>'}
      const view=filtered.slice(Math.max(0,filtered.length-18)).reverse();
      const rows=view.map(s=>'<tr>'
        +'<td class="mono">'+num(s.device_id)+'</td>'
        +'<td class="mono">'+num(s.seq,'-')+'</td>'
        +'<td>'+esc(s.link_type||'')+'</td>'
        +'<td>'+esc(s.event_type||'')+'</td>'
        +'<td class="mono">'+num(s.temperature,'-')+'</td>'
        +'<td class="mono">'+num(s.humidity,'-')+'</td>'
        +'<td class="mono">'+num(s.voltage,'-')+'</td>'
        +'<td class="mono">'+num(s.mq2_alarm,'-')+'/'+num(s.ld2402_presence,'-')+'</td>'
        +'<td class="mono">'+num(s.led_on,'-')+'/'+num(s.alarm_on,'-')+'/'+num(s.sensor_valid,'-')+'/'+num(s.auto_mode,'-')+'</td>'
        +'<td class="mono">'+esc(s.payload_summary||'')+'</td>'
      +'</tr>').join('');
      return '<table><thead><tr><th>ID</th><th>Seq</th><th>Link</th><th>Event</th><th>T</th><th>H</th><th>V</th><th>MQ2/LD2402</th><th>LED/ALM/VALID/AUTO</th><th>Summary</th></tr></thead><tbody>'+rows+'</tbody></table>';
    }
    async function tick(){
      try{
        const [s,d,r,m,a] = await Promise.all([
          fetch('/api/status').then(x=>x.json()),
          fetch('/api/devices').then(x=>x.json()),
          fetch('/api/recent').then(x=>x.json()),
          fetch('/metrics').then(x=>x.text()),
          fetch('/api/debug_ack').then(x=>x.json()),
        ]);
        const ackBase=(s.command_ack_count||0)+(s.command_timeout_count||0)+(s.command_fail_count||0);
        const ackRate=ackBase>0?pct((s.command_ack_count||0)*100/ackBase):'N/A';
        document.getElementById('k_devices_online').textContent=num(s.devices_online);
        document.getElementById('k_wifi_clients').textContent=num(s.wifi_clients_connected);
        document.getElementById('k_parsed_ok').textContent=num(s.parsed_ok);
        document.getElementById('k_ack_rate').textContent=ackRate;
        document.getElementById('k_ack_rx_1m').textContent=num(a.rx_1m,0);
        document.getElementById('k_ack_tx_ok_1m').textContent=num(a.tx_ok_1m,0);
        document.getElementById('k_ack_tx_fail_1m').textContent=num(a.tx_fail_1m,0);
        document.getElementById('k_ack_succ_1m').textContent=(a.ack_success_rate_1m===undefined)?'-':pct(Number(a.ack_success_rate_1m));
        document.getElementById('devices').innerHTML=renderDevices(d);
        document.getElementById('recent').innerHTML=renderRecent(r);
        document.getElementById('acklog').textContent=(a.lines||[]).slice().reverse().join('\n');
        document.getElementById('status').textContent=JSON.stringify(s,null,2);
        document.getElementById('metrics').textContent=m;
        document.getElementById('stamp').textContent='Updated: '+new Date().toLocaleString();
      }catch(e){
        document.getElementById('stamp').textContent='Update failed: '+e;
      }
    }
    document.getElementById('flt_all').addEventListener('click',()=>setFilter(-1));
    document.getElementById('flt_1').addEventListener('click',()=>setFilter(1));
    document.getElementById('flt_2').addEventListener('click',()=>setFilter(2));
    document.getElementById('flt_issues').addEventListener('click',()=>setOnlyIssues(!onlyIssues));
    setFilter(-1);
    setOnlyIssues(false);
    tick();
    setInterval(tick,1000);
  </script>
</body>
</html>)HTML";
        } else if (method == "GET" && startsWith(raw_path, "/fw/")) {
            const auto parts = splitPath(raw_path); // fw/{firmware_id}/{file}
            if (parts.size() != 3 || parts[0] != "fw") {
                status = "404 Not Found";
                body = "not found\n";
            } else {
                const std::string firmware_id = parts[1];
                const std::string file_name = parts[2];
                if (firmware_id.find("..") != std::string::npos ||
                    firmware_id.find('/') != std::string::npos ||
                    file_name.find("..") != std::string::npos ||
                    (file_name != "app.bin" && file_name != "manifest.json")) {
                    status = "400 Bad Request";
                    body = "invalid firmware path\n";
                } else {
                    const std::string file_path =
                        gatewayRuntimePath("data/firmware/" + firmware_id + "/" + file_name);
                    std::ifstream in(file_path, std::ios::binary);
                    if (!in.is_open()) {
                        status = "404 Not Found";
                        body = "firmware file not found\n";
                    } else {
                        std::ostringstream oss;
                        oss << in.rdbuf();
                        body = oss.str();
                        if (file_name == "app.bin") {
                            content_type = "application/octet-stream";
                        } else {
                            content_type = "application/json; charset=utf-8";
                        }
                    }
                }
            }
        } else if (method == "GET" && raw_path == "/api/status") {
            content_type = "application/json; charset=utf-8";
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
            const auto uptime_sec = std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::steady_clock::now() - start_time_)
                                        .count();
            const auto snap = stats_.snapshot();
            const auto ru = ResourceUsageReader::read();

            std::size_t recent_count = 0;
            std::size_t esp32_status_slots = 0;
            {
                std::lock_guard<std::mutex> lock(recent_mutex_);
                recent_count = recent_samples_.size();
            }
            {
                std::lock_guard<std::mutex> lock(g_esp32_ota_status_mutex);
                esp32_status_slots = g_esp32_ota_status_map.size();
            }

            std::ostringstream oss;
            oss << "{"
                << "\"timestamp\":" << now_ms << ","
                << "\"uptime_sec\":" << uptime_sec << ","
                << "\"serial_device\":\"" << jsonEscape(config_.serial.device) << "\","
                << "\"serial_instance\":\"" << jsonEscape(config_.serial.instance_name) << "\","
                << "\"serial_available\":" << (serial_available_ ? "true" : "false") << ","
                << "\"uploader_type\":\"" << jsonEscape(config_.uploader.type) << "\","
                << "\"upload_target\":\"" << jsonEscape(config_.uploader.host) << ":" << config_.uploader.port << "\","
                << "\"received_frames\":" << snap.received_frames << ","
                << "\"parsed_ok\":" << snap.parsed_ok << ","
                << "\"crc_errors\":" << snap.crc_errors << ","
                << "\"unknown_type_frames\":" << snap.unknown_type_frames << ","
                << "\"dropped_bytes\":" << snap.dropped_bytes << ","
                << "\"upload_success\":" << snap.upload_success << ","
                << "\"upload_failed\":" << snap.upload_failed << ","
                << "\"cache_backlog\":" << snap.cache_backlog << ","
                << "\"cache_replay_success\":" << snap.cache_replay_success << ","
                << "\"cache_replay_failed\":" << snap.cache_replay_failed << ","
                << "\"reconnects\":" << snap.reconnects << ","
                << "\"last_reconnect_ms\":" << snap.last_reconnect_unix_ms << ","
                << "\"devices_online\":" << snap.device_online << ","
                << "\"devices_offline\":" << snap.device_offline << ","
                << "\"wifi_clients_connected\":" << snap.wifi_clients_connected << ","
                << "\"serial_devices_online\":" << snap.serial_devices_online << ","
                << "\"wifi_devices_online\":" << snap.wifi_devices_online << ","
                << "\"command_submit_count\":" << snap.command_submit_count << ","
                << "\"command_ack_count\":" << snap.command_ack_count << ","
                << "\"command_timeout_count\":" << snap.command_timeout_count << ","
                << "\"command_fail_count\":" << snap.command_fail_count << ","
                << "\"command_late_ack_count\":" << snap.command_late_ack_count << ","
                << "\"wifi_json_parse_ok\":" << snap.wifi_json_parse_ok << ","
                << "\"wifi_json_parse_fail\":" << snap.wifi_json_parse_fail << ","
                << "\"wifi_unknown_device\":" << snap.wifi_unknown_device << ","
                << "\"wifi_events_received\":" << snap.wifi_events_received << ","
                << "\"rss_kb\":" << ru.rss_kb << ","
                << "\"vms_kb\":" << ru.vms_kb << ","
                << "\"user_cpu_ms\":" << ru.user_cpu_ms << ","
                << "\"sys_cpu_ms\":" << ru.sys_cpu_ms << ","
                << "\"threads\":" << ru.threads << ","
                << "\"ota_executor_active\":" << g_ota_active_tasks.load() << ","
                << "\"ota_executor_rejected_total\":" << g_ota_rejected_tasks.load() << ","
                << "\"esp32_ota_status_slots\":" << esp32_status_slots << ","
                << "\"recent_count\":" << recent_count << ","
                << "\"recent_capacity\":" << config_.monitor.recent_capacity
                << "}";
            body = oss.str();
        } else if (method == "GET" && raw_path == "/api/recent") {
            content_type = "application/json; charset=utf-8";
            std::deque<SensorData> snapshot;
            {
                std::lock_guard<std::mutex> lock(recent_mutex_);
                snapshot = recent_samples_;
            }

            std::ostringstream oss;
            oss << "[";
            for (std::size_t i = 0; i < snapshot.size(); ++i) {
                const auto& s = snapshot[i];
                if (i > 0) {
                    oss << ",";
                }
                oss << "{"
                    << "\"timestamp\":" << s.timestamp_unix_ms << ","
                    << "\"device_id\":" << static_cast<int>(s.device_id) << ","
                    << "\"event_type\":\"" << frameTypeName(s.frame_type) << "\","
                    << "\"device_name\":\"" << jsonEscape(s.device_name) << "\","
                    << "\"topic_suffix\":\"" << jsonEscape(s.topic_suffix) << "\","
                    << "\"payload_summary\":\"" << jsonEscape(s.payload_summary) << "\","
                    << "\"link_type\":\"" << jsonEscape(s.link_type) << "\","
                    << "\"seq\":" << s.seq << ","
                    << "\"temperature\":" << s.temperature << ","
                    << "\"humidity\":" << s.humidity << ","
                    << "\"voltage\":" << s.voltage << ","
                    << "\"light\":" << s.light << ","
                    << "\"status\":" << static_cast<int>(s.status) << ","
                    << "\"command_result\":" << static_cast<int>(s.command_result) << ","
                    << "\"mq2_alarm\":" << s.mq2_alarm << ","
                    << "\"ld2402_presence\":" << s.ld2402_presence << ","
                    << "\"led_on\":" << s.led_on << ","
                    << "\"alarm_on\":" << s.alarm_on << ","
                    << "\"sensor_valid\":" << s.sensor_valid << ","
                    << "\"auto_mode\":" << s.auto_mode
                    << "}";
            }
            oss << "]";
            body = oss.str();
        } else if (method == "GET" && raw_path == "/api/debug_ack") {
            content_type = "application/json; charset=utf-8";
            const auto now_ms = unixMsNowLocal();
            std::deque<std::string> lines;
            std::size_t rx1m = 0;
            std::size_t tx_ok1m = 0;
            std::size_t tx_fail1m = 0;
            {
                std::lock_guard<std::mutex> lock(g_ack_debug_mutex);
                ackDebugPruneOld(g_ack_rx_ts_ms, now_ms);
                ackDebugPruneOld(g_ack_tx_ok_ts_ms, now_ms);
                ackDebugPruneOld(g_ack_tx_fail_ts_ms, now_ms);
                rx1m = g_ack_rx_ts_ms.size();
                tx_ok1m = g_ack_tx_ok_ts_ms.size();
                tx_fail1m = g_ack_tx_fail_ts_ms.size();
                lines = g_ack_debug_lines;
            }
            std::ostringstream oss;
            oss << "{"
                << "\"rx_1m\":" << rx1m << ","
                << "\"tx_ok_1m\":" << tx_ok1m << ","
                << "\"tx_fail_1m\":" << tx_fail1m << ","
                << "\"ack_success_rate_1m\":" << ((tx_ok1m + tx_fail1m) > 0 ? (100.0 * tx_ok1m / (tx_ok1m + tx_fail1m)) : 0.0) << ","
                << "\"lines\":[";
            const std::size_t begin = (lines.size() > 80) ? (lines.size() - 80) : 0;
            for (std::size_t i = begin; i < lines.size(); ++i) {
                if (i > begin) oss << ",";
                oss << "\"" << jsonEscape(lines[i]) << "\"";
            }
            oss << "]}";
            body = oss.str();
        } else if (method == "GET" && raw_path == "/api/devices") {
            content_type = "application/json; charset=utf-8";
            const auto devices = device_registry_.snapshot();
            std::ostringstream oss;
            oss << "[";
            for (std::size_t i = 0; i < devices.size(); ++i) {
                const auto& d = devices[i];
                if (i > 0) {
                    oss << ",";
                }
                oss << "{"
                    << "\"device_id\":" << static_cast<int>(d.device_id) << ","
                    << "\"device_name\":\"" << jsonEscape(d.device_name) << "\","
                    << "\"topic_suffix\":\"" << jsonEscape(d.topic_suffix) << "\","
                    << "\"configured\":" << (d.configured ? "true" : "false") << ","
                    << "\"enabled\":" << (d.enabled ? "true" : "false") << ","
                    << "\"online\":" << (d.online ? "true" : "false") << ","
                    << "\"last_report_ms\":" << d.last_report_unix_ms << ","
                    << "\"report_count\":" << d.report_count << ","
                    << "\"parse_fail_count\":" << d.parse_fail_count << ","
                    << "\"link_type\":\"" << jsonEscape(d.link_type) << "\","
                    << "\"active_transport\":\"" << jsonEscape(activeTransportFor(d.device_id)) << "\","
                    << "\"wifi_rssi\":" << d.wifi_rssi << ","
                    << "\"wifi_connected\":" << (d.wifi_connected ? "true" : "false") << ","
                    << "\"wifi_last_seen_ms\":" << d.wifi_last_seen_ms << ","
                    << "\"wifi_reconnects\":" << d.wifi_reconnects << ","
                    << "\"last_summary\":\"" << jsonEscape(d.last_summary) << "\","
                    << "\"last_error\":\"" << jsonEscape(d.last_error) << "\""
                    << "}";
            }
            oss << "]";
            body = oss.str();
        } else if (method == "GET" && raw_path == "/api/commands/recent") {
            content_type = "application/json; charset=utf-8";
            std::deque<ControlCommandLogEntry> snapshot;
            {
                std::lock_guard<std::mutex> lock(g_control_command_log_mutex);
                snapshot = g_control_command_logs;
            }

            std::ostringstream oss;
            oss << "{\"commands\":[";
            for (std::size_t i = 0; i < snapshot.size(); ++i) {
                const auto& c = snapshot[i];
                if (i > 0) {
                    oss << ",";
                }
                oss << "{"
                    << "\"time\":\"" << jsonEscape(c.time_text) << "\","
                    << "\"device_id\":" << static_cast<int>(c.device_id) << ","
                    << "\"command_id\":" << c.command_id << ","
                    << "\"command_type\":\"" << jsonEscape(c.command_type) << "\","
                    << "\"status\":\"" << jsonEscape(c.status) << "\","
                    << "\"latency_ms\":" << c.latency_ms << ","
                    << "\"error\":\"" << jsonEscape(c.error) << "\""
                    << "}";
            }
            oss << "]}";
            body = oss.str();
        } else if (method == "POST" && raw_path == "/api/command") {
            content_type = "application/json; charset=utf-8";
            handleControlCommandRequestJson(req_body, status, body);
        } else if (method == "POST" && startsWith(raw_path, "/api/device/")) {
            const auto parts = splitPath(raw_path);
            if (parts.size() == 4 && parts[1] == "device" &&
                (parts[3] == "led" || parts[3] == "mode" || parts[3] == "threshold")) {
                int device_id = 0;
                if (!parseIntInRange(parts[2], 0, 255, device_id)) {
                    status = "400 Bad Request";
                    body = "{\"ok\":false,\"device_id\":0,\"command_id\":0,\"command_type\":\"\",\"status\":\"error\",\"latency_ms\":0,\"error\":\"invalid device_id\"}";
                } else {
                    ControlCommandRequest cmd_req;
                    cmd_req.device_id = static_cast<std::uint8_t>(device_id);
                    cmd_req.timeout_ms = 3000;
                    if (parts[3] == "led") {
                        const auto on = jsonGetBool(req_body, "on");
                        if (!on.has_value()) {
                            status = "400 Bad Request";
                            body = "{\"ok\":false,\"device_id\":" + std::to_string(device_id) +
                                   ",\"command_id\":0,\"command_type\":\"set_led\",\"status\":\"error\",\"latency_ms\":0,\"error\":\"missing on\"}";
                        } else {
                            const auto timeout_ms = jsonGetInt(req_body, "timeout_ms");
                            if (timeout_ms.has_value()) {
                                if (*timeout_ms <= 0) {
                                    status = "400 Bad Request";
                                    body = "{\"ok\":false,\"device_id\":" + std::to_string(device_id) +
                                           ",\"command_id\":0,\"command_type\":\"set_led\",\"status\":\"error\",\"latency_ms\":0,\"error\":\"invalid timeout_ms\"}";
                                    continue;
                                }
                                cmd_req.timeout_ms = static_cast<int>(*timeout_ms);
                                if (cmd_req.timeout_ms > 10000) cmd_req.timeout_ms = 10000;
                            }
                            cmd_req.command_type = "set_led";
                            cmd_req.has_on = true;
                            cmd_req.on = *on;
                            runControlCommand(cmd_req, status, body);
                        }
                    } else if (parts[3] == "mode") {
                        const auto mode = jsonGetString(req_body, "mode");
                        if (!mode.has_value()) {
                            status = "400 Bad Request";
                            body = "{\"ok\":false,\"device_id\":" + std::to_string(device_id) +
                                   ",\"command_id\":0,\"command_type\":\"set_mode\",\"status\":\"error\",\"latency_ms\":0,\"error\":\"missing mode\"}";
                        } else {
                            const auto timeout_ms = jsonGetInt(req_body, "timeout_ms");
                            if (timeout_ms.has_value()) {
                                if (*timeout_ms <= 0) {
                                    status = "400 Bad Request";
                                    body = "{\"ok\":false,\"device_id\":" + std::to_string(device_id) +
                                           ",\"command_id\":0,\"command_type\":\"set_mode\",\"status\":\"error\",\"latency_ms\":0,\"error\":\"invalid timeout_ms\"}";
                                    continue;
                                }
                                cmd_req.timeout_ms = static_cast<int>(*timeout_ms);
                                if (cmd_req.timeout_ms > 10000) cmd_req.timeout_ms = 10000;
                            }
                            if (*mode == "auto" || *mode == "1") {
                                cmd_req.mode_auto = true;
                            } else if (*mode == "manual" || *mode == "0") {
                                cmd_req.mode_auto = false;
                            } else {
                                status = "400 Bad Request";
                                body = "{\"ok\":false,\"device_id\":" + std::to_string(device_id) +
                                       ",\"command_id\":0,\"command_type\":\"set_mode\",\"status\":\"error\",\"latency_ms\":0,\"error\":\"invalid mode\"}";
                                continue;
                            }
                            cmd_req.command_type = "set_mode";
                            cmd_req.has_mode = true;
                            runControlCommand(cmd_req, status, body);
                        }
                    } else {
                        const auto temp = jsonGetDouble(req_body, "temperature");
                        const auto humi = jsonGetDouble(req_body, "humidity");
                        if (!temp.has_value() || !humi.has_value()) {
                            status = "400 Bad Request";
                            body = "{\"ok\":false,\"device_id\":" + std::to_string(device_id) +
                                   ",\"command_id\":0,\"command_type\":\"set_threshold\",\"status\":\"error\",\"latency_ms\":0,\"error\":\"missing temperature/humidity\"}";
                        } else {
                            const auto timeout_ms = jsonGetInt(req_body, "timeout_ms");
                            if (timeout_ms.has_value()) {
                                if (*timeout_ms <= 0) {
                                    status = "400 Bad Request";
                                    body = "{\"ok\":false,\"device_id\":" + std::to_string(device_id) +
                                           ",\"command_id\":0,\"command_type\":\"set_threshold\",\"status\":\"error\",\"latency_ms\":0,\"error\":\"invalid timeout_ms\"}";
                                    continue;
                                }
                                cmd_req.timeout_ms = static_cast<int>(*timeout_ms);
                                if (cmd_req.timeout_ms > 10000) cmd_req.timeout_ms = 10000;
                            }
                            cmd_req.command_type = "set_threshold";
                            cmd_req.has_threshold = true;
                            cmd_req.threshold_temp = *temp;
                            cmd_req.threshold_humi = *humi;
                            runControlCommand(cmd_req, status, body);
                        }
                    }
                }
            } else if (parts.size() == 5 && parts[1] == "device" && parts[4] == "query" && parts[3] == "status") {
                int device_id = 0;
                if (!parseIntInRange(parts[2], 0, 255, device_id)) {
                    status = "400 Bad Request";
                    body = "{\"ok\":false,\"device_id\":0,\"command_id\":0,\"command_type\":\"\",\"status\":\"error\",\"latency_ms\":0,\"error\":\"invalid device_id\"}";
                } else {
                    ControlCommandRequest cmd_req;
                    cmd_req.device_id = static_cast<std::uint8_t>(device_id);
                    cmd_req.command_type = "get_status";
                    cmd_req.timeout_ms = 3000;
                    const auto timeout_ms = jsonGetInt(req_body, "timeout_ms");
                    if (timeout_ms.has_value()) {
                        if (*timeout_ms <= 0) {
                            status = "400 Bad Request";
                            body = "{\"ok\":false,\"device_id\":" + std::to_string(device_id) +
                                   ",\"command_id\":0,\"command_type\":\"get_status\",\"status\":\"error\",\"latency_ms\":0,\"error\":\"invalid timeout_ms\"}";
                            continue;
                        }
                        cmd_req.timeout_ms = static_cast<int>(*timeout_ms);
                        if (cmd_req.timeout_ms > 10000) cmd_req.timeout_ms = 10000;
                    }
                    runControlCommand(cmd_req, status, body);
                }
            }
        } else if (startsWith(raw_path, "/api/ota/tasks")) {
            content_type = "application/json; charset=utf-8";
            storage::PersistentStore store(otaTaskStorePath());
            ota::OtaTaskManager mgr(store);
            const auto parts = splitPath(raw_path);
            std::ostringstream oss;
            std::string err;
            if (method == "POST" && parts.size() == 3) { // /api/ota/tasks
                handleOtaTaskRequestJson(req_body, status, body);
            } else
            if (method == "GET" && parts.size() == 3) { // /api/ota/tasks
                const auto tasks = mgr.list();
                oss << "[";
                for (std::size_t i = 0; i < tasks.size(); ++i) {
                    const auto& t = tasks[i];
                    if (i > 0) oss << ",";
                    oss << "{"
                        << "\"task_uuid\":\"" << jsonEscape(t.task_uuid) << "\","
                        << "\"device_id\":" << static_cast<int>(t.device_id) << ","
                        << "\"device_type\":\"" << jsonEscape(t.device_type) << "\","
                        << "\"firmware_id\":\"" << jsonEscape(t.firmware_id) << "\","
                        << "\"state\":\"" << ota::toString(t.state) << "\","
                        << "\"last_error\":\"" << jsonEscape(t.last_error) << "\""
                        << "}";
                }
                oss << "]";
                body = oss.str();
            } else if (method == "GET" && parts.size() == 4) { // /api/ota/tasks/{id}
                const auto task = mgr.get(parts[3]);
                if (!task) {
                    status = "404 Not Found";
                    body = "{\"error\":\"task not found\"}";
                } else {
                    oss << "{"
                        << "\"task_uuid\":\"" << jsonEscape(task->task_uuid) << "\","
                        << "\"device_id\":" << static_cast<int>(task->device_id) << ","
                        << "\"device_type\":\"" << jsonEscape(task->device_type) << "\","
                        << "\"firmware_id\":\"" << jsonEscape(task->firmware_id) << "\","
                        << "\"state\":\"" << ota::toString(task->state) << "\","
                        << "\"last_error\":\"" << jsonEscape(task->last_error) << "\""
                        << "}";
                    body = oss.str();
                }
            } else if (method == "GET" && parts.size() == 5 && parts[4] == "events") {
                const auto events = mgr.events(parts[3]);
                oss << "[";
                for (std::size_t i = 0; i < events.size(); ++i) {
                    const auto& e = events[i];
                    if (i > 0) oss << ",";
                    oss << "{"
                        << "\"task_uuid\":\"" << jsonEscape(e.task_uuid) << "\","
                        << "\"ts_unix_ms\":" << e.ts_unix_ms << ","
                        << "\"state\":\"" << jsonEscape(e.state) << "\","
                        << "\"detail\":\"" << jsonEscape(e.detail) << "\""
                        << "}";
                }
                oss << "]";
                body = oss.str();
            } else if (method == "POST" && parts.size() == 5 && parts[4] == "cancel") {
                if (!mgr.cancel(parts[3], err)) {
                    status = "400 Bad Request";
                    body = "{\"error\":\"" + jsonEscape(err) + "\"}";
                } else {
                    otaCancelSet(parts[3], true);
                    body = "{\"ok\":true}";
                }
            } else if (method == "POST" && parts.size() == 5 && parts[4] == "retry") {
                if (!mgr.retry(parts[3], err)) {
                    status = "400 Bad Request";
                    body = "{\"error\":\"" + jsonEscape(err) + "\"}";
                } else {
                    otaMetricsMarkRetry();
                    body = "{\"ok\":true}";
                }
            } else {
                status = "404 Not Found";
                body = "{\"error\":\"invalid ota path\"}";
            }
        } else if (method == "GET" && raw_path == "/metrics") {
            content_type = "text/plain; version=0.0.4; charset=utf-8";
            const auto snap = stats_.snapshot();
            const auto devices = device_registry_.snapshot();
            storage::PersistentStore store(otaTaskStorePath());
            const auto ota_tasks = store.listTasks();
            ota::FirmwareStore fw_store(gatewayRuntimePath("data/firmware"));
            std::ostringstream oss;
            const std::string instance = jsonEscape(config_.serial.instance_name);
            const std::uint64_t success_count =
                (snap.command_ack_count >= snap.command_fail_count)
                    ? (snap.command_ack_count - snap.command_fail_count)
                    : 0;
            const double success_rate = (snap.command_submit_count > 0)
                                            ? static_cast<double>(success_count) /
                                                  static_cast<double>(snap.command_submit_count)
                                            : 0.0;
            oss << "sg_frames_rx_total{instance=\"" << instance << "\"} " << snap.received_frames << "\n"
                << "sg_frames_ok_total{instance=\"" << instance << "\"} " << snap.parsed_ok << "\n"
                << "sg_frames_crc_fail_total{instance=\"" << instance << "\"} " << snap.crc_errors << "\n"
                << "sg_frames_unknown_total{instance=\"" << instance << "\"} " << snap.unknown_type_frames << "\n"
                << "sg_bytes_dropped_total{instance=\"" << instance << "\"} " << snap.dropped_bytes << "\n"
                << "sg_upload_ok_total{instance=\"" << instance << "\"} " << snap.upload_success << "\n"
                << "sg_upload_fail_total{instance=\"" << instance << "\"} " << snap.upload_failed << "\n"
                << "sg_cache_enqueue_total{instance=\"" << instance << "\"} " << snap.cache_enqueue << "\n"
                << "sg_cache_replay_ok_total{instance=\"" << instance << "\"} " << snap.cache_replay_success << "\n"
                << "sg_cache_replay_fail_total{instance=\"" << instance << "\"} " << snap.cache_replay_failed << "\n"
                << "sg_cache_backlog{instance=\"" << instance << "\"} " << snap.cache_backlog << "\n"
                << "sg_reconnect_total{instance=\"" << instance << "\"} " << snap.reconnects << "\n"
                << "sg_last_reconnect_ms{instance=\"" << instance << "\"} " << snap.last_reconnect_unix_ms << "\n"
                << "sg_devices_online{instance=\"" << instance << "\"} " << snap.device_online << "\n"
                << "sg_devices_offline{instance=\"" << instance << "\"} " << snap.device_offline << "\n"
                << "sg_wifi_clients_connected{instance=\"" << instance << "\"} " << snap.wifi_clients_connected << "\n"
                << "sg_serial_devices_online{instance=\"" << instance << "\"} " << snap.serial_devices_online << "\n"
                << "sg_wifi_devices_online{instance=\"" << instance << "\"} " << snap.wifi_devices_online << "\n"
                << "sg_command_submit_count{instance=\"" << instance << "\"} " << snap.command_submit_count << "\n"
                << "sg_command_ack_count{instance=\"" << instance << "\"} " << snap.command_ack_count << "\n"
                << "sg_command_timeout_count{instance=\"" << instance << "\"} " << snap.command_timeout_count << "\n"
                << "sg_command_fail_count{instance=\"" << instance << "\"} " << snap.command_fail_count << "\n"
                << "sg_command_late_ack_count{instance=\"" << instance << "\"} " << snap.command_late_ack_count << "\n"
                << "sg_command_success_rate{instance=\"" << instance << "\"} " << success_rate << "\n"
                << "sg_wifi_json_parse_ok{instance=\"" << instance << "\"} " << snap.wifi_json_parse_ok << "\n"
                << "sg_wifi_json_parse_fail{instance=\"" << instance << "\"} " << snap.wifi_json_parse_fail << "\n"
                << "sg_wifi_unknown_device{instance=\"" << instance << "\"} " << snap.wifi_unknown_device << "\n"
                << "sg_wifi_events_received{instance=\"" << instance << "\"} " << snap.wifi_events_received << "\n";
            for (const auto& d : devices) {
                oss << "sg_device_online{instance=\"" << instance
                    << "\",device_id=\"" << static_cast<int>(d.device_id)
                    << "\",device_name=\"" << jsonEscape(d.device_name) << "\"} "
                    << (d.online ? 1 : 0) << "\n"
                    << "sg_device_report_total{instance=\"" << instance
                    << "\",device_id=\"" << static_cast<int>(d.device_id)
                    << "\",device_name=\"" << jsonEscape(d.device_name) << "\"} "
                    << d.report_count << "\n";
            }
            std::uint64_t ota_success = 0;
            std::uint64_t ota_failed = 0;
            std::uint64_t ota_canceled = 0;
            std::uint64_t ota_active = 0;
            std::uint64_t ota_rollback = 0;
            std::uint64_t ota_retry = 0;
            std::uint64_t ota_crc_err = 0;
            std::uint64_t ota_bytes = 0;
            double ota_duration_total_sec = 0.0;
            std::uint64_t ota_duration_count = 0;
            std::unordered_map<std::string, std::uint64_t> first_event;
            std::unordered_map<std::string, std::uint64_t> last_event;
            const auto ota_events = store.listEvents("");
            for (const auto& e : ota_events) {
                auto itf = first_event.find(e.task_uuid);
                if (itf == first_event.end() || e.ts_unix_ms < itf->second) first_event[e.task_uuid] = e.ts_unix_ms;
                auto itl = last_event.find(e.task_uuid);
                if (itl == last_event.end() || e.ts_unix_ms > itl->second) last_event[e.task_uuid] = e.ts_unix_ms;
                if (e.detail.find("retried") != std::string::npos) ota_retry++;
                if (e.detail.find("crc") != std::string::npos || e.detail.find("CRC") != std::string::npos) ota_crc_err++;
            }
            for (const auto& t : ota_tasks) {
                if (t.state == ota::OtaTaskState::SUCCESS) ota_success++;
                else if (t.state == ota::OtaTaskState::FAILED) ota_failed++;
                else if (t.state == ota::OtaTaskState::CANCELED) ota_canceled++;
                else if (t.state == ota::OtaTaskState::ROLLBACK) ota_rollback++;
                else ota_active++;

                if (t.state == ota::OtaTaskState::SUCCESS) {
                    auto fw = fw_store.get(t.firmware_id);
                    if (fw) ota_bytes += fw->manifest.image_size;
                    const auto f = first_event.find(t.task_uuid);
                    const auto l = last_event.find(t.task_uuid);
                    if (f != first_event.end() && l != last_event.end() && l->second >= f->second) {
                        ota_duration_total_sec += static_cast<double>(l->second - f->second) / 1000.0;
                        ota_duration_count++;
                    }
                }
            }
            double ota_duration_avg = (ota_duration_count > 0)
                                          ? (ota_duration_total_sec / static_cast<double>(ota_duration_count))
                                          : 0.0;
            {
                std::lock_guard<std::mutex> lock(g_ota_rt_metrics.mutex);
                if (g_ota_rt_metrics.duration_count > 0) {
                    ota_duration_avg = g_ota_rt_metrics.duration_total_sec /
                                       static_cast<double>(g_ota_rt_metrics.duration_count);
                }
                ota_bytes += g_ota_rt_metrics.bytes_sent_total;
                ota_retry += g_ota_rt_metrics.retry_total;
                ota_crc_err += g_ota_rt_metrics.crc_error_total;
            }
            oss << "ota_tasks_total{result=\"success\"} " << ota_success << "\n"
                << "ota_tasks_total{result=\"failed\"} " << ota_failed << "\n"
                << "ota_tasks_total{result=\"canceled\"} " << ota_canceled << "\n"
                << "ota_task_duration_seconds " << ota_duration_avg << "\n"
                << "ota_bytes_sent_total " << ota_bytes << "\n"
                << "ota_retry_total " << ota_retry << "\n"
                << "ota_crc_error_total " << ota_crc_err << "\n"
                << "ota_rollback_total " << ota_rollback << "\n"
                << "ota_active_tasks " << ota_active << "\n"
                << "ota_executor_active " << g_ota_active_tasks.load() << "\n"
                << "ota_executor_rejected_total " << g_ota_rejected_tasks.load() << "\n";
            {
                std::lock_guard<std::mutex> lock(g_esp32_ota_status_mutex);
                oss << "esp32_ota_status_slots " << g_esp32_ota_status_map.size() << "\n";
            }
            for (const auto& t : ota_tasks) {
                if (t.state == ota::OtaTaskState::SUCCESS) {
                    oss << "ota_device_version{device_id=\"" << static_cast<int>(t.device_id)
                        << "\",version=\"" << extractVersionFromFirmwareId(t.firmware_id) << "\"} 1\n";
                }
                if (!t.last_error.empty()) {
                    oss << "ota_last_error{device_id=\"" << static_cast<int>(t.device_id)
                        << "\",error=\"" << jsonEscape(t.last_error) << "\"} 1\n";
                }
            }
            body = oss.str();
        } else if (method == "GET" && raw_path == "/api/history/system") {
            if (!history_store_) {
                status = "503 Service Unavailable";
                content_type = "application/json; charset=utf-8";
                body = R"({"error":"history store unavailable"})";
            } else {
                content_type = "application/json; charset=utf-8";
                const int limit = normalizeQueryParam(path, "limit", 100, 1, 1000);
                const int offset = normalizeQueryParam(path, "offset", 0, 0, INT_MAX);
                auto recs = history_store_->querySystemEvents(limit, offset);
                std::ostringstream oss;
                oss << "[";
                for (std::size_t i = 0; i < recs.size(); ++i) {
                    const auto& r = recs[i];
                    if (i > 0) oss << ",";
                    oss << "{"
                        << "\"id\":" << r.id << ","
                        << "\"ts_unix_ms\":" << r.ts_unix_ms << ","
                        << "\"event_type\":\"" << jsonEscape(r.event_type) << "\","
                        << "\"device_id\":" << r.device_id << ","
                        << "\"detail\":\"" << jsonEscape(r.detail) << "\""
                        << "}";
                }
                oss << "]";
                body = oss.str();
            }
        } else if (method == "GET" && raw_path == "/api/history") {
            if (!history_store_) {
                status = "503 Service Unavailable";
                content_type = "application/json; charset=utf-8";
                body = R"({"error":"history store unavailable"})";
            } else {
                content_type = "application/json; charset=utf-8";
                const int device_id = normalizeQueryParam(path, "device_id", 0, 0, INT_MAX);
                const int limit = normalizeQueryParam(path, "limit", 100, 1, 1000);
                const int offset = normalizeQueryParam(path, "offset", 0, 0, INT_MAX);
                auto recs = history_store_->querySensorEvents(device_id, limit, offset);
                std::ostringstream oss;
                oss << "[";
                for (std::size_t i = 0; i < recs.size(); ++i) {
                    const auto& r = recs[i];
                    if (i > 0) oss << ",";
                    oss << "{"
                        << "\"id\":" << r.id << ","
                        << "\"ts_unix_ms\":" << r.ts_unix_ms << ","
                        << "\"device_id\":" << r.device_id << ","
                        << "\"device_name\":\"" << jsonEscape(r.device_name) << "\","
                        << "\"frame_type\":\"" << jsonEscape(r.frame_type) << "\","
                        << "\"link_type\":\"" << jsonEscape(r.link_type) << "\","
                        << "\"seq\":" << r.seq << ","
                        << "\"temperature\":" << r.temperature << ","
                        << "\"humidity\":" << r.humidity << ","
                        << "\"voltage\":" << r.voltage << ","
                        << "\"light\":" << r.light << ","
                        << "\"status\":" << r.status << ","
                        << "\"command_id\":" << r.command_id << ","
                        << "\"command_result\":" << r.command_result << ","
                        << "\"payload_summary\":\"" << jsonEscape(r.payload_summary) << "\","
                        << "\"last_error\":\"" << jsonEscape(r.last_error) << "\","
                        << "\"wifi_rssi\":" << r.wifi_rssi << ","
                        << "\"wifi_connected\":" << (r.wifi_connected ? "true" : "false") << ","
                        << "\"wifi_last_seen_ms\":" << r.wifi_last_seen_ms << ","
                        << "\"mq2_alarm\":" << r.mq2_alarm << ","
                        << "\"ld2402_presence\":" << r.ld2402_presence << ","
                        << "\"led_on\":" << r.led_on << ","
                        << "\"alarm_on\":" << r.alarm_on << ","
                        << "\"sensor_valid\":" << r.sensor_valid << ","
                        << "\"auto_mode\":" << r.auto_mode
                        << "}";
                }
                oss << "]";
                body = oss.str();
            }
        } else {
            status = "404 Not Found";
            body = "not found\n";
        }

        const std::string resp = makeHttpResponse(status, content_type, body);
        writeAll(client_fd, resp);
        ::close(client_fd);
    }

    ::close(listen_fd);
}

void AppController::initDeviceRegistry() {
    for (const auto& dev : config_.devices) {
        device_map_[dev.id] = dev;
    }
    device_registry_.loadConfiguredDevices(config_.devices);

    if (device_map_.empty()) {
        Logger::instance().info("no devices mapping configured, fallback to sensor-<id>");
    } else {
        Logger::instance().info("loaded device mappings=" + std::to_string(device_map_.size()));
    }
}

void AppController::registerPendingCommand(std::uint8_t device_id,
                                           std::uint16_t command_id,
                                           const std::string& command_type) {
    std::lock_guard<std::mutex> lock(command_mutex_);
    PendingCommand cmd;
    cmd.device_id = device_id;
    cmd.command_id = command_id;
    cmd.command_type = command_type;
    cmd.enqueue_ms = unixMsNow();
    pending_commands_[makeCommandKey(device_id, command_id)] = cmd;
    stats_.addCommandSubmitCount(1);
}

bool AppController::reservePendingCommand(std::uint8_t device_id,
                                          const std::string& command_type,
                                          std::uint16_t& command_id) {
    std::lock_guard<std::mutex> lock(command_mutex_);
    for (std::size_t tries = 0; tries < 65535U; ++tries) {
        ++next_generated_command_id_;
        if (next_generated_command_id_ == 0) {
            next_generated_command_id_ = 1;
        }
        const std::uint16_t candidate = next_generated_command_id_;
        if (candidate == 0) {
            continue;
        }
        const auto key = makeCommandKey(device_id, candidate);
        if (pending_commands_.find(key) != pending_commands_.end()) {
            continue;
        }

        PendingCommand cmd;
        cmd.device_id = device_id;
        cmd.command_id = candidate;
        cmd.command_type = command_type;
        cmd.enqueue_ms = unixMsNow();
        pending_commands_[key] = cmd;
        stats_.addCommandSubmitCount(1);
        command_id = candidate;
        return true;
    }
    return false;
}

bool AppController::waitCommandAck(std::uint8_t device_id,
                                   std::uint16_t command_id,
                                   int timeout_ms,
                                   std::uint8_t& ack_result) {
    const auto key = makeCommandKey(device_id, command_id);
    std::unique_lock<std::mutex> lock(command_mutex_);
    const auto pred = [&]() {
        const auto it = pending_commands_.find(key);
        return !running_.load() || it == pending_commands_.end() || it->second.acked;
    };
    command_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred);

    const auto it = pending_commands_.find(key);
    if (it == pending_commands_.end()) {
        return false;
    }
    if (it->second.acked) {
        ack_result = it->second.ack_result;
        stats_.addCommandAckCount(1);
        if (ack_result != 0) {
            stats_.addCommandFailCount(1);
        }
        pending_commands_.erase(it);
        return true;
    }

    stats_.addCommandTimeoutCount(1);
    pending_commands_.erase(it);
    return false;
}

bool AppController::publishUpstreamSystemEvent(const std::string& event_type,
                                               int device_id,
                                               const std::string& detail) {
    if (uploader_ == nullptr) {
        return false;
    }
    return uploader_->uploadEvent(buildUpstreamSystemEventJson(config_, event_type, device_id, detail));
}

bool AppController::publishUpstreamOtaStatus(const ota::Esp32OtaStatus& st) {
    if (uploader_ == nullptr) {
        return false;
    }
    return uploader_->uploadOtaStatus(buildUpstreamOtaStatusJson(config_, st));
}

void AppController::completeCommandAck(std::uint8_t device_id, std::uint16_t command_id, std::uint8_t ack_result) {
    std::lock_guard<std::mutex> lock(command_mutex_);
    const auto key = makeCommandKey(device_id, command_id);
    auto it = pending_commands_.find(key);
    if (it == pending_commands_.end()) {
        stats_.addCommandLateAckCount(1);
        Logger::instance().warn("[CMD] late ack device_id=" + std::to_string(static_cast<int>(device_id)) +
                                " command_id=" + std::to_string(command_id) +
                                " result=" + std::to_string(static_cast<int>(ack_result)));
        return;
    }
    it->second.acked = true;
    it->second.ack_result = ack_result;
    command_cv_.notify_all();
}

std::uint32_t AppController::makeCommandKey(std::uint8_t device_id, std::uint16_t command_id) {
    return (static_cast<std::uint32_t>(device_id) << 16U) | static_cast<std::uint32_t>(command_id);
}

std::uint64_t AppController::unixMsNow() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void AppController::refreshDeviceMetrics() {
    const std::size_t online = device_registry_.onlineCount();
    const std::size_t total = device_registry_.totalCount();
    const std::size_t offline = total >= online ? (total - online) : 0;
    const std::size_t serial_online = device_registry_.onlineCountByLinkType("serial");
    const std::size_t wifi_online = device_registry_.onlineCountByLinkType("wifi");
    std::size_t wifi_clients = 0;
    {
        std::lock_guard<std::mutex> lock(wifi_mutex_);
        wifi_clients = wifi_clients_connected_;
    }
    stats_.setDeviceOnline(online);
    stats_.setDeviceOffline(offline);
    stats_.setSerialDevicesOnline(serial_online);
    stats_.setWifiDevicesOnline(wifi_online);
    stats_.setWifiClientsConnected(wifi_clients);
}

} // namespace sg
