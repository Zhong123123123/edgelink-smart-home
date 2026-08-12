#pragma once

#include "config/config.hpp"
#include "runtime/runtime_stats.hpp"
#include "uploader/i_uploader.hpp"

#include <memory>
#include <string>
#include <vector>

namespace sg {

class CachedUploader : public IUploader {
public:
    CachedUploader(std::unique_ptr<IUploader> inner, const UploaderConfig& config, RuntimeStats& stats);

    bool upload(const SensorData& data) override;
    bool uploadHeartbeat(const std::string& payload) override;
    bool uploadStatus(const SensorData& data) override;
    bool uploadEvent(const std::string& payload) override;
    bool uploadCommandAck(const SensorData& data) override;
    bool uploadOtaStatus(const std::string& payload) override;
    void close() override;

private:
    bool flushPending();
    bool uploadSensorLike(const SensorData& data, bool (IUploader::*fn)(const SensorData&));
    bool uploadPayloadLike(const std::string& payload, bool (IUploader::*fn)(const std::string&));
    bool appendPendingSensor(const SensorData& data, int kind);
    bool appendPendingPayload(const std::string& payload, int kind);
    bool readAllLines(std::vector<std::string>& out) const;
    bool rewriteAll(const std::vector<std::string>& lines);
    bool rewriteFrom(const std::vector<std::string>& lines, std::size_t begin_idx);
    bool truncateFile();
    bool hasPending() const;
    void enforceCacheLimit(std::vector<std::string>& lines) const;

    std::unique_ptr<IUploader> inner_;
    std::string cache_file_;
    std::size_t max_cache_lines_ = 0;
    std::size_t replay_batch_size_ = 0;
    int replay_pause_ms_ = 0;
    RuntimeStats& stats_;
};

} // namespace sg
