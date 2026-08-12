#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sg::ota {

struct OtaManifest {
    std::string firmware_id;
    std::string device_type;
    std::string version;
    std::string image;
    std::uint64_t image_size = 0;
    std::string crc32;
    std::string sha256;
    std::string entry_addr;
    std::uint32_t image_version = 0;
    std::uint64_t slot_size = 0;
    std::string min_bootloader_version;
    std::vector<std::string> features;
};

std::optional<OtaManifest> parseManifestText(const std::string& text, std::string& err);
std::optional<OtaManifest> loadManifestFile(const std::string& path, std::string& err);
std::string manifestToJson(const OtaManifest& manifest);

} // namespace sg::ota
