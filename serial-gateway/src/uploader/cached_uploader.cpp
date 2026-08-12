#include "uploader/cached_uploader.hpp"

#include "common/logger.hpp"
#include "uploader/sensor_record_codec.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

namespace sg {

namespace {

enum class PendingUploadKind {
    Sensor = 1,
    Heartbeat = 2,
    Status = 3,
    Event = 4,
    CommandAck = 5,
    OtaStatus = 6,
};

struct PendingUploadRecord {
    PendingUploadKind kind = PendingUploadKind::Sensor;
    SensorData data;
    std::string payload;
};

std::string hexEncode(const std::string& input) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 2);
    for (unsigned char ch : input) {
        out.push_back(kHex[(ch >> 4) & 0x0F]);
        out.push_back(kHex[ch & 0x0F]);
    }
    return out;
}

bool hexDecode(const std::string& input, std::string& out) {
    auto fromHex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
        return -1;
    };

    if ((input.size() % 2) != 0) {
        return false;
    }

    out.clear();
    out.reserve(input.size() / 2);
    for (std::size_t i = 0; i < input.size(); i += 2) {
        const int hi = fromHex(input[i]);
        const int lo = fromHex(input[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

std::string encodePendingRecord(const PendingUploadRecord& record) {
    std::ostringstream oss;
    oss << static_cast<int>(record.kind) << '\t';
    switch (record.kind) {
        case PendingUploadKind::Sensor:
        case PendingUploadKind::Status:
        case PendingUploadKind::CommandAck:
            oss << encodeSensorRecord(record.data);
            break;
        case PendingUploadKind::Heartbeat:
        case PendingUploadKind::Event:
        case PendingUploadKind::OtaStatus:
            oss << hexEncode(record.payload);
            break;
    }
    return oss.str();
}

bool decodePendingRecord(const std::string& line, PendingUploadRecord& record) {
    const std::size_t sep = line.find('\t');
    if (sep == std::string::npos) {
        record = PendingUploadRecord{};
        record.kind = PendingUploadKind::Sensor;
        return decodeSensorRecord(line, record.data);
    }

    try {
        const int kind = std::stoi(line.substr(0, sep));
        const std::string payload = line.substr(sep + 1);
        record = PendingUploadRecord{};
        record.kind = static_cast<PendingUploadKind>(kind);
        switch (record.kind) {
            case PendingUploadKind::Sensor:
            case PendingUploadKind::Status:
            case PendingUploadKind::CommandAck:
                return decodeSensorRecord(payload, record.data);
            case PendingUploadKind::Heartbeat:
            case PendingUploadKind::Event:
            case PendingUploadKind::OtaStatus:
                return hexDecode(payload, record.payload);
        }
    } catch (...) {
        return false;
    }
    return false;
}

bool replayPendingRecord(IUploader* inner, const PendingUploadRecord& record) {
    if (inner == nullptr) {
        return false;
    }
    switch (record.kind) {
        case PendingUploadKind::Sensor:
            return inner->upload(record.data);
        case PendingUploadKind::Heartbeat:
            return inner->uploadHeartbeat(record.payload);
        case PendingUploadKind::Status:
            return inner->uploadStatus(record.data);
        case PendingUploadKind::Event:
            return inner->uploadEvent(record.payload);
        case PendingUploadKind::CommandAck:
            return inner->uploadCommandAck(record.data);
        case PendingUploadKind::OtaStatus:
            return inner->uploadOtaStatus(record.payload);
    }
    return false;
}

} // namespace

CachedUploader::CachedUploader(std::unique_ptr<IUploader> inner, const UploaderConfig& config, RuntimeStats& stats)
    : inner_(std::move(inner)),
      cache_file_(config.disk_cache_file),
      max_cache_lines_(config.disk_cache_max_lines),
      replay_batch_size_(config.replay_batch_size),
      replay_pause_ms_(config.replay_pause_ms),
      stats_(stats) {
    std::filesystem::path p(cache_file_);
    if (!p.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    std::vector<std::string> lines;
    if (readAllLines(lines)) {
        stats_.setCacheBacklog(lines.size());
    }
}

bool CachedUploader::upload(const SensorData& data) {
    return uploadSensorLike(data, &IUploader::upload);
}

bool CachedUploader::uploadHeartbeat(const std::string& payload) {
    return uploadPayloadLike(payload, &IUploader::uploadHeartbeat);
}

bool CachedUploader::uploadStatus(const SensorData& data) {
    return uploadSensorLike(data, &IUploader::uploadStatus);
}

bool CachedUploader::uploadEvent(const std::string& payload) {
    return uploadPayloadLike(payload, &IUploader::uploadEvent);
}

bool CachedUploader::uploadCommandAck(const SensorData& data) {
    return uploadSensorLike(data, &IUploader::uploadCommandAck);
}

bool CachedUploader::uploadOtaStatus(const std::string& payload) {
    return uploadPayloadLike(payload, &IUploader::uploadOtaStatus);
}

void CachedUploader::close() {
    if (inner_ != nullptr) {
        inner_->close();
    }
}

bool CachedUploader::flushPending() {
    if (!hasPending()) {
        return true;
    }

    std::vector<std::string> lines;
    if (!readAllLines(lines)) {
        return false;
    }

    std::size_t sent_count = 0;
    std::size_t failed_index = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (replay_batch_size_ > 0 && sent_count >= replay_batch_size_) {
            failed_index = i;
            break;
        }

        PendingUploadRecord record;
        if (!decodePendingRecord(lines[i], record)) {
            continue;
        }
        if (!replayPendingRecord(inner_.get(), record)) {
            stats_.addCacheReplayFailed(1);
            failed_index = i;
            break;
        }

        ++sent_count;
        stats_.addCacheReplaySuccess(1);
        if (replay_pause_ms_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(replay_pause_ms_));
        }
    }

    if (failed_index == lines.size()) {
        const bool ok = truncateFile();
        if (ok) {
            stats_.setCacheBacklog(0);
        }
        return ok;
    }

    const bool ok = rewriteFrom(lines, failed_index);
    if (ok) {
        std::vector<std::string> pending;
        if (readAllLines(pending)) {
            stats_.setCacheBacklog(pending.size());
        }
    }
    return ok;
}

bool CachedUploader::uploadSensorLike(const SensorData& data, bool (IUploader::*fn)(const SensorData&)) {
    if (!flushPending()) {
        const int kind = (fn == &IUploader::upload)
            ? static_cast<int>(PendingUploadKind::Sensor)
            : (fn == &IUploader::uploadStatus)
                ? static_cast<int>(PendingUploadKind::Status)
                : static_cast<int>(PendingUploadKind::CommandAck);
        if (!appendPendingSensor(data, kind)) {
            Logger::instance().error("cache append failed while pending queue exists");
        }
        return false;
    }

    if (inner_ != nullptr && (inner_.get()->*fn)(data)) {
        return true;
    }

    const int kind = (fn == &IUploader::upload)
        ? static_cast<int>(PendingUploadKind::Sensor)
        : (fn == &IUploader::uploadStatus)
            ? static_cast<int>(PendingUploadKind::Status)
            : static_cast<int>(PendingUploadKind::CommandAck);
    if (!appendPendingSensor(data, kind)) {
        Logger::instance().error("cache append failed after upload failure");
    }
    return false;
}

bool CachedUploader::uploadPayloadLike(const std::string& payload, bool (IUploader::*fn)(const std::string&)) {
    if (!flushPending()) {
        const int kind = (fn == &IUploader::uploadHeartbeat)
            ? static_cast<int>(PendingUploadKind::Heartbeat)
            : (fn == &IUploader::uploadEvent)
                ? static_cast<int>(PendingUploadKind::Event)
                : static_cast<int>(PendingUploadKind::OtaStatus);
        if (!appendPendingPayload(payload, kind)) {
            Logger::instance().error("cache append failed while pending queue exists");
        }
        return false;
    }

    if (inner_ != nullptr && (inner_.get()->*fn)(payload)) {
        return true;
    }

    const int kind = (fn == &IUploader::uploadHeartbeat)
        ? static_cast<int>(PendingUploadKind::Heartbeat)
        : (fn == &IUploader::uploadEvent)
            ? static_cast<int>(PendingUploadKind::Event)
            : static_cast<int>(PendingUploadKind::OtaStatus);
    if (!appendPendingPayload(payload, kind)) {
        Logger::instance().error("cache append failed after upload failure");
    }
    return false;
}

bool CachedUploader::appendPendingSensor(const SensorData& data, int kind) {
    PendingUploadRecord record;
    record.kind = static_cast<PendingUploadKind>(kind);
    record.data = data;
    std::ofstream out(cache_file_, std::ios::app);
    if (!out) {
        return false;
    }
    out << encodePendingRecord(record) << '\n';
    out.close();
    if (!out) {
        return false;
    }
    stats_.addCacheEnqueue(1);

    if (max_cache_lines_ > 0) {
        std::vector<std::string> lines;
        if (readAllLines(lines)) {
            enforceCacheLimit(lines);
            if (!rewriteAll(lines)) {
                return false;
            }
            stats_.setCacheBacklog(lines.size());
        }
    } else {
        std::vector<std::string> lines;
        if (readAllLines(lines)) {
            stats_.setCacheBacklog(lines.size());
        }
    }

    return true;
}

bool CachedUploader::appendPendingPayload(const std::string& data, int kind) {
    PendingUploadRecord record;
    record.kind = static_cast<PendingUploadKind>(kind);
    record.payload = data;
    std::ofstream out(cache_file_, std::ios::app);
    if (!out) {
        return false;
    }
    out << encodePendingRecord(record) << '\n';
    out.close();
    if (!out) {
        return false;
    }
    stats_.addCacheEnqueue(1);

    if (max_cache_lines_ > 0) {
        std::vector<std::string> lines;
        if (readAllLines(lines)) {
            enforceCacheLimit(lines);
            if (!rewriteAll(lines)) {
                return false;
            }
            stats_.setCacheBacklog(lines.size());
        }
    } else {
        std::vector<std::string> lines;
        if (readAllLines(lines)) {
            stats_.setCacheBacklog(lines.size());
        }
    }

    return true;
}

bool CachedUploader::readAllLines(std::vector<std::string>& out) const {
    out.clear();
    std::ifstream in(cache_file_);
    if (!in) {
        return true;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            out.push_back(line);
        }
    }
    return true;
}

