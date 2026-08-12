#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sg {

struct SerialConfig {
    std::string instance_name = "default";
    std::string device = "/dev/ttyUSB0";
    int baudrate = 115200;
    int data_bits = 8;
    char parity = 'N';
    int stop_bits = 1;
    std::size_t read_chunk_size = 256;
};

struct UploaderConfig {
    std::string type = "tcp";
    std::string host = "127.0.0.1";
    int port = 9000;
    std::string mqtt_topic = "sensors/data";
    std::string mqtt_heartbeat_topic = "gateway/heartbeat";
    std::string mqtt_status_topic;
    std::string mqtt_event_topic;
    std::string mqtt_command_ack_topic;
    std::string mqtt_ota_status_topic;
    bool mqtt_control_enabled = false;
    std::string mqtt_command_down_topic;
    std::string mqtt_ota_start_topic;
    std::string mqtt_control_client_id = "serial-gateway-upstream-control";
    std::string mqtt_client_id = "serial-gateway";
    std::string mqtt_username;
    std::string mqtt_password;
    int mqtt_qos = 0;
    int mqtt_keepalive_sec = 60;
    bool mqtt_clean_session = true;
    int mqtt_max_inflight = 20;
    bool disk_cache_enabled = true;
    std::string disk_cache_file = "cache/pending_records.log";
    std::size_t disk_cache_max_lines = 10000;
    std::size_t replay_batch_size = 200;
    int replay_pause_ms = 0;
    int connect_timeout_ms = 1000;
    int reconnect_initial_ms = 500;
    int reconnect_max_ms = 5000;
};

struct RuntimeConfig {
    std::size_t queue_capacity = 1024;
    int stats_interval_sec = 5;
    bool drop_unknown_devices = false;
    int device_offline_timeout_sec = 30;
    std::size_t cache_backlog_warn_threshold = 2000;
};

struct CommandConfig {
    bool enabled = false;
    std::string bind_host = "127.0.0.1";
    int port = 9001;
    int client_timeout_ms = 200;
};

struct HeartbeatConfig {
    bool enabled = true;
    int interval_sec = 10;
    std::string gateway_id = "serial-gateway";
};

struct ReloadConfig {
    bool enabled = false;
    int check_interval_sec = 5;
};

struct MonitorConfig {
    bool enabled = true;
    std::string bind_host = "127.0.0.1";
    int port = 9010;
    std::size_t recent_capacity = 100;
};

struct DeviceConfig {
    std::uint8_t id = 0;
    std::string name;
    bool enabled = true;
    std::string topic_suffix;
    std::string link_type = "serial";
};

struct WifiDeviceServerConfig {
    bool enabled = false;
    std::string listen_host = "0.0.0.0";
    int listen_port = 9100;
};

struct MqttDeviceIngressConfig {
    bool enabled = false;
    std::string host = "127.0.0.1";
    int port = 1883;
    std::string client_id = "serial-gateway-device-ingress";
    std::string username;
    std::string password;
    std::string gateway_id = "gw001";
    std::string topic_prefix = "gateway";
    int keepalive_sec = 60;
    bool clean_session = true;
    int reconnect_initial_ms = 1000;
    int reconnect_max_ms = 5000;
};

struct TcpBinaryConfig {
    bool enabled = false;
    std::string listen_host = "0.0.0.0";
    int port = 9101;
    std::string name = "stm32_wifi_bridge";
    int priority = 100;
    int heartbeat_timeout_ms = 10000;
};

struct LogConfig {
    std::string file = "logs/serial_gateway.log";
    std::string level = "INFO";
    bool also_stdout = true;
};

struct GatewayConfig {
    SerialConfig serial;
    struct SerialInstanceConfig {
        std::string name;
        SerialConfig serial;
    };
    UploaderConfig uploader;
    RuntimeConfig runtime;
    CommandConfig command;
    HeartbeatConfig heartbeat;
    ReloadConfig reload;
    MonitorConfig monitor;
    WifiDeviceServerConfig wifi_device_server;
    MqttDeviceIngressConfig mqtt_device_ingress;
    TcpBinaryConfig tcp_binary;
    LogConfig log;
    std::vector<DeviceConfig> devices;
    std::vector<SerialInstanceConfig> serials;
};

class ConfigLoader {
public:
    static bool loadFromFile(const std::string& path, GatewayConfig& out, std::string& err);
};

} // namespace sg
