#include "common/mqtt_runtime.hpp"
#include "uploader/mqtt_uploader.hpp"

#include "common/logger.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

#ifdef SG_HAVE_MOSQUITTO
#include <mosquitto.h>
#endif

namespace sg {

namespace {

constexpr int kMqttOperationTimeoutMs = 3000;

std::uint64_t unixMsNow() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

#ifdef SG_HAVE_MOSQUITTO
mosquitto* asMosq(void* p) {
    return static_cast<mosquitto*>(p);
}
#endif

} // namespace

MqttUploader::MqttUploader(const UploaderConfig& config, RuntimeStats& stats)
    : config_(config), stats_(stats), backoff_ms_(config.reconnect_initial_ms) {
#ifdef SG_HAVE_MOSQUITTO
    if (!mqttRuntimeAcquire()) {
        Logger::instance().error("mqtt init failed: mosquitto runtime acquire failed");
        return;
    }
    const std::string client_id = config_.mqtt_client_id.empty() ? "serial-gateway" : config_.mqtt_client_id;
    mosq_ = static_cast<void*>(mosquitto_new(client_id.c_str(), config_.mqtt_clean_session, this));
    if (mosq_ == nullptr) {
        Logger::instance().error("mqtt init failed: mosquitto_new returned null");
        mqttRuntimeRelease();
        return;
    }
    mosquitto_connect_callback_set(asMosq(mosq_), &MqttUploader::onConnect);
    mosquitto_disconnect_callback_set(asMosq(mosq_), &MqttUploader::onDisconnect);
    mosquitto_publish_callback_set(asMosq(mosq_), &MqttUploader::onPublish);

    if (!config_.mqtt_username.empty()) {
        const int rc = mosquitto_username_pw_set(
            asMosq(mosq_),
            config_.mqtt_username.c_str(),
            config_.mqtt_password.empty() ? nullptr : config_.mqtt_password.c_str());
        if (rc != MOSQ_ERR_SUCCESS) {
            Logger::instance().error("mqtt set credential failed: rc=" + std::to_string(rc));
        }
    }

    mosquitto_max_inflight_messages_set(asMosq(mosq_), static_cast<unsigned int>(config_.mqtt_max_inflight));
    mosquitto_reconnect_delay_set(asMosq(mosq_),
                                  std::max(1, config_.reconnect_initial_ms / 1000),
                                  std::max(1, config_.reconnect_max_ms / 1000),
                                  true);

    Logger::instance().info(
        "mqtt backend initialized host=" + config_.host +
        " port=" + std::to_string(config_.port) +
        " topic=" + config_.mqtt_topic +
        " qos=" + std::to_string(config_.mqtt_qos) +
        " auth=" + (config_.mqtt_username.empty() ? std::string("off") : std::string("on")));
#else
    Logger::instance().warn("mqtt backend running in placeholder mode (libmosquitto not linked)");
#endif
}

MqttUploader::~MqttUploader() {
    close();
}

bool MqttUploader::upload(const SensorData& data) {
    return publishRaw(config_.mqtt_topic, toJson(data));
}

bool MqttUploader::uploadHeartbeat(const std::string& payload) {
    return publishRaw(resolveTopic(config_.mqtt_heartbeat_topic, "/heartbeat"), payload);
}

bool MqttUploader::uploadStatus(const SensorData& data) {
    return publishRaw(resolveTopic(config_.mqtt_status_topic, "/status"), toJson(data));
}

bool MqttUploader::uploadEvent(const std::string& payload) {
    return publishRaw(resolveTopic(config_.mqtt_event_topic, "/event"), payload);
}

bool MqttUploader::uploadCommandAck(const SensorData& data) {
    return publishRaw(resolveTopic(config_.mqtt_command_ack_topic, "/command_ack"), toJson(data));
}

bool MqttUploader::uploadOtaStatus(const std::string& payload) {
    return publishRaw(resolveTopic(config_.mqtt_ota_status_topic, "/ota_status"), payload);
}

bool MqttUploader::publishRaw(const std::string& topic, const std::string& payload) {
#ifdef SG_HAVE_MOSQUITTO
    if (!ensureConnected()) {
        stats_.addUploadFailed(1);
        return false;
    }

    int mid = 0;
    const int rc = mosquitto_publish(asMosq(mosq_),
                                     &mid,
                                     topic.c_str(),
                                     static_cast<int>(payload.size()),
                                     payload.c_str(),
                                     config_.mqtt_qos,
                                     false);
    if (rc != MOSQ_ERR_SUCCESS) {
        Logger::instance().warn("mqtt publish failed topic=" + topic + " rc=" + std::to_string(rc));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected_ = false;
            pending_publish_mids_.erase(mid);
            acked_publish_mids_.erase(mid);
        }
        stats_.incReconnects();
        stats_.setLastReconnectUnixMs(unixMsNow());
        stats_.addUploadFailed(1);
        return false;
    }

