#include "ota/firmware_store.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace sg::ota {
namespace fs = std::filesystem;

namespace {
std::uint32_t crc32Update(std::uint32_t crc, const std::uint8_t* data, std::size_t len) {
    crc = ~crc;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            const std::uint32_t mask = -(crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

std::string toHexCrc(std::uint32_t crc) {
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << crc;
    return oss.str();
}
} // namespace

FirmwareStore::FirmwareStore(std::string root_dir) : root_dir_(std::move(root_dir)) {
    std::error_code ec;
    fs::create_directories(root_dir_, ec);
}

std::uint32_t FirmwareStore::crc32File(const std::string& path, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        err = "failed to open image: " + path;
        return 0;
    }
    std::array<std::uint8_t, 4096> buf{};
    std::uint32_t crc = 0;
    while (in.good()) {
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        if (n > 0) crc = crc32Update(crc, buf.data(), static_cast<std::size_t>(n));
    }
    return crc;
}

bool FirmwareStore::registerFirmware(const std::string& manifest_path, const std::string& image_path, std::string& err) {
    auto m = loadManifestFile(manifest_path, err);
    if (!m) return false;

    std::error_code ec;
    const auto img_size = fs::file_size(image_path, ec);
    if (ec) {
        err = "cannot read image size";
        return false;
    }
    if (img_size != m->image_size) {
        err = "image_size mismatch";
        return false;
    }
    const std::uint32_t crc = crc32File(image_path, err);
    if (!err.empty()) return false;
    if (!m->crc32.empty() && toHexCrc(crc) != m->crc32) {
        err = "crc32 mismatch";
        return false;
    }

    const fs::path out_dir = fs::path(root_dir_) / m->firmware_id;
    fs::create_directories(out_dir, ec);
    if (ec) {
        err = "failed to create firmware dir";
        return false;
    }

    const fs::path out_img = out_dir / "app.bin";
    const fs::path out_manifest = out_dir / "manifest.json";
    fs::copy_file(image_path, out_img, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        err = "failed to copy image";
        return false;
    }
    std::ofstream mf(out_manifest);
    if (!mf.is_open()) {
        err = "failed to write manifest";
        return false;
    }
    mf << manifestToJson(*m);
    return true;
}

std::vector<FirmwareRecord> FirmwareStore::list() const {
    std::vector<FirmwareRecord> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root_dir_, ec)) {
        if (ec || !e.is_directory()) continue;
        const fs::path manifest = e.path() / "manifest.json";
        std::string err;
        auto parsed = loadManifestFile(manifest.string(), err);
        if (!parsed) continue;
        out.push_back(FirmwareRecord{*parsed, e.path().string(), (e.path() / "app.bin").string()});
    }
    return out;
}

std::optional<FirmwareRecord> FirmwareStore::get(const std::string& firmware_id) const {
    const fs::path base = fs::path(root_dir_) / firmware_id;
    const fs::path manifest = base / "manifest.json";
    std::string err;
    auto parsed = loadManifestFile(manifest.string(), err);
    if (!parsed) return std::nullopt;
    return FirmwareRecord{*parsed, base.string(), (base / "app.bin").string()};
}

bool FirmwareStore::remove(const std::string& firmware_id, std::string& err) {
    std::error_code ec;
    const auto count = fs::remove_all(fs::path(root_dir_) / firmware_id, ec);
    if (ec) {
        err = "remove failed";
        return false;
    }
    if (count == 0) {
        err = "firmware not found";
        return false;
    }
    return true;
}

} // namespace sg::ota
