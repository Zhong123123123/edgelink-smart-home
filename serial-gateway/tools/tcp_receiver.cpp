#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    const int port = (argc >= 2) ? std::stoi(argv[1]) : 9000;

    const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "socket failed\n";
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "bind failed: " << std::strerror(errno) << "\n";
        ::close(server_fd);
        return 2;
    }

    if (listen(server_fd, 4) != 0) {
        std::cerr << "listen failed\n";
        ::close(server_fd);
        return 3;
    }

    std::cout << "tcp_receiver listening on port " << port << "\n";

    while (true) {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        const int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&cli), &len);
        if (client_fd < 0) {
            continue;
        }

        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        std::cout << "client connected from " << ip << ":" << ntohs(cli.sin_port) << "\n";

        std::string line;
        char buf[512] = {0};
        while (true) {
            const ssize_t n = read(client_fd, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            line.append(buf, buf + n);

            std::size_t pos = 0;
            while (true) {
                const std::size_t eol = line.find('\n', pos);
                if (eol == std::string::npos) {
                    line = line.substr(pos);
                    break;
                }
                std::cout << line.substr(pos, eol - pos) << "\n";
                pos = eol + 1;
            }
        }

        std::cout << "client disconnected\n";
        ::close(client_fd);
    }

    return 0;
}
