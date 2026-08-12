#pragma once

#include <cstdint>
#include <string>

namespace sg {

struct ResourceUsage {
    std::uint64_t rss_kb = 0;
    std::uint64_t vms_kb = 0;
    std::uint64_t user_cpu_ms = 0;
    std::uint64_t sys_cpu_ms = 0;
    int threads = 0;
};

class ResourceUsageReader {
public:
    static ResourceUsage read();
    static std::string toLine(const ResourceUsage& ru);
};

} // namespace sg
