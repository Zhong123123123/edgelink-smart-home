#include "runtime/resource_usage.hpp"

#include <iostream>

int main() {
    const auto ru = sg::ResourceUsageReader::read();
    if (ru.rss_kb == 0 || ru.vms_kb == 0) {
        std::cerr << "resource usage memory values should be > 0\n";
        return 1;
    }
    if (ru.threads <= 0) {
        std::cerr << "resource usage threads should be > 0\n";
        return 2;
    }
    const auto line = sg::ResourceUsageReader::toLine(ru);
    if (line.find("rss_kb=") == std::string::npos || line.find("threads=") == std::string::npos) {
        std::cerr << "resource usage line format invalid\n";
        return 3;
    }
    return 0;
}
