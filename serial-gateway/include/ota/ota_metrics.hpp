#pragma once

#include <atomic>
#include <cstdint>

namespace sg::ota {

class OtaMetrics {
public:
    void incTaskSuccess() { success_.fetch_add(1, std::memory_order_relaxed); }
    void incTaskFailed() { failed_.fetch_add(1, std::memory_order_relaxed); }
    void addBytesSent(std::uint64_t n) { bytes_sent_.fetch_add(n, std::memory_order_relaxed); }

    std::uint64_t taskSuccess() const { return success_.load(std::memory_order_relaxed); }
    std::uint64_t taskFailed() const { return failed_.load(std::memory_order_relaxed); }
    std::uint64_t bytesSent() const { return bytes_sent_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> success_{0};
    std::atomic<std::uint64_t> failed_{0};
    std::atomic<std::uint64_t> bytes_sent_{0};
};

} // namespace sg::ota
