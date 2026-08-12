#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace sg::ota {

struct Esp32OtaStart {
    std::uint8_t device_id = 0;
    std::uint32_t command_id = 0;
    std::string firmware_url;
    std::string version;
    std::uint32_t size = 0;
    std::string crc32;
    bool force = false;
};

struct Esp32OtaStatus {
    std::uint8_t device_id = 0;
    std::uint32_t command_id = 0;
    std::string ota_state;
    int progress = 0;
    std::string version_from;
    std::string version_to;
    int error_code = 0;
};

class Esp32OtaAdapter {
public:
    using StatusCb = std::function<void(const Esp32OtaStatus&)>;
    using ShouldStopCb = std::function<bool()>;

    static std::string buildOtaStartJson(const Esp32OtaStart& req);
    static std::optional<Esp32OtaStatus> parseOtaStatusJson(const std::string& line);

    bool runWithTcpLineIo(int fd, const Esp32OtaStart& req, int timeout_ms, std::string& err, const StatusCb& cb = nullptr, const ShouldStopCb& should_stop_cb = nullptr);
};

} // namespace sg::ota
