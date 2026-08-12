#include "config/config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace sg {

namespace {

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

std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

bool parseKeyValue(const std::string& line, std::string& key, std::string& val) {
    const auto pos = line.find(':');
    if (pos == std::string::npos) {
        return false;
    }
    key = trim(line.substr(0, pos));
    val = stripQuotes(trim(line.substr(pos + 1)));
    return !key.empty();
}

bool toInt(const std::string& s, int& out) {
    try {
        std::size_t pos = 0;
        out = std::stoi(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

bool toSize(const std::string& s, std::size_t& out) {
    int v = 0;
    if (!toInt(s, v) || v < 0) {
        return false;
    }
    out = static_cast<std::size_t>(v);
    return true;
}

bool toBool(const std::string& s, bool& out) {
    std::string low = s;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (low == "true" || low == "1") {
        out = true;
        return true;
    }
    if (low == "false" || low == "0") {
        out = false;
        return true;
    }
    return false;
}

bool applyDeviceValue(DeviceConfig& dev, const std::string& key, const std::string& val, std::string& err) {
    int i = 0;
    bool b = false;

    if (key == "id" || key == "device_id") {
        if (!toInt(val, i) || i < 0 || i > 255) {
            err = "invalid devices.id";
            return false;
        }
        dev.id = static_cast<std::uint8_t>(i);
        return true;
    }
    if (key == "name") {
        dev.name = val;
        return true;
    }
    if (key == "enabled") {
        if (!toBool(val, b)) {
            err = "invalid devices.enabled";
            return false;
        }
        dev.enabled = b;
        return true;
    }
    if (key == "topic_suffix") {
        dev.topic_suffix = val;
        return true;
    }
    if (key == "link_type") {
        dev.link_type = val;
        return true;
    }

    err = "unknown devices key: " + key;
    return false;
}

bool applySerialValue(SerialConfig& serial, const std::string& key, const std::string& val, std::string& err) {
    int i = 0;
    std::size_t z = 0;

    if (key == "name" || key == "instance_name") {
        serial.instance_name = val;
        return true;
    }
    if (key == "device") {
        serial.device = val;
        return true;
    }
    if (key == "baudrate") {
        if (!toInt(val, i)) {
            err = "invalid serials.baudrate";
            return false;
        }
        serial.baudrate = i;
        return true;
    }
    if (key == "data_bits") {
        if (!toInt(val, i)) {
            err = "invalid serials.data_bits";
            return false;
        }
        serial.data_bits = i;
        return true;
    }
    if (key == "stop_bits") {
        if (!toInt(val, i)) {
            err = "invalid serials.stop_bits";
            return false;
        }
        serial.stop_bits = i;
        return true;
    }
    if (key == "parity") {
        if (val.empty()) {
            err = "invalid serials.parity";
            return false;
        }
        serial.parity = val[0];
        return true;
    }
    if (key == "read_chunk_size") {
        if (!toSize(val, z)) {
            err = "invalid serials.read_chunk_size";
            return false;
        }
        serial.read_chunk_size = z;
        return true;
    }

    err = "unknown serials key: " + key;
    return false;
}

} // namespace

bool ConfigLoader::loadFromFile(const std::string& path, GatewayConfig& out, std::string& err) {
    std::ifstream fin(path);
    if (!fin) {
        err = "cannot open config file: " + path;
        return false;
    }

    std::unordered_map<std::string, std::string> kv;
    std::string section;
    std::string line;
    int line_no = 0;

    std::vector<DeviceConfig> parsed_devices;
    std::vector<GatewayConfig::SerialInstanceConfig> parsed_serials;
    int current_device_idx = -1;
    int current_serial_idx = -1;

    while (std::getline(fin, line)) {
        ++line_no;

        const auto hash_pos = line.find('#');
        if (hash_pos != std::string::npos) {
            line = line.substr(0, hash_pos);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        if (line.back() == ':' && line.find(' ') == std::string::npos) {
            section = trim(line.substr(0, line.size() - 1));
            if (section != "devices" && section != "serials") {
                current_device_idx = -1;
                current_serial_idx = -1;
            }
            continue;
        }

        if (section == "devices") {
            if (line[0] == '-') {
                parsed_devices.push_back(DeviceConfig{});
                current_device_idx = static_cast<int>(parsed_devices.size()) - 1;

                const std::string inline_item = trim(line.substr(1));
                if (!inline_item.empty()) {
                    std::string key;
                    std::string val;
                    if (!parseKeyValue(inline_item, key, val)) {
                        err = "invalid devices item at line " + std::to_string(line_no);
                        return false;
                    }
                    if (!applyDeviceValue(parsed_devices[static_cast<std::size_t>(current_device_idx)], key, val, err)) {
                        return false;
                    }
                }
                continue;
            }

            if (current_device_idx < 0) {
                err = "devices fields must follow '-' item at line " + std::to_string(line_no);
                return false;
            }

            std::string key;
            std::string val;
            if (!parseKeyValue(line, key, val)) {
                err = "invalid devices line " + std::to_string(line_no) + ": " + line;
                return false;
            }
            if (!applyDeviceValue(parsed_devices[static_cast<std::size_t>(current_device_idx)], key, val, err)) {
                return false;
            }
            continue;
        }

        if (section == "serials") {
            if (line[0] == '-') {
                parsed_serials.push_back(GatewayConfig::SerialInstanceConfig{});
                current_serial_idx = static_cast<int>(parsed_serials.size()) - 1;

                const std::string inline_item = trim(line.substr(1));
                if (!inline_item.empty()) {
                    std::string key;
                    std::string val;
                    if (!parseKeyValue(inline_item, key, val)) {
                        err = "invalid serials item at line " + std::to_string(line_no);
                        return false;
                    }
                    if (!applySerialValue(parsed_serials[static_cast<std::size_t>(current_serial_idx)].serial, key, val, err)) {
                        return false;
                    }
                }
                continue;
            }

            if (current_serial_idx < 0) {
                err = "serials fields must follow '-' item at line " + std::to_string(line_no);
                return false;
            }

            std::string key;
            std::string val;
            if (!parseKeyValue(line, key, val)) {
                err = "invalid serials line " + std::to_string(line_no) + ": " + line;
                return false;
            }
            if (!applySerialValue(parsed_serials[static_cast<std::size_t>(current_serial_idx)].serial, key, val, err)) {
                return false;
            }
            continue;
        }

        std::string key;
        std::string val;
        if (!parseKeyValue(line, key, val)) {
            err = "invalid config line " + std::to_string(line_no) + ": " + line;
            return false;
        }

        if (section.empty()) {
            err = "key outside section at line " + std::to_string(line_no);
            return false;
        }

        kv[section + "." + key] = val;
    }

    if (kv.count("serial.device") != 0U) out.serial.device = kv["serial.device"];
    if (kv.count("serial.instance_name") != 0U) out.serial.instance_name = kv["serial.instance_name"];
    if (kv.count("serial.parity") != 0U && !kv["serial.parity"].empty()) out.serial.parity = kv["serial.parity"][0];

    int i = 0;
    std::size_t z = 0;
    bool b = false;

    if (kv.count("serial.baudrate") != 0U && !toInt(kv["serial.baudrate"], i)) { err = "invalid serial.baudrate"; return false; }
    if (kv.count("serial.baudrate") != 0U) out.serial.baudrate = i;

    if (kv.count("serial.data_bits") != 0U && !toInt(kv["serial.data_bits"], i)) { err = "invalid serial.data_bits"; return false; }
    if (kv.count("serial.data_bits") != 0U) out.serial.data_bits = i;

    if (kv.count("serial.stop_bits") != 0U && !toInt(kv["serial.stop_bits"], i)) { err = "invalid serial.stop_bits"; return false; }
    if (kv.count("serial.stop_bits") != 0U) out.serial.stop_bits = i;

    if (kv.count("serial.read_chunk_size") != 0U && !toSize(kv["serial.read_chunk_size"], z)) { err = "invalid serial.read_chunk_size"; return false; }
    if (kv.count("serial.read_chunk_size") != 0U) out.serial.read_chunk_size = z;
    if (out.serial.instance_name.empty()) {
        out.serial.instance_name = "default";
    }

    if (kv.count("uploader.host") != 0U) out.uploader.host = kv["uploader.host"];
    if (kv.count("uploader.type") != 0U) out.uploader.type = kv["uploader.type"];
    if (kv.count("uploader.mqtt_topic") != 0U) out.uploader.mqtt_topic = kv["uploader.mqtt_topic"];
    if (kv.count("uploader.mqtt_heartbeat_topic") != 0U) out.uploader.mqtt_heartbeat_topic = kv["uploader.mqtt_heartbeat_topic"];
    if (kv.count("uploader.mqtt_status_topic") != 0U) out.uploader.mqtt_status_topic = kv["uploader.mqtt_status_topic"];
    if (kv.count("uploader.mqtt_event_topic") != 0U) out.uploader.mqtt_event_topic = kv["uploader.mqtt_event_topic"];
    if (kv.count("uploader.mqtt_command_ack_topic") != 0U) out.uploader.mqtt_command_ack_topic = kv["uploader.mqtt_command_ack_topic"];
    if (kv.count("uploader.mqtt_ota_status_topic") != 0U) out.uploader.mqtt_ota_status_topic = kv["uploader.mqtt_ota_status_topic"];
    if (kv.count("uploader.mqtt_command_down_topic") != 0U) out.uploader.mqtt_command_down_topic = kv["uploader.mqtt_command_down_topic"];
    if (kv.count("uploader.mqtt_ota_start_topic") != 0U) out.uploader.mqtt_ota_start_topic = kv["uploader.mqtt_ota_start_topic"];
    if (kv.count("uploader.mqtt_control_client_id") != 0U) out.uploader.mqtt_control_client_id = kv["uploader.mqtt_control_client_id"];
    if (kv.count("uploader.mqtt_client_id") != 0U) out.uploader.mqtt_client_id = kv["uploader.mqtt_client_id"];
    if (kv.count("uploader.mqtt_username") != 0U) out.uploader.mqtt_username = kv["uploader.mqtt_username"];
    if (kv.count("uploader.mqtt_password") != 0U) out.uploader.mqtt_password = kv["uploader.mqtt_password"];
    if (kv.count("uploader.disk_cache_file") != 0U) out.uploader.disk_cache_file = kv["uploader.disk_cache_file"];
    if (kv.count("uploader.disk_cache_max_lines") != 0U && !toSize(kv["uploader.disk_cache_max_lines"], z)) { err = "invalid uploader.disk_cache_max_lines"; return false; }
    if (kv.count("uploader.disk_cache_max_lines") != 0U) out.uploader.disk_cache_max_lines = z;
    if (kv.count("uploader.replay_batch_size") != 0U && !toSize(kv["uploader.replay_batch_size"], z)) { err = "invalid uploader.replay_batch_size"; return false; }
    if (kv.count("uploader.replay_batch_size") != 0U) out.uploader.replay_batch_size = z;

    if (kv.count("uploader.port") != 0U && !toInt(kv["uploader.port"], i)) { err = "invalid uploader.port"; return false; }
    if (kv.count("uploader.port") != 0U) out.uploader.port = i;

    if (kv.count("uploader.connect_timeout_ms") != 0U && !toInt(kv["uploader.connect_timeout_ms"], i)) { err = "invalid uploader.connect_timeout_ms"; return false; }
    if (kv.count("uploader.connect_timeout_ms") != 0U) out.uploader.connect_timeout_ms = i;

    if (kv.count("uploader.reconnect_initial_ms") != 0U && !toInt(kv["uploader.reconnect_initial_ms"], i)) { err = "invalid uploader.reconnect_initial_ms"; return false; }
    if (kv.count("uploader.reconnect_initial_ms") != 0U) out.uploader.reconnect_initial_ms = i;

    if (kv.count("uploader.reconnect_max_ms") != 0U && !toInt(kv["uploader.reconnect_max_ms"], i)) { err = "invalid uploader.reconnect_max_ms"; return false; }
    if (kv.count("uploader.reconnect_max_ms") != 0U) out.uploader.reconnect_max_ms = i;
    if (kv.count("uploader.replay_pause_ms") != 0U && !toInt(kv["uploader.replay_pause_ms"], i)) { err = "invalid uploader.replay_pause_ms"; return false; }
    if (kv.count("uploader.replay_pause_ms") != 0U) out.uploader.replay_pause_ms = i;
    if (kv.count("uploader.mqtt_qos") != 0U && !toInt(kv["uploader.mqtt_qos"], i)) { err = "invalid uploader.mqtt_qos"; return false; }
    if (kv.count("uploader.mqtt_qos") != 0U) out.uploader.mqtt_qos = i;
    if (kv.count("uploader.mqtt_keepalive_sec") != 0U && !toInt(kv["uploader.mqtt_keepalive_sec"], i)) { err = "invalid uploader.mqtt_keepalive_sec"; return false; }
    if (kv.count("uploader.mqtt_keepalive_sec") != 0U) out.uploader.mqtt_keepalive_sec = i;
    if (kv.count("uploader.mqtt_max_inflight") != 0U && !toInt(kv["uploader.mqtt_max_inflight"], i)) { err = "invalid uploader.mqtt_max_inflight"; return false; }
    if (kv.count("uploader.mqtt_max_inflight") != 0U) out.uploader.mqtt_max_inflight = i;
    if (kv.count("uploader.mqtt_clean_session") != 0U && !toBool(kv["uploader.mqtt_clean_session"], b)) { err = "invalid uploader.mqtt_clean_session"; return false; }
    if (kv.count("uploader.mqtt_clean_session") != 0U) out.uploader.mqtt_clean_session = b;
    if (kv.count("uploader.mqtt_control_enabled") != 0U && !toBool(kv["uploader.mqtt_control_enabled"], b)) { err = "invalid uploader.mqtt_control_enabled"; return false; }
    if (kv.count("uploader.mqtt_control_enabled") != 0U) out.uploader.mqtt_control_enabled = b;

    if (kv.count("uploader.disk_cache_enabled") != 0U && !toBool(kv["uploader.disk_cache_enabled"], b)) { err = "invalid uploader.disk_cache_enabled"; return false; }
    if (kv.count("uploader.disk_cache_enabled") != 0U) out.uploader.disk_cache_enabled = b;
    if (out.uploader.replay_pause_ms < 0) { err = "invalid uploader.replay_pause_ms, expected >= 0"; return false; }
    if (out.uploader.mqtt_qos < 0 || out.uploader.mqtt_qos > 2) { err = "invalid uploader.mqtt_qos, expected 0..2"; return false; }
    if (out.uploader.mqtt_keepalive_sec <= 0) { err = "invalid uploader.mqtt_keepalive_sec, expected > 0"; return false; }
    if (out.uploader.mqtt_max_inflight <= 0) { err = "invalid uploader.mqtt_max_inflight, expected > 0"; return false; }

    if (kv.count("runtime.queue_capacity") != 0U && !toSize(kv["runtime.queue_capacity"], z)) { err = "invalid runtime.queue_capacity"; return false; }
    if (kv.count("runtime.queue_capacity") != 0U) out.runtime.queue_capacity = z;

    if (kv.count("runtime.stats_interval_sec") != 0U && !toInt(kv["runtime.stats_interval_sec"], i)) { err = "invalid runtime.stats_interval_sec"; return false; }
    if (kv.count("runtime.stats_interval_sec") != 0U) out.runtime.stats_interval_sec = i;

    if (kv.count("runtime.drop_unknown_devices") != 0U && !toBool(kv["runtime.drop_unknown_devices"], b)) { err = "invalid runtime.drop_unknown_devices"; return false; }
    if (kv.count("runtime.drop_unknown_devices") != 0U) out.runtime.drop_unknown_devices = b;
    if (kv.count("runtime.device_offline_timeout_sec") != 0U && !toInt(kv["runtime.device_offline_timeout_sec"], i)) { err = "invalid runtime.device_offline_timeout_sec"; return false; }
    if (kv.count("runtime.device_offline_timeout_sec") != 0U) out.runtime.device_offline_timeout_sec = i;
    if (kv.count("runtime.cache_backlog_warn_threshold") != 0U && !toSize(kv["runtime.cache_backlog_warn_threshold"], z)) { err = "invalid runtime.cache_backlog_warn_threshold"; return false; }
    if (kv.count("runtime.cache_backlog_warn_threshold") != 0U) out.runtime.cache_backlog_warn_threshold = z;
    if (out.runtime.device_offline_timeout_sec <= 0) { err = "invalid runtime.device_offline_timeout_sec, expected > 0"; return false; }

    if (kv.count("command.bind_host") != 0U) out.command.bind_host = kv["command.bind_host"];
    if (kv.count("command.enabled") != 0U && !toBool(kv["command.enabled"], b)) { err = "invalid command.enabled"; return false; }
    if (kv.count("command.enabled") != 0U) out.command.enabled = b;
    if (kv.count("command.port") != 0U && !toInt(kv["command.port"], i)) { err = "invalid command.port"; return false; }
    if (kv.count("command.port") != 0U) out.command.port = i;
    if (kv.count("command.client_timeout_ms") != 0U && !toInt(kv["command.client_timeout_ms"], i)) { err = "invalid command.client_timeout_ms"; return false; }
    if (kv.count("command.client_timeout_ms") != 0U) out.command.client_timeout_ms = i;
    if (out.command.port <= 0 || out.command.port > 65535) { err = "invalid command.port range"; return false; }
    if (out.command.client_timeout_ms <= 0) { err = "invalid command.client_timeout_ms, expected > 0"; return false; }

    if (kv.count("heartbeat.gateway_id") != 0U) out.heartbeat.gateway_id = kv["heartbeat.gateway_id"];
    if (kv.count("heartbeat.enabled") != 0U && !toBool(kv["heartbeat.enabled"], b)) { err = "invalid heartbeat.enabled"; return false; }
    if (kv.count("heartbeat.enabled") != 0U) out.heartbeat.enabled = b;
    if (kv.count("heartbeat.interval_sec") != 0U && !toInt(kv["heartbeat.interval_sec"], i)) { err = "invalid heartbeat.interval_sec"; return false; }
    if (kv.count("heartbeat.interval_sec") != 0U) out.heartbeat.interval_sec = i;
    if (out.heartbeat.interval_sec <= 0) { err = "invalid heartbeat.interval_sec, expected > 0"; return false; }

    if (kv.count("reload.enabled") != 0U && !toBool(kv["reload.enabled"], b)) { err = "invalid reload.enabled"; return false; }
    if (kv.count("reload.enabled") != 0U) out.reload.enabled = b;
    if (kv.count("reload.check_interval_sec") != 0U && !toInt(kv["reload.check_interval_sec"], i)) { err = "invalid reload.check_interval_sec"; return false; }
    if (kv.count("reload.check_interval_sec") != 0U) out.reload.check_interval_sec = i;
    if (out.reload.check_interval_sec <= 0) { err = "invalid reload.check_interval_sec, expected > 0"; return false; }

    if (kv.count("monitor.bind_host") != 0U) out.monitor.bind_host = kv["monitor.bind_host"];
    if (kv.count("monitor.enabled") != 0U && !toBool(kv["monitor.enabled"], b)) { err = "invalid monitor.enabled"; return false; }
    if (kv.count("monitor.enabled") != 0U) out.monitor.enabled = b;
    if (kv.count("monitor.port") != 0U && !toInt(kv["monitor.port"], i)) { err = "invalid monitor.port"; return false; }
    if (kv.count("monitor.port") != 0U) out.monitor.port = i;
    if (kv.count("monitor.recent_capacity") != 0U && !toSize(kv["monitor.recent_capacity"], z)) { err = "invalid monitor.recent_capacity"; return false; }
    if (kv.count("monitor.recent_capacity") != 0U) out.monitor.recent_capacity = z;
    if (out.monitor.port <= 0 || out.monitor.port > 65535) { err = "invalid monitor.port range"; return false; }
    if (out.monitor.recent_capacity == 0) { err = "invalid monitor.recent_capacity, expected > 0"; return false; }

    if (kv.count("wifi_device_server.listen_host") != 0U) out.wifi_device_server.listen_host = kv["wifi_device_server.listen_host"];
    if (kv.count("wifi_device_server.enabled") != 0U && !toBool(kv["wifi_device_server.enabled"], b)) { err = "invalid wifi_device_server.enabled"; return false; }
    if (kv.count("wifi_device_server.enabled") != 0U) out.wifi_device_server.enabled = b;
    if (kv.count("wifi_device_server.listen_port") != 0U && !toInt(kv["wifi_device_server.listen_port"], i)) { err = "invalid wifi_device_server.listen_port"; return false; }
    if (kv.count("wifi_device_server.listen_port") != 0U) out.wifi_device_server.listen_port = i;
    if (out.wifi_device_server.listen_port <= 0 || out.wifi_device_server.listen_port > 65535) { err = "invalid wifi_device_server.listen_port range"; return false; }

    if (kv.count("mqtt_device_ingress.host") != 0U) out.mqtt_device_ingress.host = kv["mqtt_device_ingress.host"];
    if (kv.count("mqtt_device_ingress.client_id") != 0U) out.mqtt_device_ingress.client_id = kv["mqtt_device_ingress.client_id"];
    if (kv.count("mqtt_device_ingress.username") != 0U) out.mqtt_device_ingress.username = kv["mqtt_device_ingress.username"];
    if (kv.count("mqtt_device_ingress.password") != 0U) out.mqtt_device_ingress.password = kv["mqtt_device_ingress.password"];
    if (kv.count("mqtt_device_ingress.gateway_id") != 0U) out.mqtt_device_ingress.gateway_id = kv["mqtt_device_ingress.gateway_id"];
    if (kv.count("mqtt_device_ingress.topic_prefix") != 0U) out.mqtt_device_ingress.topic_prefix = kv["mqtt_device_ingress.topic_prefix"];
    if (kv.count("mqtt_device_ingress.enabled") != 0U && !toBool(kv["mqtt_device_ingress.enabled"], b)) { err = "invalid mqtt_device_ingress.enabled"; return false; }
    if (kv.count("mqtt_device_ingress.enabled") != 0U) out.mqtt_device_ingress.enabled = b;
    if (kv.count("mqtt_device_ingress.port") != 0U && !toInt(kv["mqtt_device_ingress.port"], i)) { err = "invalid mqtt_device_ingress.port"; return false; }
    if (kv.count("mqtt_device_ingress.port") != 0U) out.mqtt_device_ingress.port = i;
    if (kv.count("mqtt_device_ingress.keepalive_sec") != 0U && !toInt(kv["mqtt_device_ingress.keepalive_sec"], i)) { err = "invalid mqtt_device_ingress.keepalive_sec"; return false; }
    if (kv.count("mqtt_device_ingress.keepalive_sec") != 0U) out.mqtt_device_ingress.keepalive_sec = i;
    if (kv.count("mqtt_device_ingress.clean_session") != 0U && !toBool(kv["mqtt_device_ingress.clean_session"], b)) { err = "invalid mqtt_device_ingress.clean_session"; return false; }
    if (kv.count("mqtt_device_ingress.clean_session") != 0U) out.mqtt_device_ingress.clean_session = b;
    if (kv.count("mqtt_device_ingress.reconnect_initial_ms") != 0U && !toInt(kv["mqtt_device_ingress.reconnect_initial_ms"], i)) { err = "invalid mqtt_device_ingress.reconnect_initial_ms"; return false; }
    if (kv.count("mqtt_device_ingress.reconnect_initial_ms") != 0U) out.mqtt_device_ingress.reconnect_initial_ms = i;
    if (kv.count("mqtt_device_ingress.reconnect_max_ms") != 0U && !toInt(kv["mqtt_device_ingress.reconnect_max_ms"], i)) { err = "invalid mqtt_device_ingress.reconnect_max_ms"; return false; }
    if (kv.count("mqtt_device_ingress.reconnect_max_ms") != 0U) out.mqtt_device_ingress.reconnect_max_ms = i;
    if (out.mqtt_device_ingress.port <= 0 || out.mqtt_device_ingress.port > 65535) { err = "invalid mqtt_device_ingress.port range"; return false; }
    if (out.mqtt_device_ingress.keepalive_sec <= 0) { err = "invalid mqtt_device_ingress.keepalive_sec, expected > 0"; return false; }
    if (out.mqtt_device_ingress.reconnect_initial_ms <= 0) { err = "invalid mqtt_device_ingress.reconnect_initial_ms, expected > 0"; return false; }
    if (out.mqtt_device_ingress.reconnect_max_ms <= 0) { err = "invalid mqtt_device_ingress.reconnect_max_ms, expected > 0"; return false; }

    if (kv.count("tcp_binary.listen_host") != 0U) out.tcp_binary.listen_host = kv["tcp_binary.listen_host"];
    if (kv.count("tcp_binary.enabled") != 0U && !toBool(kv["tcp_binary.enabled"], b)) { err = "invalid tcp_binary.enabled"; return false; }
    if (kv.count("tcp_binary.enabled") != 0U) out.tcp_binary.enabled = b;
    if (kv.count("tcp_binary.port") != 0U && !toInt(kv["tcp_binary.port"], i)) { err = "invalid tcp_binary.port"; return false; }
    if (kv.count("tcp_binary.port") != 0U) out.tcp_binary.port = i;
    if (kv.count("tcp_binary.name") != 0U) out.tcp_binary.name = kv["tcp_binary.name"];
    if (kv.count("tcp_binary.priority") != 0U && !toInt(kv["tcp_binary.priority"], i)) { err = "invalid tcp_binary.priority"; return false; }
    if (kv.count("tcp_binary.priority") != 0U) out.tcp_binary.priority = i;
    if (kv.count("tcp_binary.heartbeat_timeout_ms") != 0U && !toInt(kv["tcp_binary.heartbeat_timeout_ms"], i)) { err = "invalid tcp_binary.heartbeat_timeout_ms"; return false; }
    if (kv.count("tcp_binary.heartbeat_timeout_ms") != 0U) out.tcp_binary.heartbeat_timeout_ms = i;
    if (out.tcp_binary.port <= 0 || out.tcp_binary.port > 65535) { err = "invalid tcp_binary.port range"; return false; }
    if (out.tcp_binary.priority < 0) { err = "invalid tcp_binary.priority, expected >= 0"; return false; }
    if (out.tcp_binary.heartbeat_timeout_ms <= 0) { err = "invalid tcp_binary.heartbeat_timeout_ms, expected > 0"; return false; }

    if (kv.count("log.file") != 0U) out.log.file = kv["log.file"];
    if (kv.count("log.level") != 0U) out.log.level = kv["log.level"];

    if (kv.count("log.also_stdout") != 0U && !toBool(kv["log.also_stdout"], b)) { err = "invalid log.also_stdout"; return false; }
    if (kv.count("log.also_stdout") != 0U) out.log.also_stdout = b;

    std::transform(out.uploader.type.begin(), out.uploader.type.end(), out.uploader.type.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (out.uploader.type != "tcp" && out.uploader.type != "mqtt") {
        err = "invalid uploader.type, expected tcp or mqtt";
        return false;
    }

    std::unordered_set<int> id_set;
    for (auto& dev : parsed_devices) {
        if (dev.name.empty()) {
            err = "devices.name is required";
            return false;
        }
        if (!id_set.insert(static_cast<int>(dev.id)).second) {
            err = "duplicated devices.id";
            return false;
        }
        if (dev.link_type.empty()) {
            dev.link_type = "serial";
        }
        std::transform(dev.link_type.begin(), dev.link_type.end(), dev.link_type.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (dev.link_type != "serial" && dev.link_type != "wifi" && dev.link_type != "mqtt") {
            err = "invalid devices.link_type, expected serial or wifi or mqtt";
            return false;
        }
    }

    std::unordered_set<std::string> serial_name_set;
    for (std::size_t idx = 0; idx < parsed_serials.size(); ++idx) {
        auto& item = parsed_serials[idx];
        if (item.serial.device.empty()) {
            err = "serials.device is required";
            return false;
        }
        if (item.serial.instance_name.empty()) {
            item.serial.instance_name = "serial-" + std::to_string(idx);
        }
        item.name = item.serial.instance_name;
        if (!serial_name_set.insert(item.name).second) {
            err = "duplicated serials.instance_name";
            return false;
        }
    }

    out.devices = std::move(parsed_devices);
    out.serials = std::move(parsed_serials);
    return true;
}

} // namespace sg
