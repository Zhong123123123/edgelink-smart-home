#include "device/device_registry.hpp"

namespace sg {

void DeviceRegistry::loadConfiguredDevices(const std::vector<DeviceConfig>& devices) {
    std::lock_guard<std::mutex> lock(mutex_);
    devices_.clear();
    for (const auto& dev : devices) {
        DeviceRuntimeState state;
        state.device_id = dev.id;
        state.device_name = dev.name.empty() ? fallbackDeviceName(dev.id) : dev.name;
        state.topic_suffix = dev.topic_suffix;
        state.configured = true;
        state.enabled = dev.enabled;
        state.link_type = dev.link_type.empty() ? "serial" : dev.link_type;
        devices_[dev.id] = state;
    }
}

DeviceUpdateResult DeviceRegistry::updateOnEvent(const SensorData& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    DeviceUpdateResult result;

    auto& state = devices_[data.device_id];
    const bool was_online = state.online;
    if (!state.configured) {
        state.device_id = data.device_id;
        state.device_name = fallbackDeviceName(data.device_id);
        state.enabled = true;
    }

    if (!data.device_name.empty()) {
        state.device_name = data.device_name;
    }
    if (!data.topic_suffix.empty()) {
        state.topic_suffix = data.topic_suffix;
    }
    state.last_report_unix_ms = data.timestamp_unix_ms;
    state.report_count++;
    state.last_summary = data.payload_summary;
    state.last_error = data.last_error;
    state.online = true;
    if (!data.link_type.empty()) {
        state.link_type = data.link_type;
    }
    state.wifi_rssi = data.wifi_rssi;
    state.wifi_connected = data.wifi_connected;
    state.wifi_last_seen_ms = data.wifi_last_seen_ms;

    result.state = state;
    result.became_online = !was_online && state.online;
    return result;
}

void DeviceRegistry::markDeviceError(std::uint8_t device_id, const std::string& reason, std::uint64_t ts_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = devices_[device_id];
    if (state.device_name.empty()) {
        state.device_name = fallbackDeviceName(device_id);
    }
    state.device_id = device_id;
    state.parse_fail_count++;
    state.last_error = reason;
    if (ts_ms > 0) {
        state.last_report_unix_ms = ts_ms;
    }
}

DeviceUpdateResult DeviceRegistry::markTransportOnline(std::uint8_t device_id,
                                                       const std::string& link_type,
                                                       std::uint64_t ts_ms,
                                                       const std::string& device_name,
                                                       const std::string& summary) {
    std::lock_guard<std::mutex> lock(mutex_);
    DeviceUpdateResult result;
    auto& state = devices_[device_id];
    const bool was_online = state.online;
    if (state.device_name.empty()) {
        state.device_name = fallbackDeviceName(device_id);
    }
    state.device_id = device_id;
    state.online = true;
    state.report_count++;
    state.last_report_unix_ms = ts_ms;
    state.link_type = link_type.empty() ? state.link_type : link_type;
    if (!device_name.empty()) {
        state.device_name = device_name;
    }
    if (!summary.empty()) {
        state.last_summary = summary;
    }
    result.state = state;
    result.became_online = !was_online && state.online;
    return result;
}

void DeviceRegistry::markTransportDisconnected(std::uint8_t device_id,
                                               const std::string& link_type,
                                               std::uint64_t ts_ms,
                                               const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = devices_[device_id];
    if (state.device_name.empty()) {
        state.device_name = fallbackDeviceName(device_id);
    }
    state.device_id = device_id;
    state.link_type = link_type.empty() ? state.link_type : link_type;
    state.online = false;
    state.last_error = reason;
    if (ts_ms > 0) {
        state.last_report_unix_ms = ts_ms;
    }
    if (state.link_type == "wifi") {
        state.wifi_connected = false;
        state.wifi_reconnects++;
        if (ts_ms > 0) {
            state.wifi_last_seen_ms = ts_ms;
        }
    }
}

void DeviceRegistry::markWifiDisconnected(std::uint8_t device_id, std::uint64_t ts_ms, const std::string& reason) {
    markTransportDisconnected(device_id, "wifi", ts_ms, reason);
}

std::vector<DeviceStateTransition> DeviceRegistry::markOfflineByTimeout(std::uint64_t now_ms, std::uint64_t timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeviceStateTransition> changed;

    for (auto& kv : devices_) {
        auto& state = kv.second;
        if (!state.online || state.last_report_unix_ms == 0) {
            continue;
        }
        if (now_ms < state.last_report_unix_ms) {
            continue;
        }
        const std::uint64_t elapsed = now_ms - state.last_report_unix_ms;
        if (elapsed >= timeout_ms) {
            state.online = false;
            changed.push_back(DeviceStateTransition{state.device_id, state.device_name, false, now_ms});
        }
    }

    return changed;
}

std::vector<DeviceRuntimeState> DeviceRegistry::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeviceRuntimeState> out;
    out.reserve(devices_.size());
    for (const auto& kv : devices_) {
        out.push_back(kv.second);
    }
    return out;
}

std::size_t DeviceRegistry::onlineCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t cnt = 0;
    for (const auto& kv : devices_) {
        if (kv.second.online) {
            ++cnt;
        }
    }
    return cnt;
}

std::size_t DeviceRegistry::totalCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_.size();
}

std::size_t DeviceRegistry::onlineCountByLinkType(const std::string& link_type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t cnt = 0;
    for (const auto& kv : devices_) {
        if (kv.second.online && kv.second.link_type == link_type) {
            ++cnt;
        }
    }
    return cnt;
}

std::string DeviceRegistry::fallbackDeviceName(std::uint8_t id) {
    return "sensor-" + std::to_string(static_cast<int>(id));
}

} // namespace sg
