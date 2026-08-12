#include "uploader/tcp_uploader.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

namespace sg {

namespace {

std::uint64_t unixMsNow() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

} // namespace

TcpUploader::TcpUploader(const UploaderConfig& config, RuntimeStats& stats)
    : config_(config), stats_(stats), backoff_ms_(config.reconnect_initial_ms) {}

TcpUploader::~TcpUploader() {
    close();
}

bool TcpUploader::upload(const SensorData& data) {
    if (!ensureConnected()) {
        return false;
    }

    if (!sendLine(toJson(data))) {
        close();
        stats_.incReconnects();
        stats_.setLastReconnectUnixMs(unixMsNow());
        stats_.addUploadFailed(1);
        return false;
    }
    stats_.addUploadSuccess(1);
    return true;
}

bool TcpUploader::uploadHeartbeat(const std::string& payload) {
    if (!ensureConnected()) {
        return false;
    }

    if (!sendLine(payload)) {
        close();
        stats_.incReconnects();
        stats_.setLastReconnectUnixMs(unixMsNow());
        stats_.addUploadFailed(1);
        return false;
    }
    return true;
}

bool TcpUploader::uploadStatus(const SensorData& data) {
    return upload(data);
}

bool TcpUploader::uploadEvent(const std::string& payload) {
    return uploadHeartbeat(payload);
}

bool TcpUploader::uploadCommandAck(const SensorData& data) {
    return upload(data);
}

bool TcpUploader::uploadOtaStatus(const std::string& payload) {
    return uploadHeartbeat(payload);
}

void TcpUploader::close() {
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
}

bool TcpUploader::ensureConnected() {
    if (sock_ >= 0) {
        return true;
    }

    if (connectSocket()) {
        backoff_ms_ = config_.reconnect_initial_ms;
        return true;
    }

    stats_.incReconnects();
    stats_.setLastReconnectUnixMs(unixMsNow());
    std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms_));
    backoff_ms_ = std::min(backoff_ms_ * 2, config_.reconnect_max_ms);
    return false;
}

bool TcpUploader::connectSocket() {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const std::string port = std::to_string(config_.port);
    if (getaddrinfo(config_.host.c_str(), port.c_str(), &hints, &result) != 0) {
        return false;
    }

    for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        const int fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            sock_ = fd;
            freeaddrinfo(result);
            return true;
        }
        ::close(fd);
    }

    freeaddrinfo(result);
    return false;
}

bool TcpUploader::sendAll(const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(sock_, data + sent, len - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool TcpUploader::sendLine(const std::string& payload) {
    const std::string wire = payload + "\n";
    return sendAll(wire.data(), wire.size());
}

std::string TcpUploader::toJson(const SensorData& data) {
    std::ostringstream oss;
    const std::string fallback_name = "sensor-" + std::to_string(static_cast<int>(data.device_id));
    const std::string& name = data.device_name.empty() ? fallback_name : data.device_name;

    oss << "{"
        << "\"device_id\":" << static_cast<int>(data.device_id) << ","
        << "\"device_name\":\"" << name << "\"," 
        << "\"link_type\":\"" << (data.link_type.empty() ? "serial" : data.link_type) << "\","
        << "\"event_type\":\"" << frameTypeName(data.frame_type) << "\","
        << "\"timestamp\":" << data.timestamp_unix_ms << ","
        << "\"temperature\":" << data.temperature << ","
        << "\"humidity\":" << data.humidity << ","
        << "\"voltage\":" << data.voltage << ","
        << "\"status\":" << static_cast<int>(data.status) << ","
        << "\"command_id\":" << data.command_id << ","
        << "\"command_result\":" << static_cast<int>(data.command_result);
    if (!data.payload_summary.empty()) {
        oss << ",\"payload_summary\":\"" << data.payload_summary << "\"";
    }
    if (!data.topic_suffix.empty()) {
        oss << ",\"topic_suffix\":\"" << data.topic_suffix << "\"";
    }
    oss
        << "}";
    return oss.str();
}

} // namespace sg
