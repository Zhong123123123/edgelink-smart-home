#include "common/logger.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace sg {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

bool Logger::init(const std::string& file_path, const std::string& level_name, bool also_stdout, std::string& err) {
    std::lock_guard<std::mutex> lock(mutex_);
    threshold_ = parseLevel(level_name);
    also_stdout_ = also_stdout;
    if (file_.is_open()) {
        file_.close();
    }
    file_.clear();

    std::filesystem::path path(file_path);
    if (!path.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            err = "failed to create log dir: " + ec.message();
            return false;
        }
    }

    file_.open(file_path, std::ios::app);
    if (!file_) {
        err = "failed to open log file: " + file_path;
        return false;
    }
    return true;
}

void Logger::error(const std::string& msg) { log(LogLevel::ERROR, msg); }
void Logger::warn(const std::string& msg) { log(LogLevel::WARN, msg); }
void Logger::info(const std::string& msg) { log(LogLevel::INFO, msg); }
void Logger::debug(const std::string& msg) { log(LogLevel::DEBUG, msg); }

void Logger::log(LogLevel level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(level) > static_cast<int>(threshold_)) {
        return;
    }
    const std::string line = nowString() + " [" + levelToString(level) + "] " + msg;
    if (file_) {
        file_ << line << '\n';
        file_.flush();
    }
    if (also_stdout_) {
        std::cout << line << std::endl;
    }
}

std::string Logger::nowString() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&tt, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::WARN: return "WARN";
        case LogLevel::INFO: return "INFO";
        case LogLevel::DEBUG: return "DEBUG";
    }
    return "INFO";
}

LogLevel Logger::parseLevel(const std::string& level_name) {
    if (level_name == "ERROR") return LogLevel::ERROR;
    if (level_name == "WARN") return LogLevel::WARN;
    if (level_name == "DEBUG") return LogLevel::DEBUG;
    return LogLevel::INFO;
}

} // namespace sg
