#include "ota/ota_manifest.hpp"

#include <cassert>
#include <string>

int main() {
    const std::string json = R"({
  "firmware_id":"stm32-smarthome-1.1.0",
  "device_type":"stm32f407-smarthome",
  "version":"1.1.0",
  "image":"app.bin",
  "image_size":286720,
  "crc32":"0x91A2B3C4",
  "features":["ota_confirm","sensor"]
})";
    std::string err;
    auto m = sg::ota::parseManifestText(json, err);
    assert(m.has_value());
    assert(m->firmware_id == "stm32-smarthome-1.1.0");
    assert(m->image_size == 286720);
    assert(m->features.size() == 2);
    return 0;
}
