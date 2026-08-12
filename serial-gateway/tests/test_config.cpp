#include "config/config.hpp"

#include <fstream>
#include <iostream>

namespace {

bool testValidConfig() {
    const std::string path = "/tmp/sg_test_config.yaml";
    std::ofstream fout(path);
    fout << "serial:\n";
    fout << "  instance_name: \"primary\"\n";
    fout << "  device: \"/tmp/ttyTest\"\n";
    fout << "  baudrate: 9600\n";
    fout << "serials:\n";
    fout << "  - instance_name: \"line-a\"\n";
    fout << "    device: \"/tmp/ttyA\"\n";
    fout << "    baudrate: 115200\n";
    fout << "  - instance_name: \"line-b\"\n";
    fout << "    device: \"/tmp/ttyB\"\n";
    fout << "    baudrate: 57600\n";
    fout << "uploader:\n";
    fout << "  type: \"tcp\"\n";
    fout << "  host: \"127.0.0.1\"\n";
    fout << "  port: 9100\n";
    fout << "  mqtt_topic: \"sensors/test\"\n";
    fout << "  mqtt_heartbeat_topic: \"gateway/hb\"\n";
    fout << "  mqtt_status_topic: \"gateway/status\"\n";
    fout << "  mqtt_event_topic: \"gateway/event\"\n";
    fout << "  mqtt_command_ack_topic: \"gateway/command_ack\"\n";
    fout << "  mqtt_ota_status_topic: \"gateway/ota_status\"\n";
    fout << "  mqtt_control_enabled: true\n";
    fout << "  mqtt_command_down_topic: \"gateway/command/down\"\n";
    fout << "  mqtt_ota_start_topic: \"gateway/ota/start\"\n";
    fout << "  mqtt_control_client_id: \"sg-control\"\n";
    fout << "  mqtt_client_id: \"sg-test\"\n";
    fout << "  mqtt_username: \"user1\"\n";
    fout << "  mqtt_password: \"pass1\"\n";
    fout << "  mqtt_qos: 2\n";
    fout << "  mqtt_keepalive_sec: 30\n";
    fout << "  mqtt_clean_session: false\n";
    fout << "  mqtt_max_inflight: 50\n";
    fout << "  disk_cache_enabled: true\n";
    fout << "  disk_cache_file: \"cache/test_pending.log\"\n";
    fout << "  disk_cache_max_lines: 321\n";
    fout << "  replay_batch_size: 15\n";
    fout << "  replay_pause_ms: 2\n";
    fout << "runtime:\n";
    fout << "  queue_capacity: 64\n";
    fout << "  drop_unknown_devices: true\n";
    fout << "  device_offline_timeout_sec: 15\n";
    fout << "  cache_backlog_warn_threshold: 999\n";
    fout << "command:\n";
    fout << "  enabled: true\n";
    fout << "  bind_host: \"127.0.0.1\"\n";
    fout << "  port: 9901\n";
    fout << "  client_timeout_ms: 300\n";
    fout << "heartbeat:\n";
    fout << "  enabled: true\n";
    fout << "  interval_sec: 7\n";
    fout << "  gateway_id: \"gw-01\"\n";
    fout << "reload:\n";
    fout << "  enabled: true\n";
    fout << "  check_interval_sec: 4\n";
    fout << "monitor:\n";
    fout << "  enabled: true\n";
    fout << "  bind_host: \"127.0.0.1\"\n";
    fout << "  port: 9910\n";
    fout << "  recent_capacity: 77\n";
    fout << "wifi_device_server:\n";
    fout << "  enabled: true\n";
    fout << "  listen_host: \"0.0.0.0\"\n";
    fout << "  listen_port: 9100\n";
    fout << "tcp_binary:\n";
    fout << "  enabled: true\n";
    fout << "  listen_host: \"0.0.0.0\"\n";
    fout << "  port: 9101\n";
    fout << "  name: \"stm32_wifi_bridge\"\n";
    fout << "  priority: 100\n";
    fout << "  heartbeat_timeout_ms: 10000\n";
    fout << "devices:\n";
    fout << "  - device_id: 1\n";
    fout << "    name: \"alpha\"\n";
    fout << "    enabled: true\n";
    fout << "    link_type: \"serial\"\n";
    fout << "    topic_suffix: \"a/1\"\n";
    fout << "  - device_id: 2\n";
    fout << "    name: \"beta\"\n";
    fout << "    enabled: false\n";
    fout << "    link_type: \"wifi\"\n";
    fout << "log:\n";
    fout << "  level: \"DEBUG\"\n";
    fout << "  also_stdout: false\n";
    fout.close();

    sg::GatewayConfig cfg;
    std::string err;
    if (!sg::ConfigLoader::loadFromFile(path, cfg, err)) {
        std::cerr << "config load failed: " << err << "\n";
        return false;
    }

    if (cfg.serial.instance_name != "primary" || cfg.serial.device != "/tmp/ttyTest" || cfg.serial.baudrate != 9600 ||
        cfg.serials.size() != 2 || cfg.serials[0].serial.instance_name != "line-a" ||
        cfg.serials[0].serial.device != "/tmp/ttyA" || cfg.serials[0].serial.baudrate != 115200 ||
        cfg.serials[1].serial.instance_name != "line-b" || cfg.serials[1].serial.device != "/tmp/ttyB" ||
        cfg.serials[1].serial.baudrate != 57600 || cfg.uploader.type != "tcp" ||
        cfg.uploader.port != 9100 || cfg.uploader.mqtt_topic != "sensors/test" ||
        cfg.uploader.mqtt_heartbeat_topic != "gateway/hb" ||
        cfg.uploader.mqtt_status_topic != "gateway/status" ||
        cfg.uploader.mqtt_event_topic != "gateway/event" ||
        cfg.uploader.mqtt_command_ack_topic != "gateway/command_ack" ||
        cfg.uploader.mqtt_ota_status_topic != "gateway/ota_status" ||
        !cfg.uploader.mqtt_control_enabled ||
        cfg.uploader.mqtt_command_down_topic != "gateway/command/down" ||
        cfg.uploader.mqtt_ota_start_topic != "gateway/ota/start" ||
        cfg.uploader.mqtt_control_client_id != "sg-control" ||
        cfg.uploader.mqtt_client_id != "sg-test" || cfg.uploader.mqtt_username != "user1" ||
        cfg.uploader.mqtt_password != "pass1" || cfg.uploader.mqtt_qos != 2 ||
        cfg.uploader.mqtt_keepalive_sec != 30 || cfg.uploader.mqtt_clean_session ||
        cfg.uploader.mqtt_max_inflight != 50 || !cfg.uploader.disk_cache_enabled ||
        cfg.uploader.disk_cache_file != "cache/test_pending.log" || cfg.uploader.disk_cache_max_lines != 321 ||
        cfg.uploader.replay_batch_size != 15 || cfg.uploader.replay_pause_ms != 2 ||
        cfg.runtime.queue_capacity != 64 ||
        !cfg.runtime.drop_unknown_devices || cfg.runtime.device_offline_timeout_sec != 15 ||
        cfg.runtime.cache_backlog_warn_threshold != 999 ||
        !cfg.command.enabled || cfg.command.bind_host != "127.0.0.1" ||
        cfg.command.port != 9901 || cfg.command.client_timeout_ms != 300 ||
        !cfg.heartbeat.enabled || cfg.heartbeat.interval_sec != 7 || cfg.heartbeat.gateway_id != "gw-01" ||
        !cfg.reload.enabled || cfg.reload.check_interval_sec != 4 ||
        !cfg.monitor.enabled || cfg.monitor.bind_host != "127.0.0.1" || cfg.monitor.port != 9910 ||
        cfg.monitor.recent_capacity != 77 ||
        !cfg.wifi_device_server.enabled || cfg.wifi_device_server.listen_host != "0.0.0.0" ||
        cfg.wifi_device_server.listen_port != 9100 ||
        !cfg.tcp_binary.enabled || cfg.tcp_binary.listen_host != "0.0.0.0" ||
        cfg.tcp_binary.port != 9101 || cfg.tcp_binary.name != "stm32_wifi_bridge" ||
        cfg.tcp_binary.priority != 100 || cfg.tcp_binary.heartbeat_timeout_ms != 10000 ||
        cfg.log.level != "DEBUG" || cfg.log.also_stdout ||
        cfg.devices.size() != 2 || cfg.devices[0].id != 1 || cfg.devices[0].name != "alpha" ||
        !cfg.devices[0].enabled || cfg.devices[0].topic_suffix != "a/1" || cfg.devices[0].link_type != "serial" ||
        cfg.devices[1].id != 2 || cfg.devices[1].name != "beta" || cfg.devices[1].enabled ||
        cfg.devices[1].link_type != "wifi") {
        std::cerr << "config values mismatch\n";
        return false;
    }

    return true;
}

bool testDuplicateSerialNameRejected() {
    const std::string path = "/tmp/sg_test_config_dup_serial.yaml";
    std::ofstream fout(path);
    fout << "serials:\n";
    fout << "  - instance_name: \"line-a\"\n";
    fout << "    device: \"/tmp/ttyA\"\n";
    fout << "  - instance_name: \"line-a\"\n";
    fout << "    device: \"/tmp/ttyB\"\n";
    fout.close();

    sg::GatewayConfig cfg;
    std::string err;
    if (sg::ConfigLoader::loadFromFile(path, cfg, err)) {
        std::cerr << "duplicate serial name should fail\n";
        return false;
    }
    return true;
}

bool testDuplicateDeviceIdRejected() {
    const std::string path = "/tmp/sg_test_config_dup.yaml";
    std::ofstream fout(path);
    fout << "devices:\n";
    fout << "  - id: 1\n";
    fout << "    name: \"alpha\"\n";
    fout << "  - id: 1\n";
    fout << "    name: \"beta\"\n";
    fout.close();

    sg::GatewayConfig cfg;
    std::string err;
    if (sg::ConfigLoader::loadFromFile(path, cfg, err)) {
        std::cerr << "duplicate id should fail\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!testValidConfig()) {
        return 1;
    }
    if (!testDuplicateDeviceIdRejected()) {
        return 2;
    }
    if (!testDuplicateSerialNameRejected()) {
        return 3;
    }
    return 0;
}
