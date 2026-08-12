#include "device/device_registry.hpp"

#include <iostream>
#include <vector>

namespace {

bool testConfiguredLoadAndOnlineOffline() {
    sg::DeviceRegistry registry;

    std::vector<sg::DeviceConfig> cfg;
    sg::DeviceConfig d;
    d.id = 1;
    d.name = "sensor-01";
    d.enabled = true;
    d.topic_suffix = "line-a/1";
    cfg.push_back(d);

    registry.loadConfiguredDevices(cfg);

    auto snap = registry.snapshot();
    if (snap.size() != 1 || snap[0].device_id != 1 || snap[0].online) {
        std::cerr << "configured load mismatch\n";
        return false;
    }

    sg::SensorData data;
    data.timestamp_unix_ms = 1000;
    data.device_id = 1;
    data.device_name = "sensor-01";
    data.topic_suffix = "line-a/1";
    data.payload_summary = "temp=20.1";

    const auto update = registry.updateOnEvent(data);
    if (!update.became_online || !update.state.online || update.state.report_count != 1) {
        std::cerr << "update online mismatch\n";
        return false;
    }

    const auto transitions = registry.markOfflineByTimeout(5000, 2000);
    if (transitions.size() != 1 || transitions[0].device_id != 1 || transitions[0].online) {
        std::cerr << "offline transition mismatch\n";
        return false;
    }

    snap = registry.snapshot();
    if (snap.size() != 1 || snap[0].online) {
        std::cerr << "snapshot online state mismatch\n";
        return false;
    }

    return true;
}

bool testErrorRecord() {
    sg::DeviceRegistry registry;
    registry.markDeviceError(9, "parse failed", 2000);

    const auto snap = registry.snapshot();
    if (snap.size() != 1 || snap[0].device_id != 9 || snap[0].parse_fail_count != 1 ||
        snap[0].last_error != "parse failed" || snap[0].last_report_unix_ms != 2000) {
        std::cerr << "error record mismatch\n";
        return false;
    }

    return true;
}

} // namespace

int main() {
    if (!testConfiguredLoadAndOnlineOffline()) {
        return 1;
    }
    if (!testErrorRecord()) {
        return 2;
    }
    return 0;
}
