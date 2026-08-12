#include "serial/serial_port.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace sg {

namespace {

constexpr int kSerialWritePollTimeoutMs = 1000;

speed_t toSpeed(int baudrate) { // 将波特率转换为termios speed_t
    switch (baudrate) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        default: return B115200;
    }
}

} // namespace

SerialPort::~SerialPort() { // 析构函数，关闭串口
    close();
}

bool SerialPort::open(const SerialConfig& config, std::string& err) { // 打开串口
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    fd_ = ::open(config.device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK); // 打开串口文件
    if (fd_ < 0) {
        err = "open serial failed: " + std::string(std::strerror(errno));
        return false;
    }

    termios tty{}; // 串口参数结构体
    if (tcgetattr(fd_, &tty) != 0) { 
        err = "tcgetattr failed: " + std::string(std::strerror(errno));
        close();
        return false;
    }

    cfmakeraw(&tty);
    tty.c_cflag |= CREAD | CLOCAL; // 设置串口为原始模式，禁用本地控制
    tty.c_cflag &= ~CSIZE; // 清除数据位掩码

    switch (config.data_bits) { // 设置数据位
        case 5: tty.c_cflag |= CS5; break; // 5位数据位
        case 6: tty.c_cflag |= CS6; break; // 6位数据位
        case 7: tty.c_cflag |= CS7; break; // 7位数据位
        case 8: tty.c_cflag |= CS8; break; // 8位数据位
        default: tty.c_cflag |= CS8; break; // 默认8位数据位
    }

    tty.c_cflag &= ~(PARENB | PARODD); // 清除校验位掩码
    if (config.parity == 'E') {
        tty.c_cflag |= PARENB; // 设置偶校验位
    } else if (config.parity == 'O') {
        tty.c_cflag |= (PARENB | PARODD); // 设置奇校验位
    }

    if (config.stop_bits == 2) {
        tty.c_cflag |= CSTOPB; // 设置2位停止位
    } else {
        tty.c_cflag &= ~CSTOPB; // 清除停止位掩码
    }

    const speed_t speed = toSpeed(config.baudrate); // 将波特率转换为termios speed_t
    cfsetispeed(&tty, speed); // 设置输入波特率
    cfsetospeed(&tty, speed); // 设置输出波特率

    tty.c_cc[VMIN] = 0; // 非阻塞读取，至少接收1个字符
    tty.c_cc[VTIME] = 1; // 设置超时时间，单位为秒

    if (tcsetattr(fd_, TCSANOW, &tty) != 0) { // 应用新的串口参数
        err = "tcsetattr failed: " + std::string(std::strerror(errno)); 
        close();
        return false;
    }

    return true;
}

ssize_t SerialPort::read(uint8_t* buf, std::size_t len, std::string& err) { // 读取串口数据
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) {
        err = "serial not open";
        return -1;
    }

    const ssize_t n = ::read(fd_, buf, len);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        err = "serial read failed: " + std::string(std::strerror(errno));
        return -1;
    }
    return (n < 0) ? 0 : n;
}

ssize_t SerialPort::write(const uint8_t* buf, std::size_t len, std::string& err) { // 写入串口数据
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) {
        err = "serial not open";
        return -1;
    }

    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::write(fd_, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                pollfd pfd{};
                pfd.fd = fd_;
                pfd.events = POLLOUT;
                const int rc = ::poll(&pfd, 1, kSerialWritePollTimeoutMs);
                if (rc > 0) {
                    continue;
                }
                if (rc == 0) {
                    err = "serial write timeout";
                    return -1;
                }
                if (errno == EINTR) {
                    continue;
                }
                err = "serial poll failed: " + std::string(std::strerror(errno));
                return -1;
            }
            if (errno == EINTR) {
                continue;
            }
            err = "serial write failed: " + std::string(std::strerror(errno));
            return -1;
        }
        sent += static_cast<std::size_t>(n);
    }
    return static_cast<ssize_t>(sent);
}

void SerialPort::close() { // 关闭串口
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool SerialPort::isOpen() const { // 检查串口是否打开
    std::lock_guard<std::mutex> lock(mutex_);
    return fd_ >= 0;
}

} // namespace sg
