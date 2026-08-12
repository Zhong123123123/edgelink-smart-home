#include "runtime/resource_usage.hpp"

#include <sys/resource.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <string>

namespace sg {

namespace {

std::uint64_t tvToMs(const timeval& tv) {
    return static_cast<std::uint64_t>(tv.tv_sec) * 1000ULL + static_cast<std::uint64_t>(tv.tv_usec / 1000);
}

} // namespace

ResourceUsage ResourceUsageReader::read() {
    ResourceUsage ru;

    {
        std::ifstream statm("/proc/self/statm");
        std::uint64_t size_pages = 0;
        std::uint64_t rss_pages = 0;
        if (statm >> size_pages >> rss_pages) {
            const long page_sz = sysconf(_SC_PAGESIZE);
            const std::uint64_t page_kb = (page_sz > 0) ? static_cast<std::uint64_t>(page_sz / 1024) : 4ULL;
            ru.vms_kb = size_pages * page_kb;
            ru.rss_kb = rss_pages * page_kb;
        }
    }

    {
        rusage r{};
        if (getrusage(RUSAGE_SELF, &r) == 0) {
            ru.user_cpu_ms = tvToMs(r.ru_utime);
            ru.sys_cpu_ms = tvToMs(r.ru_stime);
        }
    }

    {
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line)) {
            if (line.rfind("Threads:", 0) == 0) {
                std::istringstream iss(line.substr(8));
                iss >> ru.threads;
                break;
            }
        }
    }

    return ru;
}

std::string ResourceUsageReader::toLine(const ResourceUsage& ru) {
    std::ostringstream oss;
    oss << "res rss_kb=" << ru.rss_kb
        << " vms_kb=" << ru.vms_kb
        << " user_cpu_ms=" << ru.user_cpu_ms
        << " sys_cpu_ms=" << ru.sys_cpu_ms
        << " threads=" << ru.threads;
    return oss.str();
}

} // namespace sg