    if (config_.mqtt_qos > 0) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_publish_mids_.insert(mid);
        }
        if (!waitForPublishAck(mid)) {
            Logger::instance().warn("mqtt publish ack timeout topic=" + topic + " mid=" + std::to_string(mid));
            stats_.incReconnects();
            stats_.setLastReconnectUnixMs(unixMsNow());
            stats_.addUploadFailed(1);
            return false;
        }
    }

    stats_.addUploadSuccess(1);
    return true;
#else
    (void)topic;
    (void)payload;
    if (!warned_) {
        Logger::instance().warn(
            "MQTT uploader placeholder mode: install libmosquitto-dev to enable real publish.");
        warned_ = true;
    }
    stats_.incReconnects();
    stats_.setLastReconnectUnixMs(unixMsNow());
    stats_.addUploadFailed(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms_));
    backoff_ms_ = std::min(backoff_ms_ * 2, config_.reconnect_max_ms);
    return false;
#endif
}

std::string MqttUploader::resolveTopic(const std::string& configured, const std::string& fallback_suffix) const {
    if (!configured.empty()) {
        return configured;
    }
    return config_.mqtt_topic + fallback_suffix;
}

void MqttUploader::close() {
#ifdef SG_HAVE_MOSQUITTO
    if (mosq_ != nullptr) {
        if (connected_) {
            mosquitto_disconnect(asMosq(mosq_));
            connected_ = false;
        }
        if (loop_started_) {
            mosquitto_loop_stop(asMosq(mosq_), true);
            loop_started_ = false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_publish_mids_.clear();
            acked_publish_mids_.clear();
            connect_ready_ = false;
            last_connect_rc_ = -1;
        }
        mosquitto_destroy(asMosq(mosq_));
        mosq_ = nullptr;
        mqttRuntimeRelease();
    }
#endif
}

bool MqttUploader::ensureConnected() {
#ifdef SG_HAVE_MOSQUITTO
    if (mosq_ == nullptr) {
        return false;
    }

    if (!loop_started_) {
        const int loop_rc = mosquitto_loop_start(asMosq(mosq_));
        if (loop_rc != MOSQ_ERR_SUCCESS) {
            Logger::instance().warn("mqtt loop start failed: rc=" + std::to_string(loop_rc));
            stats_.incReconnects();
            stats_.setLastReconnectUnixMs(unixMsNow());
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms_));
            backoff_ms_ = std::min(backoff_ms_ * 2, config_.reconnect_max_ms);
            return false;
        }
        loop_started_ = true;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected_) {
            return true;
        }
        connect_ready_ = false;
        last_connect_rc_ = -1;
    }

    const int rc = mosquitto_connect_async(asMosq(mosq_),
                                           config_.host.c_str(),
                                           config_.port,
                                           config_.mqtt_keepalive_sec);
    if (rc != MOSQ_ERR_SUCCESS) {
        stats_.incReconnects();
        stats_.setLastReconnectUnixMs(unixMsNow());
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms_));
        backoff_ms_ = std::min(backoff_ms_ * 2, config_.reconnect_max_ms);
        return false;
    }

    const int timeout_ms = (config_.connect_timeout_ms > 0) ? config_.connect_timeout_ms : kMqttOperationTimeoutMs;
    std::unique_lock<std::mutex> lock(mutex_);
    const bool connected = cv_.wait_for(lock,
                                        std::chrono::milliseconds(timeout_ms),
                                        [&]() { return connect_ready_; });
    if (!connected || !connected_) {
        lock.unlock();
        stats_.incReconnects();
        stats_.setLastReconnectUnixMs(unixMsNow());
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms_));
        backoff_ms_ = std::min(backoff_ms_ * 2, config_.reconnect_max_ms);
        return false;
    }
    backoff_ms_ = config_.reconnect_initial_ms;
    return true;
