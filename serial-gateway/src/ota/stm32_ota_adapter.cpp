#include "ota/stm32_ota_adapter.hpp"

#include "common/logger.hpp"
#include "protocol/crc16.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <thread>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace sg::ota {
namespace {

void pushU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
}

void pushU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 24U) & 0xFFU));
}

std::uint16_t readU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8U);
}

std::vector<std::uint8_t> buildFrame(std::uint8_t type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> buf;
    buf.reserve(payload.size() + 8);
    buf.push_back(0xAA);
    buf.push_back(0x55);
    buf.push_back(static_cast<std::uint8_t>(payload.size() + 1U));
    buf.push_back(type);
    buf.insert(buf.end(), payload.begin(), payload.end());
    const std::uint16_t crc = sg::crc16_modbus(&buf[2], static_cast<std::size_t>(1 + payload.size() + 1));
    buf.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    buf.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xFFU));
    return buf;
}

std::string hex32(std::uint32_t v) {
    char buf[11]{};
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(v));
    return std::string(buf);
}

std::string hex8(std::uint8_t v) {
    char buf[5]{};
    std::snprintf(buf, sizeof(buf), "0x%02X", static_cast<unsigned int>(v));
    return std::string(buf);
}

std::uint64_t steadyMsNow() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::uint8_t frameType(const std::vector<std::uint8_t>& frame) {
    if (frame.size() < 4U) return 0xFFU;
    return frame[3];
}

std::size_t framePayloadLen(const std::vector<std::uint8_t>& frame) {
    if (frame.size() < 4U) return 0U;
    const std::uint8_t len = frame[2];
    return (len == 0U) ? 0U : static_cast<std::size_t>(len - 1U);
}

std::uint16_t frameSeqIfAny(const std::vector<std::uint8_t>& frame) {
    if (frame.size() < 8U) return 0U;
    return readU16(frame.data() + 4U);
}

bool parseAckFrame(const std::vector<std::uint8_t>& frame, std::uint16_t seq, std::string& err, bool& decided) {
    decided = false;
    if (frame.size() < 6U || frame[0] != 0xAAU || frame[1] != 0x55U) return false;
    const std::uint8_t len = frame[2];
    const std::size_t total = 2U + 1U + static_cast<std::size_t>(len) + 2U;
    if (frame.size() != total || len < 1U) return false;
    const std::uint8_t type = frame[3];
    const std::size_t payload_len = static_cast<std::size_t>(len - 1U);
    const std::uint16_t wire_crc = readU16(frame.data() + total - 2U);
    const std::uint16_t calc_crc = sg::crc16_modbus(frame.data() + 2, 1U + static_cast<std::size_t>(len));
    if (wire_crc != calc_crc) return false;
    if (type != 0x22U && type != 0x23U) return true;
    if (payload_len < 2U) return false;
    const std::uint16_t ack_seq = readU16(frame.data() + 4U);
    if (ack_seq != seq) return true;
    decided = true;
    if (type == 0x22U) return true;
    if (payload_len >= 4U) {
        const std::uint16_t code = readU16(frame.data() + 6U);
        err = "received nack code=" + std::to_string(code);
    } else {
        err = "received nack";
    }
    return false;
}

struct AckEvent {
    bool valid{false};
    bool is_ack{false};
    std::uint16_t seq{0U};
    std::string err;
};

AckEvent parseAckEvent(const std::vector<std::uint8_t>& frame) {
    AckEvent ev{};
    if (frame.size() < 6U || frame[0] != 0xAAU || frame[1] != 0x55U) return ev;
    const std::uint8_t len = frame[2];
    const std::size_t total = 2U + 1U + static_cast<std::size_t>(len) + 2U;
    if (frame.size() != total || len < 1U) return ev;
    const std::uint8_t type = frame[3];
    if (type != 0x22U && type != 0x23U) return ev;
    const std::size_t payload_len = static_cast<std::size_t>(len - 1U);
    if (payload_len < 2U) return ev;
    const std::uint16_t wire_crc = readU16(frame.data() + total - 2U);
    const std::uint16_t calc_crc = sg::crc16_modbus(frame.data() + 2, 1U + static_cast<std::size_t>(len));
    if (wire_crc != calc_crc) return ev;

    ev.valid = true;
    ev.seq = readU16(frame.data() + 4U);
    ev.is_ack = (type == 0x22U);
    if (!ev.is_ack) {
        if (payload_len >= 4U) {
            const std::uint16_t code = readU16(frame.data() + 6U);
            ev.err = "received nack code=" + std::to_string(code);
        } else {
            ev.err = "received nack";
        }
    }
    return ev;
}

} // namespace

