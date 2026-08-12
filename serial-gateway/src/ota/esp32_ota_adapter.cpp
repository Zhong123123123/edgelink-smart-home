#include "ota/esp32_ota_adapter.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <optional>
#include <regex>
#include <sstream>

namespace sg::ota {
namespace {

std::optional<long long> jsonGetInt(const std::string& s, const std::string& key) {
    const std::regex re("\\\"" + key + "\\\"\\s*:\\s*(-?[0-9]+)");
    std::smatch m;
    if (!std::regex_search(s, m, re) || m.size() < 2) return std::nullopt;
    try { return std::stoll(m[1].str()); } catch (...) { return std::nullopt; }
}

std::optional<std::string> jsonGetString(const std::string& s, const std::string& key) {
    const std::regex re("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
    std::smatch m;
    if (!std::regex_search(s, m, re) || m.size() < 2) return std::nullopt;
    return m[1].str();
}

std::string escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 4);
    for (char c : in) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out.push_back(c);
    }
    return out;
}

} // namespace

std::string Esp32OtaAdapter::buildOtaStartJson(const Esp32OtaStart& req) {
    std::ostringstream oss;
    oss << "{"
        << "\"type\":\"command\"," 
        << "\"command_id\":" << req.command_id << ","
        << "\"command_type\":\"ota_start\"," 
        << "\"device_id\":" << static_cast<int>(req.device_id) << ","
        << "\"firmware_url\":\"" << escape(req.firmware_url) << "\"," 
        << "\"version\":\"" << escape(req.version) << "\"," 
        << "\"size\":" << req.size << ","
        << "\"crc32\":\"" << escape(req.crc32) << "\"," 
        << "\"force\":" << (req.force ? "true" : "false")
        << "}";
    return oss.str();
}

std::optional<Esp32OtaStatus> Esp32OtaAdapter::parseOtaStatusJson(const std::string& line) {
    const auto event_type = jsonGetString(line, "event_type");
    if (!event_type || *event_type != "ota_status") return std::nullopt;

    const auto dev = jsonGetInt(line, "device_id");
    auto cmd = jsonGetInt(line, "command_id");
    if (!cmd) {
        cmd = jsonGetInt(line, "cmd_id");
    }
    auto state = jsonGetString(line, "ota_state");
    if (!state) {
        state = jsonGetString(line, "state");
    }
    const auto progress = jsonGetInt(line, "progress");
    const auto from = jsonGetString(line, "version_from");
    const auto to = jsonGetString(line, "version_to");
    const auto err = jsonGetInt(line, "error_code");
    if (!dev || !cmd || !state) return std::nullopt;

    Esp32OtaStatus out;
    out.device_id = static_cast<std::uint8_t>(*dev);
    out.command_id = static_cast<std::uint32_t>(*cmd);
    out.ota_state = *state;
    out.progress = progress ? static_cast<int>(*progress) : 0;
    out.version_from = from.value_or("");
    out.version_to = to.value_or("");
    out.error_code = err ? static_cast<int>(*err) : 0;
    return out;
}

bool Esp32OtaAdapter::runWithTcpLineIo(int fd, const Esp32OtaStart& req, int timeout_ms, std::string& err, const StatusCb& cb, const ShouldStopCb& should_stop_cb) {
    auto shouldStop = [&]() {
        return should_stop_cb && should_stop_cb();
    };
    if (shouldStop()) {
        err = "ota canceled";
        return false;
    }
    const std::string cmd = buildOtaStartJson(req) + "\n";
    if (::send(fd, cmd.data(), cmd.size(), 0) != static_cast<ssize_t>(cmd.size())) {
        err = "send ota_start failed";
        return false;
    }

    std::string line;
    const auto deadline_ms = timeout_ms;
    int waited = 0;
    while (waited < deadline_ms) {
        if (shouldStop()) {
            err = "ota canceled";
            return false;
        }
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int rc = ::poll(&pfd, 1, 200);
        waited += 200;
        if (rc < 0) {
            err = "poll failed";
            return false;
        }
        if (rc == 0) continue;

        char c = 0;
        const ssize_t n = ::recv(fd, &c, 1, 0);
        if (n <= 0) {
            err = "connection closed";
            return false;
        }
        if (c == '\n') {
            auto st = parseOtaStatusJson(line);
            line.clear();
            if (!st) continue;
            if (cb) cb(*st);
            if (st->ota_state == "success") return true;
            if (st->ota_state == "failed") {
                err = "esp32 ota failed error_code=" + std::to_string(st->error_code);
                return false;
            }
        } else {
            line.push_back(c);
        }
    }

    err = "esp32 ota timeout";
    return false;
}

} // namespace sg::ota