#else
    return false;
#endif
}

bool MqttUploader::waitForPublishAck(int mid) {
#ifdef SG_HAVE_MOSQUITTO
    const int timeout_ms = (config_.connect_timeout_ms > 0) ? config_.connect_timeout_ms : kMqttOperationTimeoutMs;
    std::unique_lock<std::mutex> lock(mutex_);
    const bool published = cv_.wait_for(lock,
                                        std::chrono::milliseconds(timeout_ms),
                                        [&]() {
                                            return acked_publish_mids_.count(mid) > 0 || !connected_;
                                        });
    if (!published || !connected_ || acked_publish_mids_.count(mid) == 0) {
        pending_publish_mids_.erase(mid);
        acked_publish_mids_.erase(mid);
        connected_ = false;
        return false;
    }
    pending_publish_mids_.erase(mid);
    acked_publish_mids_.erase(mid);
    return true;
#else
    (void)mid;
    return false;
#endif
}

void MqttUploader::onConnect(struct mosquitto*, void* userdata, int rc) {
#ifdef SG_HAVE_MOSQUITTO
    auto* self = static_cast<MqttUploader*>(userdata);
    if (self == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->last_connect_rc_ = rc;
    self->connect_ready_ = true;
    self->connected_ = (rc == MOSQ_ERR_SUCCESS);
    self->cv_.notify_all();
#else
    (void)userdata;
    (void)rc;
#endif
}

void MqttUploader::onDisconnect(struct mosquitto*, void* userdata, int rc) {
#ifdef SG_HAVE_MOSQUITTO
    auto* self = static_cast<MqttUploader*>(userdata);
    if (self == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->connected_ = false;
    self->connect_ready_ = false;
    if (rc != MOSQ_ERR_SUCCESS) {
        self->stats_.incReconnects();
        self->stats_.setLastReconnectUnixMs(unixMsNow());
    }
    self->cv_.notify_all();
#else
    (void)userdata;
    (void)rc;
#endif
}

void MqttUploader::onPublish(struct mosquitto*, void* userdata, int mid) {
#ifdef SG_HAVE_MOSQUITTO
    auto* self = static_cast<MqttUploader*>(userdata);
    if (self == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->acked_publish_mids_.insert(mid);
    self->pending_publish_mids_.erase(mid);
    self->cv_.notify_all();
#else
    (void)userdata;
    (void)mid;
#endif
}

std::string MqttUploader::toJson(const SensorData& data) {
    std::ostringstream oss;
    const std::string fallback_name = "sensor-" + std::to_string(static_cast<int>(data.device_id));
    const std::string& name = data.device_name.empty() ? fallback_name : data.device_name;

    oss << "{"
        << "\"device_id\":" << static_cast<int>(data.device_id) << ","
        << "\"device_name\":\"" << name << "\"," 
        << "\"event_type\":\"" << frameTypeName(data.frame_type) << "\"," 
        << "\"timestamp\":" << data.timestamp_unix_ms << ","
        << "\"temperature\":" << data.temperature << ","
        << "\"humidity\":" << data.humidity << ","
        << "\"voltage\":" << data.voltage << ","
        << "\"status\":" << static_cast<int>(data.status) << ","
        << "\"command_id\":" << data.command_id << ","
        << "\"command_result\":" << static_cast<int>(data.command_result);
    if (!data.payload_summary.empty()) {
        oss << ",\"payload_summary\":\"" << data.payload_summary << "\"";
    }
    if (!data.topic_suffix.empty()) {
        oss << ",\"topic_suffix\":\"" << data.topic_suffix << "\"";
    }
    oss << "}";
    return oss.str();
}

} // namespace sg
