#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace sg {

struct RuntimeStatsSnapshot {
    std::uint64_t received_frames = 0;
    std::uint64_t parsed_ok = 0;
    std::uint64_t crc_errors = 0;
    std::uint64_t unknown_type_frames = 0;
    std::uint64_t dropped_bytes = 0;
    std::uint64_t upload_success = 0;
    std::uint64_t upload_failed = 0;
    std::uint64_t reconnects = 0;
    std::uint64_t cache_enqueue = 0;
    std::uint64_t cache_replay_success = 0;
    std::uint64_t cache_replay_failed = 0;
    std::uint64_t cache_backlog = 0;
    std::uint64_t device_online = 0;
    std::uint64_t device_offline = 0;
    std::uint64_t wifi_clients_connected = 0;
    std::uint64_t serial_devices_online = 0;
    std::uint64_t wifi_devices_online = 0;
    std::uint64_t command_submit_count = 0;
    std::uint64_t command_ack_count = 0;
    std::uint64_t command_timeout_count = 0;
    std::uint64_t command_fail_count = 0;
    std::uint64_t command_late_ack_count = 0;
    std::uint64_t wifi_json_parse_ok = 0;
    std::uint64_t wifi_json_parse_fail = 0;
    std::uint64_t wifi_unknown_device = 0;
    std::uint64_t wifi_events_received = 0;
    std::uint64_t last_reconnect_unix_ms = 0;
};

class RuntimeStats {
public:
    void addReceivedFrames(std::uint64_t n);
    void addParsedOk(std::uint64_t n);
    void addCrcErrors(std::uint64_t n);
    void addUnknownTypeFrames(std::uint64_t n);
    void addDroppedBytes(std::uint64_t n);
    void addUploadSuccess(std::uint64_t n);
    void addUploadFailed(std::uint64_t n);
    void addCacheEnqueue(std::uint64_t n);
    void addCacheReplaySuccess(std::uint64_t n);
    void addCacheReplayFailed(std::uint64_t n);
    void setCacheBacklog(std::uint64_t n);
    void setDeviceOnline(std::uint64_t n);
    void setDeviceOffline(std::uint64_t n);
    void setWifiClientsConnected(std::uint64_t n);
    void setSerialDevicesOnline(std::uint64_t n);
    void setWifiDevicesOnline(std::uint64_t n);
    void addCommandSubmitCount(std::uint64_t n);
    void addCommandAckCount(std::uint64_t n);
    void addCommandTimeoutCount(std::uint64_t n);
    void addCommandFailCount(std::uint64_t n);
    void addCommandLateAckCount(std::uint64_t n);
    void addWifiJsonParseOk(std::uint64_t n);
    void addWifiJsonParseFail(std::uint64_t n);
    void addWifiUnknownDevice(std::uint64_t n);
    void addWifiEventsReceived(std::uint64_t n);
    void incReconnects();
    void setLastReconnectUnixMs(std::uint64_t ts_ms);

    std::string snapshotLine() const;
    RuntimeStatsSnapshot snapshot() const;

private:
    std::atomic<std::uint64_t> received_frames_{0};
    std::atomic<std::uint64_t> parsed_ok_{0};
    std::atomic<std::uint64_t> crc_errors_{0};
    std::atomic<std::uint64_t> unknown_type_frames_{0};
    std::atomic<std::uint64_t> dropped_bytes_{0};
    std::atomic<std::uint64_t> upload_success_{0};
    std::atomic<std::uint64_t> upload_failed_{0};
    std::atomic<std::uint64_t> reconnects_{0};
    std::atomic<std::uint64_t> cache_enqueue_{0};
    std::atomic<std::uint64_t> cache_replay_success_{0};
    std::atomic<std::uint64_t> cache_replay_failed_{0};
    std::atomic<std::uint64_t> cache_backlog_{0};
    std::atomic<std::uint64_t> device_online_{0};
    std::atomic<std::uint64_t> device_offline_{0};
    std::atomic<std::uint64_t> wifi_clients_connected_{0};
    std::atomic<std::uint64_t> serial_devices_online_{0};
    std::atomic<std::uint64_t> wifi_devices_online_{0};
    std::atomic<std::uint64_t> command_submit_count_{0};
    std::atomic<std::uint64_t> command_ack_count_{0};
    std::atomic<std::uint64_t> command_timeout_count_{0};
    std::atomic<std::uint64_t> command_fail_count_{0};
    std::atomic<std::uint64_t> command_late_ack_count_{0};
    std::atomic<std::uint64_t> wifi_json_parse_ok_{0};
    std::atomic<std::uint64_t> wifi_json_parse_fail_{0};
    std::atomic<std::uint64_t> wifi_unknown_device_{0};
    std::atomic<std::uint64_t> wifi_events_received_{0};
    std::atomic<std::uint64_t> last_reconnect_unix_ms_{0};
};

} // namespace sg
