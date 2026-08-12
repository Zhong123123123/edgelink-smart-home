#include "config/config.hpp"
#include "uploader/cached_uploader.hpp"
#include "uploader/sensor_record_codec.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

namespace {

struct SharedState {
    int fail_count = 0;
    int call_count = 0;
    std::vector<sg::SensorData> uploaded;
    std::vector<std::string> heartbeat_payloads;
    std::vector<std::string> event_payloads;
    std::vector<std::string> ota_payloads;
};

class FakeUploader : public sg::IUploader {
public:
    explicit FakeUploader(std::shared_ptr<SharedState> state) : state_(std::move(state)) {}

    bool upload(const sg::SensorData& data) override {
        state_->call_count++;
        if (state_->fail_count > 0) {
            state_->fail_count--;
            return false;
        }
        state_->uploaded.push_back(data);
        return true;
    }

    bool uploadHeartbeat(const std::string&) override {
        state_->call_count++;
        if (state_->fail_count > 0) {
            state_->fail_count--;
            return false;
        }
        state_->heartbeat_payloads.push_back("heartbeat");
        return true;
    }

    bool uploadEvent(const std::string& payload) override {
        state_->call_count++;
        if (state_->fail_count > 0) {
            state_->fail_count--;
            return false;
        }
        state_->event_payloads.push_back(payload);
        return true;
    }

    bool uploadOtaStatus(const std::string& payload) override {
        state_->call_count++;
        if (state_->fail_count > 0) {
            state_->fail_count--;
            return false;
        }
        state_->ota_payloads.push_back(payload);
        return true;
    }

    void close() override {}

private:
    std::shared_ptr<SharedState> state_;
};

long lineCount(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return 0;
    }
    long cnt = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            ++cnt;
        }
    }
    return cnt;
}

std::vector<std::string> readLines(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            out.push_back(line);
        }
    }
    return out;
}

} // namespace

int main() {
    const std::string cache_path = "/tmp/sg_test_cached_uploader.log";
    const std::string cache_path_limit = "/tmp/sg_test_cached_uploader_limit.log";
    std::error_code ec;
    std::filesystem::remove(cache_path, ec);
    std::filesystem::remove(cache_path_limit, ec);

    sg::UploaderConfig cfg;
    cfg.disk_cache_file = cache_path;
    sg::RuntimeStats stats;

    auto state = std::make_shared<SharedState>();
    state->fail_count = 1;

    auto inner = std::make_unique<FakeUploader>(state);
    sg::CachedUploader uploader(std::move(inner), cfg, stats);

    sg::SensorData d1;
    d1.device_id = 1;
    d1.device_name = "alpha";
    d1.temperature = 20.1;

    sg::SensorData d2;
    d2.device_id = 2;
    d2.device_name = "beta";
    d2.temperature = 21.2;

    if (uploader.upload(d1)) {
        std::cerr << "first upload should fail and be cached\n";
        return 1;
    }
    if (lineCount(cache_path) != 1) {
        std::cerr << "cache file should have 1 pending line\n";
        return 2;
    }

    if (!uploader.upload(d2)) {
        std::cerr << "second upload should flush pending and succeed\n";
        return 3;
    }

    if (lineCount(cache_path) != 0) {
        std::cerr << "cache file should be empty after successful flush\n";
        return 4;
    }

    if (state->uploaded.size() != 2 || state->uploaded[0].device_id != 1 || state->uploaded[1].device_id != 2) {
        std::cerr << "uploaded data order mismatch\n";
        return 5;
    }

    sg::UploaderConfig cfg_limit;
    cfg_limit.disk_cache_file = cache_path_limit;
    cfg_limit.disk_cache_max_lines = 3;
    sg::RuntimeStats stats_limit;

    auto state_limit = std::make_shared<SharedState>();
    state_limit->fail_count = 1000;
    auto inner_limit = std::make_unique<FakeUploader>(state_limit);
    sg::CachedUploader uploader_limit(std::move(inner_limit), cfg_limit, stats_limit);

    for (int id = 1; id <= 5; ++id) {
        sg::SensorData d;
        d.device_id = static_cast<std::uint8_t>(id);
        d.device_name = "sensor-" + std::to_string(id);
        if (uploader_limit.upload(d)) {
            std::cerr << "limit scenario should cache only\n";
            return 6;
        }
    }

    if (lineCount(cache_path_limit) != 3) {
        std::cerr << "cache line limit not enforced\n";
        return 7;
    }

    const auto lines = readLines(cache_path_limit);
    if (lines.size() != 3) {
        std::cerr << "cache lines read mismatch\n";
        return 8;
    }

    sg::SensorData a;
    sg::SensorData b;
    sg::SensorData c;
    if (!sg::decodeSensorRecord(lines[0], a) || !sg::decodeSensorRecord(lines[1], b) ||
        !sg::decodeSensorRecord(lines[2], c)) {
        std::cerr << "decode cached lines failed\n";
        return 9;
    }
    if (a.device_id != 3 || b.device_id != 4 || c.device_id != 5) {
        std::cerr << "cache should keep newest lines only\n";
        return 10;
    }

    const std::string cache_path_payload = "/tmp/sg_test_cached_uploader_payload.log";
    std::filesystem::remove(cache_path_payload, ec);

    sg::UploaderConfig cfg_payload;
    cfg_payload.disk_cache_file = cache_path_payload;
    sg::RuntimeStats stats_payload;

    auto payload_state = std::make_shared<SharedState>();
    payload_state->fail_count = 1;
    auto inner_payload = std::make_unique<FakeUploader>(payload_state);
    sg::CachedUploader payload_uploader(std::move(inner_payload), cfg_payload, stats_payload);

    if (payload_uploader.uploadEvent("{\"kind\":\"alarm\"}")) {
        std::cerr << "event upload should fail and be cached\n";
        return 11;
    }
    if (lineCount(cache_path_payload) != 1) {
        std::cerr << "event cache file should have 1 pending line\n";
        return 12;
    }
    if (!payload_uploader.uploadOtaStatus("{\"status\":\"ok\"}")) {
        std::cerr << "ota upload should flush cached event and succeed\n";
        return 13;
    }
    if (lineCount(cache_path_payload) != 0) {
        std::cerr << "payload cache file should be empty after replay\n";
        return 14;
    }
    if (payload_state->event_payloads.size() != 1 || payload_state->event_payloads[0] != "{\"kind\":\"alarm\"}") {
        std::cerr << "cached event payload replay mismatch\n";
        return 15;
    }
    if (payload_state->ota_payloads.size() != 1 || payload_state->ota_payloads[0] != "{\"status\":\"ok\"}") {
        std::cerr << "ota payload upload mismatch\n";
        return 16;
    }

    std::filesystem::remove(cache_path, ec);
    std::filesystem::remove(cache_path_limit, ec);
    std::filesystem::remove(cache_path_payload, ec);
    return 0;
}
