#pragma once

#include "config/config.hpp"
#include "protocol/frame.hpp"
#include "runtime/runtime_stats.hpp"
#include "uploader/i_uploader.hpp"

#include <string>

namespace sg {

class TcpUploader : public IUploader {
public:
    TcpUploader(const UploaderConfig& config, RuntimeStats& stats);
    ~TcpUploader();

    bool upload(const SensorData& data) override;
    bool uploadHeartbeat(const std::string& payload) override;
    bool uploadStatus(const SensorData& data) override;
    bool uploadEvent(const std::string& payload) override;
    bool uploadCommandAck(const SensorData& data) override;
    bool uploadOtaStatus(const std::string& payload) override;
    void close() override;

private:
    bool ensureConnected();
    bool connectSocket();
    bool sendAll(const char* data, std::size_t len);
    bool sendLine(const std::string& payload);
    static std::string toJson(const SensorData& data);

    UploaderConfig config_;
    RuntimeStats& stats_;
    int sock_ = -1;
    int backoff_ms_ = 0;
};

} // namespace sg
