#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>

namespace {
volatile std::sig_atomic_t g_stop = 0;

void onSig(int) { g_stop = 1; }

speed_t toBaud(int baud) {
    switch (baud) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
#ifdef B230400
        case 230400: return B230400;
#endif
        default: return B115200;
    }
}

int openSerial(const std::string& path, int baud) {
    int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) {
        std::cerr << "[bridge] open serial failed: " << path << " err=" << std::strerror(errno) << "\n";
        return -1;
    }

    termios tty{};
    if (tcgetattr(fd, &tty) != 0) {
        std::cerr << "[bridge] tcgetattr failed err=" << std::strerror(errno) << "\n";
        ::close(fd);
        return -1;
    }

    cfmakeraw(&tty);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;

    const speed_t spd = toBaud(baud);
    cfsetispeed(&tty, spd);
    cfsetospeed(&tty, spd);

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        std::cerr << "[bridge] tcsetattr failed err=" << std::strerror(errno) << "\n";
        ::close(fd);
        return -1;
    }

    return fd;
}

int openListen(const std::string& host, int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "[bridge] socket failed err=" << std::strerror(errno) << "\n";
        return -1;
    }
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "[bridge] invalid listen host: " << host << "\n";
        ::close(fd);
        return -1;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[bridge] bind failed err=" << std::strerror(errno) << "\n";
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 4) != 0) {
        std::cerr << "[bridge] listen failed err=" << std::strerror(errno) << "\n";
        ::close(fd);
        return -1;
    }
    return fd;
}

bool writeAll(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        const ssize_t n = ::write(fd, data + off, len - off);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string listen_host = "127.0.0.1";
    int listen_port = 19091;
    std::string serial_dev = "/dev/ttyUSB0";
    int baud = 115200;

    for (int i = 1; i < argc; ++i) {
        const std::string a(argv[i]);
        if (a.rfind("--listen-host=", 0) == 0) listen_host = a.substr(14);
        else if (a.rfind("--listen-port=", 0) == 0) listen_port = std::stoi(a.substr(14));
        else if (a.rfind("--serial=", 0) == 0) serial_dev = a.substr(9);
        else if (a.rfind("--baud=", 0) == 0) baud = std::stoi(a.substr(7));
    }

    std::signal(SIGINT, onSig);
    std::signal(SIGTERM, onSig);

    const int sfd = openSerial(serial_dev, baud);
    if (sfd < 0) return 1;

    const int lfd = openListen(listen_host, listen_port);
    if (lfd < 0) {
        ::close(sfd);
        return 2;
    }

    std::cout << "[bridge] listening " << listen_host << ":" << listen_port
              << " <-> " << serial_dev << " @" << baud << "\n";

    while (!g_stop) {
        sockaddr_in ca{};
        socklen_t cl = sizeof(ca);
        const int cfd = ::accept(lfd, reinterpret_cast<sockaddr*>(&ca), &cl);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[bridge] accept failed err=" << std::strerror(errno) << "\n";
            continue;
        }

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &ca.sin_addr, ip, sizeof(ip));
        std::cout << "[bridge] client connected " << ip << ":" << ntohs(ca.sin_port) << "\n";

        pollfd pfd[2]{};
        pfd[0].fd = cfd;
        pfd[0].events = POLLIN;
        pfd[1].fd = sfd;
        pfd[1].events = POLLIN;

        bool alive = true;
        while (alive && !g_stop) {
            const int rc = ::poll(pfd, 2, 500);
            if (rc < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (rc == 0) continue;

            if ((pfd[0].revents & POLLIN) != 0) {
                char buf[2048];
                const ssize_t n = ::read(cfd, buf, sizeof(buf));
                if (n <= 0) {
                    alive = false;
                } else if (!writeAll(sfd, buf, static_cast<size_t>(n))) {
                    alive = false;
                }
            }
            if ((pfd[1].revents & POLLIN) != 0) {
                char buf[2048];
                const ssize_t n = ::read(sfd, buf, sizeof(buf));
                if (n > 0) {
                    if (!writeAll(cfd, buf, static_cast<size_t>(n))) {
                        alive = false;
                    }
                }
            }
            if ((pfd[0].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                alive = false;
            }
        }

        ::close(cfd);
        std::cout << "[bridge] client disconnected\n";
    }

    ::close(lfd);
    ::close(sfd);
    std::cout << "[bridge] stopped\n";
    return 0;
}
