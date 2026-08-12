#include <mosquitto.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) {
    g_stop.store(true);
}

void onMessage(struct mosquitto*, void*, const struct mosquitto_message* msg) {
    if (msg == nullptr || msg->payload == nullptr || msg->payloadlen <= 0) {
        return;
    }
    const std::string payload(static_cast<const char*>(msg->payload),
                              static_cast<std::size_t>(msg->payloadlen));
    std::cout << payload << std::endl;
}

} // namespace

int main(int argc, char** argv) {
    const std::string host = (argc >= 2) ? argv[1] : "127.0.0.1";
    const int port = (argc >= 3) ? std::stoi(argv[2]) : 1883;
    const std::string topic = (argc >= 4) ? argv[3] : "sensors/data";

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    mosquitto_lib_init();
    mosquitto* mosq = mosquitto_new("serial-gateway-mqtt-receiver", true, nullptr);
    if (mosq == nullptr) {
        std::cerr << "mosquitto_new failed\n";
        mosquitto_lib_cleanup();
        return 1;
    }

    mosquitto_message_callback_set(mosq, onMessage);

    if (mosquitto_connect(mosq, host.c_str(), port, 60) != MOSQ_ERR_SUCCESS) {
        std::cerr << "mqtt connect failed to " << host << ':' << port << "\n";
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 2;
    }

    if (mosquitto_subscribe(mosq, nullptr, topic.c_str(), 0) != MOSQ_ERR_SUCCESS) {
        std::cerr << "mqtt subscribe failed for topic " << topic << "\n";
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 3;
    }

    while (!g_stop.load()) {
        const int rc = mosquitto_loop(mosq, 500, 1);
        if (rc != MOSQ_ERR_SUCCESS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (mosquitto_reconnect(mosq) != MOSQ_ERR_SUCCESS) {
                continue;
            }
        }
    }

    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}