bool Stm32OtaAdapter::sendFrame(int fd, std::uint8_t type, const std::vector<std::uint8_t>& payload, std::string& err) {
    const std::vector<std::uint8_t> buf = buildFrame(type, payload);

    const ssize_t n = ::send(fd, buf.data(), buf.size(), 0);
    if (n != static_cast<ssize_t>(buf.size())) {
        err = "send frame failed";
        return false;
    }
    return true;
}

bool Stm32OtaAdapter::recvAck(int fd, std::uint16_t seq, int timeout_ms, std::string& err) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    while (clock::now() < deadline) {
        const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{};
        tv.tv_sec = static_cast<int>(remain / 1000);
        tv.tv_usec = static_cast<int>((remain % 1000) * 1000);
        const int rc = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (rc <= 0) {
            err = "ack timeout";
            return false;
        }

        std::uint8_t hdr[4]{};
        const ssize_t n = ::recv(fd, hdr, sizeof(hdr), MSG_WAITALL);
        if (n != static_cast<ssize_t>(sizeof(hdr))) {
            err = "ack read failed";
            return false;
        }
        if (hdr[0] != 0xAA || hdr[1] != 0x55) {
            continue;
        }

        const std::uint8_t len = hdr[2];
        const std::uint8_t type = hdr[3];
        if (len < 1U) {
            continue;
        }
        const std::size_t payload_len = static_cast<std::size_t>(len - 1U);
        std::vector<std::uint8_t> payload(payload_len + 2U, 0U); // +crc16
        if (::recv(fd, payload.data(), payload.size(), MSG_WAITALL) != static_cast<ssize_t>(payload.size())) {
            err = "ack payload read failed";
            return false;
        }

        std::vector<std::uint8_t> crc_buf;
        crc_buf.reserve(static_cast<std::size_t>(2U + payload_len));
        crc_buf.push_back(len);
        crc_buf.push_back(type);
        crc_buf.insert(crc_buf.end(), payload.begin(), payload.begin() + static_cast<long>(payload_len));
        const std::uint16_t wire_crc = readU16(payload.data() + payload_len);
        const std::uint16_t calc_crc = sg::crc16_modbus(crc_buf.data(), crc_buf.size());
        if (wire_crc != calc_crc) {
            continue;
        }

        if ((type == 0x22U || type == 0x23U) && payload_len >= 2U) {
            const std::uint16_t ack_seq = readU16(payload.data());
            if (ack_seq != seq) {
                continue;
            }
            if (type == 0x22U) {
                return true;
            }
            if (payload_len >= 4U) {
                const std::uint16_t code = readU16(payload.data() + 2U);
                err = "received nack code=" + std::to_string(code);
            } else {
                err = "received nack";
            }
            return false;
        }
        // ignore BOOT_HELLO / VERSION_REPORT / PROGRESS / REPORT
    }
    err = "ack timeout";
    return false;
}

