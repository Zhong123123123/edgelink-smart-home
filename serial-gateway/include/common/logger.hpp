#pragma once

#include <fstream>
#include <mutex>
#include <string>

namespace sg {

enum class LogLevel {
    ERROR = 0,
    WARN = 1,
    INFO = 2,
    DEBUG = 3
};

class Logger {
public:
    static Logger& instance();

    bool init(const std::string& file_path, const std::string& level_name, bool also_stdout, std::string& err);
    void error(const std::string& msg);
    void warn(const std::string& msg);
    void info(const std::string& msg);
    void debug(const std::string& msg);

private:
    Logger() = default;
    void log(LogLevel level, const std::string& msg);
    static std::string nowString();
    static std::string levelToString(LogLevel level);
    static LogLevel parseLevel(const std::string& level_name);

    std::mutex mutex_;
    std::ofstream file_;
    LogLevel threshold_ = LogLevel::INFO;
    bool also_stdout_ = true;
};

} // namespace sg
