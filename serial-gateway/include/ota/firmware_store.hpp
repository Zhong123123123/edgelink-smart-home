#pragma once

#include "ota/ota_manifest.hpp"

#include <optional>
#include <string>
#include <vector>

namespace sg::ota {

struct FirmwareRecord {
    OtaManifest manifest;
    std::string base_dir;
    std::string image_path;
};

class FirmwareStore {
public:
    explicit FirmwareStore(std::string root_dir);

    bool registerFirmware(const std::string& manifest_path, const std::string& image_path, std::string& err);
    std::vector<FirmwareRecord> list() const;
    std::optional<FirmwareRecord> get(const std::string& firmware_id) const;
    bool remove(const std::string& firmware_id, std::string& err);

    static std::uint32_t crc32File(const std::string& path, std::string& err);

private:
    std::string root_dir_;
};

} // namespace sg::ota
