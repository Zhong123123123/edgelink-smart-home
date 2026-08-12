#pragma once

#include "config/config.hpp"
#include "device/device_registry.hpp"
#include "protocol/frame_parser.hpp"
#include "runtime/runtime_stats.hpp"
#include "runtime/thread_safe_queue.hpp"
#include "serial/serial_port.hpp"
#include "uploader/i_uploader.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct mosquitto;
struct mosquitto_message;

namespace sg::storage { class HistoryStore; }
namespace sg::ota { struct Esp32OtaStatus; }

namespace sg {
struct ControlCommandRequest {
    std::uint8_t device_id = 0;
    std::uint16_t command_id = 0;
    bool has_command_id = false;
    std::string command_type;
    int timeout_ms = 3000;
    bool has_on = false;
    bool on = false;
    bool has_mode = false;
    bool mode_auto = false;
    bool has_threshold = false;
    double threshold_temp = 0.0;
    double threshold_humi = 0.0;
    bool has_report_interval = false;
    int report_interval_ms = 0;
    bool has_log_level = false;
    std::string log_level;
};

class AppController {
public:
    explicit AppController(const GatewayConfig& config);
    ~AppController();

    bool start();
    void stop();

private:
    friend void mqttIngressOnConnect(struct mosquitto*, void*, int);
    friend void mqttIngressOnDisconnect(struct mosquitto*, void*, int);
    friend void mqttIngressOnMessage(struct mosquitto*, void*, const struct mosquitto_message*);
    friend void upstreamControlOnConnect(struct mosquitto*, void*, int);
    friend void upstreamControlOnDisconnect(struct mosquitto*, void*, int);
    friend void upstreamControlOnMessage(struct mosquitto*, void*, const struct mosquitto_message*);

    struct PendingCommand {
        std::uint8_t device_id = 0;
        std::uint16_t command_id = 0;
        std::string command_type;
        std::uint64_t enqueue_ms = 0;
        bool acked = false;
        std::uint8_t ack_result = 0xFF;
    };

    void serialLoop();
    void parserLoop();
    void uploaderLoop();
    void statsLoop();
    void commandLoop();
    void wifiDeviceServerLoop();
    void mqttDeviceLoop();
    void upstreamControlLoop();
    void tcpBinaryLoop();
    void heartbeatLoop();
    void monitorLoop();
    void initDeviceRegistry();
    void pushRecentSample(const SensorData& data);
    void refreshDeviceMetrics();
    bool sendWifiCommandLine(std::uint8_t device_id, const std::string& line);
    bool sendTcpBinaryFrame(std::uint8_t device_id, const std::vector<std::uint8_t>& frame);
    void bindActiveTransport(std::uint8_t device_id, const std::string& transport, int priority);
    std::string activeTransportFor(std::uint8_t device_id);
    int transportPriority(const std::string& transport) const;
    bool shouldAcceptTransportData(std::uint8_t device_id, const std::string& incoming_transport);
    void clearActiveTransport(std::uint8_t device_id, const std::string& reason);
    bool sendMqttDevicePayload(std::uint8_t device_id,
                               const std::string& subtopic,
                               const std::string& payload,
                               std::string& err);
    void handleMqttIngressConnect(int rc);
    void handleMqttIngressDisconnect(int rc);
    void handleMqttIngressMessage(const std::string& topic, const std::string& payload);
    void handleUpstreamControlConnect(int rc);
    void handleUpstreamControlDisconnect(int rc);
    void handleUpstreamControlMessage(const std::string& topic, const std::string& payload);
    void executeControlCommand(ControlCommandRequest cmd_req, std::string& http_status, std::string& response_body);
    void handleControlCommandRequestJson(const std::string& req_body, std::string& http_status, std::string& response_body);
    void handleOtaTaskRequestJson(const std::string& req_body, std::string& http_status, std::string& response_body);
    void launchOtaTask(std::string task_uuid,
                       std::string firmware_id,
                       std::string device_type,
                       std::string transport,
                       std::string target,
                       std::uint8_t device_id,
                       std::string firmware_url);
    void cancelActiveOtaTasks();
    void waitForOtaTasks();
    void reapFinishedOtaTasksLocked();
    void registerPendingCommand(std::uint8_t device_id, std::uint16_t command_id, const std::string& command_type);
    bool reservePendingCommand(std::uint8_t device_id,
                               const std::string& command_type,
                               std::uint16_t& command_id);
    bool waitCommandAck(std::uint8_t device_id, std::uint16_t command_id, int timeout_ms, std::uint8_t& ack_result);
    void completeCommandAck(std::uint8_t device_id, std::uint16_t command_id, std::uint8_t ack_result);
    bool publishUpstreamSystemEvent(const std::string& event_type,
                                    int device_id,
                                    const std::string& detail);
    bool publishUpstreamOtaStatus(const ota::Esp32OtaStatus& st);
    static std::uint32_t makeCommandKey(std::uint8_t device_id, std::uint16_t command_id);
    static std::uint64_t unixMsNow();

    GatewayConfig config_;
    RuntimeStats stats_;
    SerialPort serial_;
    FrameParser parser_;
    ThreadSafeQueue<std::vector<std::uint8_t>> raw_queue_;
    ThreadSafeQueue<SensorData> data_queue_;
    std::unique_ptr<IUploader> uploader_;
    std::unique_ptr<sg::storage::HistoryStore> history_store_;
    DeviceRegistry device_registry_;
    std::deque<SensorData> recent_samples_;
    std::mutex recent_mutex_;
    std::unordered_map<std::uint8_t, DeviceConfig> device_map_;
    std::unordered_set<std::uint8_t> unknown_device_warned_;
    std::mutex transport_mutex_;
    std::unordered_map<std::uint8_t, std::string> active_transport_;
    std::unordered_map<std::uint8_t, int> active_transport_priority_;
    std::mutex wifi_mutex_;
    std::unordered_map<std::uint8_t, int> wifi_device_fds_;
    std::unordered_map<std::uint8_t, int> tcp_binary_device_fds_;
    std::size_t wifi_clients_connected_{0};
    std::mutex mqtt_device_mutex_;
    void* mqtt_device_mosq_ = nullptr;
    bool mqtt_device_connected_ = false;
    std::mutex upstream_control_mutex_;
    void* upstream_control_mosq_ = nullptr;
    bool upstream_control_connected_ = false;
    std::mutex command_mutex_;
    std::condition_variable command_cv_;
    std::unordered_map<std::uint32_t, PendingCommand> pending_commands_;
    std::uint16_t next_generated_command_id_{1000};
    std::mutex ota_tasks_mutex_;
    std::vector<std::pair<std::string, std::future<void>>> ota_tasks_;
    std::unordered_set<std::string> active_ota_task_ids_;

    std::atomic<bool> running_{false};
    bool serial_available_{false};
    std::chrono::steady_clock::time_point start_time_;
    std::thread serial_thread_;
    std::thread parser_thread_;
    std::thread uploader_thread_;
    std::thread stats_thread_;
    std::thread command_thread_;
    std::thread wifi_thread_;
    std::thread mqtt_thread_;
    std::thread upstream_control_thread_;
    std::thread tcp_binary_thread_;
    std::thread heartbeat_thread_;
    std::thread monitor_thread_;
};

} // namespace sg
