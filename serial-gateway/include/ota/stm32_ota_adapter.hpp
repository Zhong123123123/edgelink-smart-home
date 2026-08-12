#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sg::ota {

struct Stm32OtaOptions {
    std::string host = "127.0.0.1";
    int port = 19090;
    std::size_t chunk_size = 240;
    int timeout_ms = 5000;
    int max_retries = 3;
    bool wait_boot_hello = true;
    int hello_timeout_ms = 10000;
    int prepare_ack_timeout_ms = 15000;
    int data_ack_timeout_ms = 5000;
    int verify_ack_timeout_ms = 5000;
    int commit_ack_timeout_ms = 5000;
    int inter_chunk_delay_ms = 0;
    std::uint32_t target_base = 0;
    std::uint32_t image_version = 0;
    std::function<bool(const std::vector<std::uint8_t>& frame, std::string& err)> send_raw_frame;
    std::function<bool(std::vector<std::uint8_t>& frame, int timeout_ms, std::string& err)> recv_raw_frame;
};

class Stm32OtaAdapter {
public:
    using ProgressCb = std::function<void(int)>;
    using ShouldStopCb = std::function<bool()>;

    bool run(const std::vector<std::uint8_t>& image,
             std::uint32_t image_crc32,
             const Stm32OtaOptions& opt,
             std::string& err,
             const ProgressCb& progress_cb = nullptr,
             const ShouldStopCb& should_stop_cb = nullptr);

private:
    bool sendFrame(int fd, std::uint8_t type, const std::vector<std::uint8_t>& payload, std::string& err);
    bool recvAck(int fd, std::uint16_t seq, int timeout_ms, std::string& err);
};

} // namespace sg::ota
