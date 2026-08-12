#include "ota/firmware_store.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>

int main() {
    namespace fs = std::filesystem;
    fs::create_directories("build/test_fw_pkg");

    {
        std::ofstream img("build/test_fw_pkg/app.bin", std::ios::binary);
        for (int i = 0; i < 16; ++i) img.put(static_cast<char>(i));
    }

    std::string crc_err;
    const auto crc = sg::ota::FirmwareStore::crc32File("build/test_fw_pkg/app.bin", crc_err);
    assert(crc_err.empty());

    char crc_buf[16];
    std::snprintf(crc_buf, sizeof(crc_buf), "0x%08X", crc);

    {
        std::ofstream mf("build/test_fw_pkg/manifest.json");
        mf << "{\n"
           << "\"firmware_id\":\"fw-1.0.0\",\n"
           << "\"device_type\":\"stm32f407-smarthome\",\n"
           << "\"version\":\"1.0.0\",\n"
           << "\"image\":\"app.bin\",\n"
           << "\"image_size\":16,\n"
           << "\"crc32\":\"" << crc_buf << "\"\n"
           << "}";
    }

    sg::ota::FirmwareStore store("build/firmware_store");
    std::string err;
    assert(store.registerFirmware("build/test_fw_pkg/manifest.json", "build/test_fw_pkg/app.bin", err));
    const auto list = store.list();
    assert(!list.empty());
    return 0;
}
