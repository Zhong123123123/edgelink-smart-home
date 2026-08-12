#pragma once

#include "config/config.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <sys/types.h>

namespace sg {

class SerialPort { // 串口类
public:
    SerialPort() = default;
    ~SerialPort();

    bool open(const SerialConfig& config, std::string& err);
    ssize_t read(uint8_t* buf, std::size_t len, std::string& err);
    ssize_t write(const uint8_t* buf, std::size_t len, std::string& err);
    void close();
    bool isOpen() const;

private:
    mutable std::mutex mutex_; // 互斥锁，用于保护串口操作
    int fd_ = -1; // 串口文件描述符
};

} // namespace sg
