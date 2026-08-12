#pragma once

#include "config/config.hpp"
#include "runtime/runtime_stats.hpp"
#include "uploader/i_uploader.hpp"

#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_set>

struct mosquitto;

namespace sg {

class MqttUploader : public IUploader {
public:
    MqttUploader(const UploaderConfig& config, RuntimeStats& stats);
    ~MqttUploader() override;

    bool upload(const SensorData& data) override;
    bool uploadHeartbeat(const std::string& payload) override;
    bool uploadStatus(const SensorData& data) override;
    bool uploadEvent(const std::string& payload) override;
    bool uploadCommandAck(const SensorData& data) override;
    bool uploadOtaStatus(const std::string& payload) override;
    void close() override;

private:
    bool ensureConnected();
    bool waitForPublishAck(int mid);
    bool publishRaw(const std::string& topic, const std::string& payload);
    std::string resolveTopic(const std::string& configured, const std::string& fallback_suffix) const;
    static std::string toJson(const SensorData& data);
    static void onConnect(struct mosquitto*, void* userdata, int rc);
    static void onDisconnect(struct mosquitto*, void* userdata, int rc);
    static void onPublish(struct mosquitto*, void* userdata, int mid);

    UploaderConfig config_;
    RuntimeStats& stats_;
    bool warned_ = false;
    int backoff_ms_ = 0;

#ifdef SG_HAVE_MOSQUITTO
    void* mosq_ = nullptr;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool connected_ = false;
    bool connect_ready_ = false;
    bool loop_started_ = false;
    int last_connect_rc_ = -1;
    std::unordered_set<int> pending_publish_mids_;
    std::unordered_set<int> acked_publish_mids_;
#endif
};

} // namespace sg
