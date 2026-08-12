#include "runtime/runtime_stats.hpp"

#include <sstream>

namespace sg {

void RuntimeStats::addReceivedFrames(std::uint64_t n) {
    received_frames_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::addParsedOk(std::uint64_t n) {
    parsed_ok_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::addCrcErrors(std::uint64_t n) {
    crc_errors_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::addUnknownTypeFrames(std::uint64_t n) {
    unknown_type_frames_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::addDroppedBytes(std::uint64_t n) {
    dropped_bytes_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::addUploadSuccess(std::uint64_t n) {
    upload_success_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::addUploadFailed(std::uint64_t n) {
    upload_failed_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::addCacheEnqueue(std::uint64_t n) {
    cache_enqueue_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::addCacheReplaySuccess(std::uint64_t n) {
    cache_replay_success_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::addCacheReplayFailed(std::uint64_t n) {
    cache_replay_failed_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::setCacheBacklog(std::uint64_t n) {
    cache_backlog_.store(n, std::memory_order_relaxed);
}

void RuntimeStats::setDeviceOnline(std::uint64_t n) {
    device_online_.store(n, std::memory_order_relaxed);
}

void RuntimeStats::setDeviceOffline(std::uint64_t n) {
    device_offline_.store(n, std::memory_order_relaxed);
}

void RuntimeStats::setWifiClientsConnected(std::uint64_t n) {
    wifi_clients_connected_.store(n, std::memory_order_relaxed);
}

void RuntimeStats::setSerialDevicesOnline(std::uint64_t n) {
    serial_devices_online_.store(n, std::memory_order_relaxed);
}

void RuntimeStats::setWifiDevicesOnline(std::uint64_t n) {
    wifi_devices_online_.store(n, std::memory_order_relaxed);
}

void RuntimeStats::addCommandSubmitCount(std::uint64_t n) {
    command_submit_count_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::addCommandAckCount(std::uint64_t n) {
    command_ack_count_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::addCommandTimeoutCount(std::uint64_t n) {
    command_timeout_count_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::addCommandFailCount(std::uint64_t n) {
    command_fail_count_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::addCommandLateAckCount(std::uint64_t n) {
    command_late_ack_count_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::addWifiJsonParseOk(std::uint64_t n) {
    wifi_json_parse_ok_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::addWifiJsonParseFail(std::uint64_t n) {
    wifi_json_parse_fail_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::addWifiUnknownDevice(std::uint64_t n) {
    wifi_unknown_device_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::addWifiEventsReceived(std::uint64_t n) {
    wifi_events_received_.fetch_add(n, std::memory_order_relaxed);
}

void RuntimeStats::incReconnects() {
    reconnects_.fetch_add(1, std::memory_order_relaxed);
}

void RuntimeStats::setLastReconnectUnixMs(std::uint64_t ts_ms) {
    last_reconnect_unix_ms_.store(ts_ms, std::memory_order_relaxed);
}

std::string RuntimeStats::snapshotLine() const {
    const auto s = snapshot();
    std::ostringstream oss;
    oss << "metrics.frames_rx=" << s.received_frames
        << " metrics.frames_ok=" << s.parsed_ok
        << " metrics.frames_crc_fail=" << s.crc_errors
        << " metrics.frames_unknown_type=" << s.unknown_type_frames
        << " metrics.bytes_dropped=" << s.dropped_bytes
        << " metrics.upload_ok=" << s.upload_success
        << " metrics.upload_fail=" << s.upload_failed
        << " metrics.cache_backlog=" << s.cache_backlog
        << " metrics.cache_enqueue=" << s.cache_enqueue
        << " metrics.cache_replay_ok=" << s.cache_replay_success
        << " metrics.cache_replay_fail=" << s.cache_replay_failed
        << " metrics.reconnects=" << s.reconnects
        << " metrics.last_reconnect_ms=" << s.last_reconnect_unix_ms
        << " metrics.devices_online=" << s.device_online
        << " metrics.devices_offline=" << s.device_offline
        << " metrics.wifi_clients_connected=" << s.wifi_clients_connected
        << " metrics.serial_devices_online=" << s.serial_devices_online
        << " metrics.wifi_devices_online=" << s.wifi_devices_online
        << " metrics.command_submit_count=" << s.command_submit_count
        << " metrics.command_ack_count=" << s.command_ack_count
        << " metrics.command_timeout_count=" << s.command_timeout_count
        << " metrics.command_fail_count=" << s.command_fail_count
        << " metrics.command_late_ack_count=" << s.command_late_ack_count
        << " metrics.wifi_json_parse_ok=" << s.wifi_json_parse_ok
        << " metrics.wifi_json_parse_fail=" << s.wifi_json_parse_fail
        << " metrics.wifi_unknown_device=" << s.wifi_unknown_device
        << " metrics.wifi_events_received=" << s.wifi_events_received;
    return oss.str();
}

RuntimeStatsSnapshot RuntimeStats::snapshot() const {
    RuntimeStatsSnapshot s;
    s.received_frames = received_frames_.load(std::memory_order_relaxed);
    s.parsed_ok = parsed_ok_.load(std::memory_order_relaxed);
    s.crc_errors = crc_errors_.load(std::memory_order_relaxed);
    s.unknown_type_frames = unknown_type_frames_.load(std::memory_order_relaxed);
    s.dropped_bytes = dropped_bytes_.load(std::memory_order_relaxed);
    s.upload_success = upload_success_.load(std::memory_order_relaxed);
    s.upload_failed = upload_failed_.load(std::memory_order_relaxed);
    s.reconnects = reconnects_.load(std::memory_order_relaxed);
    s.cache_enqueue = cache_enqueue_.load(std::memory_order_relaxed);
    s.cache_replay_success = cache_replay_success_.load(std::memory_order_relaxed);
    s.cache_replay_failed = cache_replay_failed_.load(std::memory_order_relaxed);
    s.cache_backlog = cache_backlog_.load(std::memory_order_relaxed);
    s.device_online = device_online_.load(std::memory_order_relaxed);
    s.device_offline = device_offline_.load(std::memory_order_relaxed);
    s.wifi_clients_connected = wifi_clients_connected_.load(std::memory_order_relaxed);
    s.serial_devices_online = serial_devices_online_.load(std::memory_order_relaxed);
    s.wifi_devices_online = wifi_devices_online_.load(std::memory_order_relaxed);
    s.command_submit_count = command_submit_count_.load(std::memory_order_relaxed);
    s.command_ack_count = command_ack_count_.load(std::memory_order_relaxed);
    s.command_timeout_count = command_timeout_count_.load(std::memory_order_relaxed);
    s.command_fail_count = command_fail_count_.load(std::memory_order_relaxed);
    s.command_late_ack_count = command_late_ack_count_.load(std::memory_order_relaxed);
    s.wifi_json_parse_ok = wifi_json_parse_ok_.load(std::memory_order_relaxed);
    s.wifi_json_parse_fail = wifi_json_parse_fail_.load(std::memory_order_relaxed);
    s.wifi_unknown_device = wifi_unknown_device_.load(std::memory_order_relaxed);
    s.wifi_events_received = wifi_events_received_.load(std::memory_order_relaxed);
    s.last_reconnect_unix_ms = last_reconnect_unix_ms_.load(std::memory_order_relaxed);
    return s;
}

} // namespace sg
