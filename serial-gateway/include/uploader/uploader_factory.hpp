#pragma once

#include "config/config.hpp"
#include "runtime/runtime_stats.hpp"
#include "uploader/i_uploader.hpp"

#include <memory>

namespace sg {

class UploaderFactory {
public:
    static std::unique_ptr<IUploader> create(const UploaderConfig& config, RuntimeStats& stats);
};

} // namespace sg
