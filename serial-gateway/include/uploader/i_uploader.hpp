#pragma once

#include "protocol/frame.hpp"

#include <string>

namespace sg {

class IUploader {
public:
    virtual ~IUploader() = default;

    virtual bool upload(const SensorData& data) = 0;
    virtual bool uploadHeartbeat(const std::string& payload) = 0;
    virtual bool uploadStatus(const SensorData& data) { return upload(data); }
    virtual bool uploadEvent(const std::string& payload) { return uploadHeartbeat(payload); }
    virtual bool uploadCommandAck(const SensorData& data) { return upload(data); }
    virtual bool uploadOtaStatus(const std::string& payload) { return uploadEvent(payload); }
    virtual void close() = 0;
};

} // namespace sg
