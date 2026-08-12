#include "common/mqtt_runtime.hpp"

#include "common/logger.hpp"

#include <mutex>

#ifdef SG_HAVE_MOSQUITTO
#include <mosquitto.h>
#endif

namespace sg {

namespace {

std::mutex g_mqtt_runtime_mutex;
int g_mqtt_runtime_refcount = 0;

} // namespace

bool mqttRuntimeAcquire() {
#ifndef SG_HAVE_MOSQUITTO
    return true;
#else
    std::lock_guard<std::mutex> lock(g_mqtt_runtime_mutex);
    if (g_mqtt_runtime_refcount == 0) {
        const int rc = mosquitto_lib_init();
        if (rc != MOSQ_ERR_SUCCESS) {
            Logger::instance().error("mosquitto_lib_init failed rc=" + std::to_string(rc));
            return false;
        }
        Logger::instance().info("mosquitto runtime initialized");
    }
    ++g_mqtt_runtime_refcount;
    return true;
#endif
}

void mqttRuntimeRelease() {
#ifdef SG_HAVE_MOSQUITTO
    std::lock_guard<std::mutex> lock(g_mqtt_runtime_mutex);
    if (g_mqtt_runtime_refcount <= 0) {
        Logger::instance().warn("mosquitto runtime release ignored: refcount already zero");
        return;
    }
    --g_mqtt_runtime_refcount;
    if (g_mqtt_runtime_refcount == 0) {
        mosquitto_lib_cleanup();
        Logger::instance().info("mosquitto runtime cleaned up");
    }
#endif
}

} // namespace sg
