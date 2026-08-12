#include "ota/esp32_ota_adapter.hpp"

#include <cassert>

int main() {
    sg::ota::Esp32OtaStart req;
    req.device_id = 2;
    req.command_id = 3001;
    req.firmware_url = "http://gateway:9080/fw/esp32-wifi-node-1.1.0/app.bin";
    req.version = "1.1.0";
    req.size = 1024;
    req.crc32 = "0x12345678";

    const auto line = sg::ota::Esp32OtaAdapter::buildOtaStartJson(req);
    assert(line.find("\"command_type\":\"ota_start\"") != std::string::npos);

    const std::string status_json =
        "{\"device_id\":2,\"event_type\":\"ota_status\",\"command_id\":3001,\"ota_state\":\"downloading\",\"progress\":42,\"version_from\":\"1.0.0\",\"version_to\":\"1.1.0\",\"error_code\":0,\"seq\":1201}";
    const auto st = sg::ota::Esp32OtaAdapter::parseOtaStatusJson(status_json);
    assert(st.has_value());
    assert(st->progress == 42);
    assert(st->ota_state == "downloading");
    return 0;
}
