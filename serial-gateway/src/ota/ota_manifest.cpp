#include "ota/ota_manifest.hpp"

#include <fstream>
#include <regex>
#include <sstream>

namespace sg::ota {
namespace {

std::optional<std::string> jsonString(const std::string& text, const std::string& key) {
    const std::regex re("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
    std::smatch m;
    if (!std::regex_search(text, m, re) || m.size() < 2) return std::nullopt;
    return m[1].str();
}

std::optional<std::uint64_t> jsonUint(const std::string& text, const std::string& key) {
    const std::regex re("\\\"" + key + "\\\"\\s*:\\s*([0-9]+)");
    std::smatch m;
    if (!std::regex_search(text, m, re) || m.size() < 2) return std::nullopt;
    try {
        return static_cast<std::uint64_t>(std::stoull(m[1].str()));
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::string> jsonStringArray(const std::string& text, const std::string& key) {
    const std::regex re("\\\"" + key + "\\\"\\s*:\\s*\\[(.*?)\\]");
    std::smatch m;
    if (!std::regex_search(text, m, re) || m.size() < 2) return {};
    std::vector<std::string> out;
    const std::string body = m[1].str();
    const std::regex item_re("\\\"([^\\\"]+)\\\"");
    auto it = std::sregex_iterator(body.begin(), body.end(), item_re);
    for (; it != std::sregex_iterator(); ++it) out.push_back((*it)[1].str());
    return out;
}

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else out.push_back(c);
    }
    return out;
}

} // namespace

std::optional<OtaManifest> parseManifestText(const std::string& text, std::string& err) {
    OtaManifest m;
    const auto firmware_id = jsonString(text, "firmware_id");
    const auto device_type = jsonString(text, "device_type");
    const auto version = jsonString(text, "version");
    const auto image = jsonString(text, "image");
    const auto image_size = jsonUint(text, "image_size");
    const auto crc32 = jsonString(text, "crc32");

    if (!firmware_id || !device_type || !version || !image || !image_size || !crc32) {
        err = "manifest missing required fields";
        return std::nullopt;
    }

    m.firmware_id = *firmware_id;
    m.device_type = *device_type;
    m.version = *version;
    m.image = *image;
    m.image_size = *image_size;
    m.crc32 = *crc32;
    m.sha256 = jsonString(text, "sha256").value_or("");
    m.entry_addr = jsonString(text, "entry_addr").value_or("");
    m.image_version = static_cast<std::uint32_t>(jsonUint(text, "image_version").value_or(0));
    m.slot_size = jsonUint(text, "slot_size").value_or(0);
    m.min_bootloader_version = jsonString(text, "min_bootloader_version").value_or("");
    m.features = jsonStringArray(text, "features");
    return m;
}

std::optional<OtaManifest> loadManifestFile(const std::string& path, std::string& err) {
    std::ifstream in(path);
    if (!in.is_open()) {
        err = "failed to open manifest: " + path;
        return std::nullopt;
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    return parseManifestText(oss.str(), err);
}

std::string manifestToJson(const OtaManifest& m) {
    std::ostringstream oss;
    oss << "{\n"
        << "  \"firmware_id\": \"" << jsonEscape(m.firmware_id) << "\",\n"
        << "  \"device_type\": \"" << jsonEscape(m.device_type) << "\",\n"
        << "  \"version\": \"" << jsonEscape(m.version) << "\",\n"
        << "  \"image\": \"" << jsonEscape(m.image) << "\",\n"
        << "  \"image_size\": " << m.image_size << ",\n"
        << "  \"crc32\": \"" << jsonEscape(m.crc32) << "\",\n"
        << "  \"sha256\": \"" << jsonEscape(m.sha256) << "\",\n"
        << "  \"entry_addr\": \"" << jsonEscape(m.entry_addr) << "\",\n"
        << "  \"image_version\": " << m.image_version << ",\n"
        << "  \"slot_size\": " << m.slot_size << ",\n"
        << "  \"min_bootloader_version\": \"" << jsonEscape(m.min_bootloader_version) << "\",\n"
        << "  \"features\": [";
    for (std::size_t i = 0; i < m.features.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << "\"" << jsonEscape(m.features[i]) << "\"";
    }
    oss << "]\n}";
    return oss.str();
}

} // namespace sg::ota
