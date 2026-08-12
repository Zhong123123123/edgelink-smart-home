#include "uploader/uploader_factory.hpp"

#include "uploader/cached_uploader.hpp"
#include "uploader/mqtt_uploader.hpp"
#include "uploader/tcp_uploader.hpp"

namespace sg {

std::unique_ptr<IUploader> UploaderFactory::create(const UploaderConfig& config, RuntimeStats& stats) {
    std::unique_ptr<IUploader> base;
    if (config.type == "mqtt") {
        base = std::make_unique<MqttUploader>(config, stats);
    } else {
        base = std::make_unique<TcpUploader>(config, stats);
    }

    if (config.disk_cache_enabled) {
        return std::make_unique<CachedUploader>(std::move(base), config, stats);
    }
    return base;
}

} // namespace sg