bool Stm32OtaAdapter::run(const std::vector<std::uint8_t>& image,
                          std::uint32_t image_crc32,
                          const Stm32OtaOptions& opt,
                          std::string& err,
                          const ProgressCb& progress_cb,
                          const ShouldStopCb& should_stop_cb) {
    const std::size_t chunk_size = std::min<std::size_t>(opt.chunk_size, 246U);
    if (chunk_size == 0U) {
        err = "invalid chunk_size";
        return false;
    }
    auto shouldStop = [&]() {
        return should_stop_cb && should_stop_cb();
    };
    if (shouldStop()) {
        err = "ota canceled";
        return false;
    }
    const bool use_custom_transport = static_cast<bool>(opt.send_raw_frame) && static_cast<bool>(opt.recv_raw_frame);
    int fd = -1;
    if (!use_custom_transport) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            err = "socket create failed";
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(opt.port));
        if (::inet_pton(AF_INET, opt.host.c_str(), &addr.sin_addr) != 1) {
            ::close(fd);
            err = "invalid host";
            return false;
        }
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            err = "connect failed";
            return false;
        }
    }

    auto logState = [](const std::string& from, const std::string& to) {
        Logger::instance().info("[OTA][FSM] state transition " + from + " -> " + to);
    };
    auto logRxFrame = [](const std::vector<std::uint8_t>& frame) {
        Logger::instance().info("[OTA][RX] type=" + hex8(frameType(frame)) +
                                " seq=" + std::to_string(frameSeqIfAny(frame)) +
                                " payload_len=" + std::to_string(framePayloadLen(frame)));
    };

    std::uint16_t seq = 1;
    std::vector<std::uint8_t> prepare;
    pushU16(prepare, seq);
    pushU32(prepare, static_cast<std::uint32_t>(image.size()));
    pushU32(prepare, image_crc32);
    pushU16(prepare, static_cast<std::uint16_t>(chunk_size));
    if (opt.target_base != 0U) {
        pushU32(prepare, opt.target_base);
        if (opt.image_version != 0U) {
            pushU32(prepare, opt.image_version);
        }
    } else if (opt.image_version != 0U) {
        pushU32(prepare, 0U);
        pushU32(prepare, opt.image_version);
    }
    Logger::instance().info("[OTA][PREPARE] size=" + std::to_string(image.size()) +
                            " crc32=" + hex32(image_crc32) +
                            " chunk=" + std::to_string(chunk_size) +
                            " target_base=" + hex32(opt.target_base) +
                            " image_version=" + std::to_string(opt.image_version));
    auto sendAndRecvAck = [&](std::uint8_t type,
                              const std::vector<std::uint8_t>& payload,
                              std::uint16_t expect_seq,
                              int phase_timeout_ms,
                              std::string& out_err) {
        static std::unordered_map<std::uint16_t, std::string> ack_cache;
        auto consumeCachedAck = [&](std::uint16_t seq_to_consume, std::string& consume_err) -> int {
            const auto it = ack_cache.find(seq_to_consume);
            if (it == ack_cache.end()) return 0; // no decision
            const std::string cached_err = it->second;
            ack_cache.erase(it);
            if (cached_err.empty()) {
                consume_err.clear();
                Logger::instance().info("[OTA][RX] cached ack seq=" + std::to_string(seq_to_consume));
                return 1; // ack
            }
            consume_err = cached_err;
            Logger::instance().warn("[OTA][RX] cached nack seq=" + std::to_string(seq_to_consume) +
                                    " err=" + cached_err);
            return -1; // nack
        };
        Logger::instance().info("[OTA][TX] type=" + hex8(type) +
                                " seq=" + std::to_string(expect_seq) +
                                " payload_len=" + std::to_string(payload.size()) +
                                " timeout_ms=" + std::to_string(phase_timeout_ms));
        if (use_custom_transport) {
            const std::vector<std::uint8_t> frame = buildFrame(type, payload);
            const std::uint64_t tx_data_ts = steadyMsNow();
            if (!opt.send_raw_frame(frame, out_err)) return false;
            {
                const int cached = consumeCachedAck(expect_seq, out_err);
                if (cached == 1) {
                    Logger::instance().info("[OTA][TIMING] seq=" + std::to_string(expect_seq) +
                                            " tx_data_ts=" + std::to_string(tx_data_ts) +
                                            " ack_deadline_ts=" +
                                            std::to_string(tx_data_ts + static_cast<std::uint64_t>(phase_timeout_ms)) +
                                            " rx_ack_ts=" + std::to_string(steadyMsNow()) +
                                            " cached=1");
                    return true;
                }
                if (cached == -1) return false;
            }
            using clock = std::chrono::steady_clock;
            const auto deadline = clock::now() + std::chrono::milliseconds(phase_timeout_ms);
            const std::uint64_t ack_deadline_ts = tx_data_ts + static_cast<std::uint64_t>(phase_timeout_ms);
            constexpr int kAckGraceWindowMs = 1500;
            bool grace_used = false;
            while (clock::now() < deadline) {
                const int remain = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count());
                std::vector<std::uint8_t> rx_frame;
                std::string local_err;
                if (!opt.recv_raw_frame(rx_frame, remain, local_err)) {
                    out_err = local_err.empty() ? "ack timeout" : local_err;
                    return false;
                }
                logRxFrame(rx_frame);
                const AckEvent ev = parseAckEvent(rx_frame);
                if (ev.valid) {
                    ack_cache[ev.seq] = ev.is_ack ? std::string() : ev.err;
                    Logger::instance().info("[OTA][RX] cache update seq=" + std::to_string(ev.seq) +
                                            " kind=" + std::string(ev.is_ack ? "ack" : "nack"));
                    const int cached = consumeCachedAck(expect_seq, out_err);
                    if (cached == 1) {
                        Logger::instance().info("[OTA][TIMING] seq=" + std::to_string(expect_seq) +
                                                " tx_data_ts=" + std::to_string(tx_data_ts) +
                                                " ack_deadline_ts=" + std::to_string(ack_deadline_ts) +
                                                " rx_ack_ts=" + std::to_string(steadyMsNow()));
                        return true;
                    }
                    if (cached == -1) return false;
                    continue;
                }
                if (frameType(rx_frame) == 0x22U || frameType(rx_frame) == 0x23U) {
                    Logger::instance().info("[OTA][TIMING] seq=" + std::to_string(expect_seq) +
                                            " tx_data_ts=" + std::to_string(tx_data_ts) +
                                            " ack_deadline_ts=" + std::to_string(ack_deadline_ts) +
                                            " rx_ack_ts=" + std::to_string(steadyMsNow()));
                }
                bool decided = false;
                const bool ok = parseAckFrame(rx_frame, expect_seq, out_err, decided);
                if (decided) return ok;
            }
            const std::uint64_t timeout_decide_ts = steadyMsNow();
            Logger::instance().warn("[OTA][TIMING] seq=" + std::to_string(expect_seq) +
                                    " timeout_decide_ts=" + std::to_string(timeout_decide_ts) +
                                    " tx_data_ts=" + std::to_string(tx_data_ts) +
                                    " ack_deadline_ts=" + std::to_string(ack_deadline_ts));
            {
                std::vector<std::uint8_t> rx_frame;
                std::string local_err;
                if (opt.recv_raw_frame(rx_frame, kAckGraceWindowMs, local_err)) {
                    grace_used = true;
                    logRxFrame(rx_frame);
                    const AckEvent ev = parseAckEvent(rx_frame);
                    if (ev.valid) {
                        ack_cache[ev.seq] = ev.is_ack ? std::string() : ev.err;
                        Logger::instance().info("[OTA][RX] cache update seq=" + std::to_string(ev.seq) +
                                                " kind=" + std::string(ev.is_ack ? "ack" : "nack"));
                        const int cached = consumeCachedAck(expect_seq, out_err);
                        if (cached == 1) {
                            Logger::instance().warn("[OTA][TIMING] seq=" + std::to_string(expect_seq) +
                                                    " accepted_in_grace=1");
                            return true;
                        }
                        if (cached == -1) return false;
                    }
                    if (frameType(rx_frame) == 0x22U || frameType(rx_frame) == 0x23U) {
                        Logger::instance().info("[OTA][TIMING] seq=" + std::to_string(expect_seq) +
                                                " tx_data_ts=" + std::to_string(tx_data_ts) +
                                                " ack_deadline_ts=" + std::to_string(ack_deadline_ts) +
                                                " rx_ack_ts=" + std::to_string(steadyMsNow()) +
                                                " grace_ms=" + std::to_string(kAckGraceWindowMs));
                    }
                    bool decided = false;
                    const bool ok = parseAckFrame(rx_frame, expect_seq, out_err, decided);
                    if (decided) {
                        Logger::instance().warn("[OTA][TIMING] seq=" + std::to_string(expect_seq) +
                                                " accepted_in_grace=1");
                        return ok;
                    }
                }
            }
            if (grace_used) {
                Logger::instance().warn("[OTA][TIMING] seq=" + std::to_string(expect_seq) +
                                        " grace_used_no_decision=1");
            }
            out_err = "ack timeout";
            return false;
        }
        return sendFrame(fd, type, payload, out_err) && recvAck(fd, expect_seq, phase_timeout_ms, out_err);
    };

    auto retryExchange = [&](const char* phase,
                             std::uint8_t type,
                             const std::vector<std::uint8_t>& payload,
                             std::uint16_t expect_seq,
                             int phase_timeout_ms,
                             std::string& out_err) {
        for (int attempt = 1; attempt <= opt.max_retries; ++attempt) {
            if (sendAndRecvAck(type, payload, expect_seq, phase_timeout_ms, out_err)) {
                return true;
            }
            Logger::instance().warn("[OTA][RETRY] phase=" + std::string(phase) +
                                    " attempt=" + std::to_string(attempt) +
                                    "/" + std::to_string(opt.max_retries) +
                                    " err=" + out_err);
            if (out_err == "ack timeout" && attempt < opt.max_retries) {
                // Give delayed ACK forwarding a small window before re-send.
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
            }
        }
        return false;
    };

    if (opt.wait_boot_hello && use_custom_transport) {
        logState("PRECHECK", "WAIT_BOOT_HELLO");
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::milliseconds(opt.hello_timeout_ms);
        bool got_hello = false;
        while (clock::now() < deadline) {
            const int remain = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count());
            std::vector<std::uint8_t> rx_frame;
            std::string local_err;
            if (!opt.recv_raw_frame(rx_frame, remain, local_err)) {
                if (local_err.empty() || local_err == "ack timeout") {
                    err = "boot hello timeout";
                } else {
                    err = local_err;
                }
                break;
            }
            logRxFrame(rx_frame);
            if (frameType(rx_frame) == 0x20U) {
                got_hello = true;
                break;
            }
        }
        if (!got_hello) {
            if (fd >= 0) ::close(fd);
            if (err.empty()) err = "boot hello timeout";
            return false;
        }
        logState("WAIT_BOOT_HELLO", "WAIT_PREPARE_ACK");
    } else {
        logState("PRECHECK", "WAIT_PREPARE_ACK");
    }

    if (!retryExchange("prepare", 0x31U, prepare, seq, opt.prepare_ack_timeout_ms, err)) {
        if (fd >= 0) ::close(fd);
        return false;
    }

    logState("WAIT_PREPARE_ACK", "WAIT_DATA_ACK");
    std::size_t offset = 0;
    while (offset < image.size()) {
        if (shouldStop()) {
            err = "ota canceled";
            ::close(fd);
            return false;
        }
        ++seq;
        const std::size_t n = std::min(chunk_size, image.size() - offset);
        std::vector<std::uint8_t> payload;
        pushU16(payload, seq);
        pushU32(payload, static_cast<std::uint32_t>(offset));
        pushU16(payload, static_cast<std::uint16_t>(n));
        payload.insert(payload.end(), image.begin() + static_cast<long>(offset), image.begin() + static_cast<long>(offset + n));

        std::string local_err;
        if (!retryExchange("data", 0x32U, payload, seq, opt.data_ack_timeout_ms, local_err)) {
            err = local_err;
            if (fd >= 0) ::close(fd);
            return false;
        }

        offset += n;
        if (offset < image.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (opt.inter_chunk_delay_ms > 0 && offset < image.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(opt.inter_chunk_delay_ms));
        }
        if (progress_cb) {
            const int pct = static_cast<int>((offset * 100U) / image.size());
            progress_cb(pct);
        }
    }

    ++seq;
    std::vector<std::uint8_t> verify;
    pushU16(verify, seq);
    pushU32(verify, image_crc32);
    logState("WAIT_DATA_ACK", "WAIT_VERIFY_ACK");
    Logger::instance().info("[OTA][VERIFY] seq=" + std::to_string(seq) +
                            " sent_bytes=" + std::to_string(offset) +
                            " expected_size=" + std::to_string(image.size()) +
                            " expected_crc32=" + hex32(image_crc32));
    if (!retryExchange("verify", 0x33U, verify, seq, opt.verify_ack_timeout_ms, err)) {
        if (fd >= 0) ::close(fd);
        return false;
    }

    ++seq;
    std::vector<std::uint8_t> commit;
    pushU16(commit, seq);
    logState("WAIT_VERIFY_ACK", "WAIT_COMMIT_ACK");
    Logger::instance().info("[OTA][COMMIT] seq=" + std::to_string(seq) +
                            " sent_bytes=" + std::to_string(offset) +
                            " expected_size=" + std::to_string(image.size()) +
                            " expected_crc32=" + hex32(image_crc32));
    if (!retryExchange("commit", 0x34U, commit, seq, opt.commit_ack_timeout_ms, err)) {
        if (err == "ack timeout") {
            err = "commit ack timeout";
        }
        if (fd >= 0) ::close(fd);
        return false;
    }

    logState("WAIT_COMMIT_ACK", "DONE");
    if (fd >= 0) ::close(fd);
    return true;
}

} // namespace sg::ota