bool CachedUploader::rewriteAll(const std::vector<std::string>& lines) {
    const std::string tmp_file = cache_file_ + ".tmp";
    {
        std::ofstream out(tmp_file, std::ios::trunc);
        if (!out) {
            return false;
        }
        for (const auto& line : lines) {
            out << line << '\n';
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp_file, cache_file_, ec);
    if (ec) {
        return false;
    }
    return true;
}

bool CachedUploader::rewriteFrom(const std::vector<std::string>& lines, std::size_t begin_idx) {
    std::vector<std::string> pending;
    if (begin_idx < lines.size()) {
        pending.assign(lines.begin() + static_cast<long>(begin_idx), lines.end());
    }
    enforceCacheLimit(pending);
    return rewriteAll(pending);
}

bool CachedUploader::truncateFile() {
    std::ofstream out(cache_file_, std::ios::trunc);
    return static_cast<bool>(out);
}

bool CachedUploader::hasPending() const {
    std::error_code ec;
    if (!std::filesystem::exists(cache_file_, ec)) {
        return false;
    }
    return std::filesystem::file_size(cache_file_, ec) > 0;
}

void CachedUploader::enforceCacheLimit(std::vector<std::string>& lines) const {
    if (max_cache_lines_ == 0 || lines.size() <= max_cache_lines_) {
        return;
    }

    const std::size_t to_drop = lines.size() - max_cache_lines_;
    lines.erase(lines.begin(), lines.begin() + static_cast<long>(to_drop));
}

} // namespace sg
