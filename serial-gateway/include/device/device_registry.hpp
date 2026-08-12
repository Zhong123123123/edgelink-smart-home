#pragma once

#include "config/config.hpp"
#include "protocol/frame.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sg {

struct DeviceRuntimeState {
    std::uint8_t device_id = 0;
    std::string device_name;
    std::string topic_suffix;
    bool configured = false;
    bool enabled = true;
    bool online = false;
    std::string link_type = "serial";
    std::uint64_t last_report_unix_ms = 0;
    std::uint64_t report_count = 0;
    std::uint64_t parse_fail_count = 0;
    std::string last_summary;
    std::string last_error;
    int wifi_rssi = 0;
    bool wifi_connected = false;
    std::uint64_t wifi_last_seen_ms = 0;
    std::uint64_t wifi_reconnects = 0;
};

struct DeviceStateTransition {
    std::uint8_t device_id = 0;
    std::string device_name;
    bool online = false;
    std::uint64_t timestamp_unix_ms = 0;
};

struct DeviceUpdateResult {
    DeviceRuntimeState state;
    bool became_online = false;
};

class DeviceRegistry {
public:
    void loadConfiguredDevices(const std::vector<DeviceConfig>& devices);

    DeviceUpdateResult updateOnEvent(const SensorData& data);
    void markDeviceError(std::uint8_t device_id, const std::string& reason, std::uint64_t ts_ms);
    DeviceUpdateResult markTransportOnline(std::uint8_t device_id,
                                           const std::string& link_type,
                                           std::uint64_t ts_ms,
                                           const std::string& device_name,
                                           const std::string& summary);
    void markTransportDisconnected(std::uint8_t device_id,
                                   const std::string& link_type,
                                   std::uint64_t ts_ms,
                                   const std::string& reason);
    void markWifiDisconnected(std::uint8_t device_id, std::uint64_t ts_ms, const std::string& reason);

    std::vector<DeviceStateTransition> markOfflineByTimeout(std::uint64_t now_ms, std::uint64_t timeout_ms);
    std::vector<DeviceRuntimeState> snapshot() const;

    std::size_t onlineCount() const;
    std::size_t totalCount() const;
    std::size_t onlineCountByLinkType(const std::string& link_type) const;

private:
    static std::string fallbackDeviceName(std::uint8_t id);

    mutable std::mutex mutex_;
    std::unordered_map<std::uint8_t, DeviceRuntimeState> devices_;
};

} // namespace sg
