#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace {

bool makePty(int& master_fd, std::string& slave_name) {
    int slave_fd = -1;
    char name_buf[128] = {0};
    if (openpty(&master_fd, &slave_fd, name_buf, nullptr, nullptr) != 0) {
        return false;
    }
    slave_name = name_buf;
    ::close(slave_fd);
    return true;
}

bool forwardData(int src_fd, int dst_fd) {
    char buf[512];
    const ssize_t n = ::read(src_fd, buf, sizeof(buf));
    if (n <= 0) {
        return n == 0 || errno == EAGAIN || errno == EWOULDBLOCK;
    }

    ssize_t sent = 0;
    while (sent < n) {
        const ssize_t w = ::write(dst_fd, buf + sent, static_cast<std::size_t>(n - sent));
        if (w <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return false;
        }
        sent += w;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const std::string link_a = (argc >= 2) ? argv[1] : "/tmp/ttyV0";
    const std::string link_b = (argc >= 3) ? argv[2] : "/tmp/ttyV1";

    int master_a = -1;
    int master_b = -1;
    std::string slave_a;
    std::string slave_b;

    if (!makePty(master_a, slave_a) || !makePty(master_b, slave_b)) {
        std::cerr << "openpty failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    ::unlink(link_a.c_str());
    ::unlink(link_b.c_str());
    if (::symlink(slave_a.c_str(), link_a.c_str()) != 0 || ::symlink(slave_b.c_str(), link_b.c_str()) != 0) {
        std::cerr << "symlink failed: " << std::strerror(errno) << "\n";
        return 2;
    }

    std::cout << "virtual serial ready: " << link_a << "->" << slave_a << " ; "
              << link_b << "->" << slave_b << "\n";

    pollfd fds[2] = {
        {master_a, POLLIN, 0},
        {master_b, POLLIN, 0},
    };

    while (true) {
        const int rc = ::poll(fds, 2, 500);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "poll failed: " << std::strerror(errno) << "\n";
            return 3;
        }
        if (rc == 0) {
            continue;
        }

        if ((fds[0].revents & POLLIN) != 0 && !forwardData(master_a, master_b)) {
            std::cerr << "forward A->B failed\n";
            return 4;
        }
        if ((fds[1].revents & POLLIN) != 0 && !forwardData(master_b, master_a)) {
            std::cerr << "forward B->A failed\n";
            return 5;
        }
    }

    return 0;
}
