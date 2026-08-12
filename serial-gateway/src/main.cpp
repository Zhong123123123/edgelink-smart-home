#include "app/app_controller.hpp"
#include "common/logger.hpp"
#include "config/config.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_reload{false};

void onStopSignal(int) {
    g_stop.store(true);
}

void onReloadSignal(int) {
    g_reload.store(true);
}

std::string readConfigPath(int argc, char** argv) {
    std::string path = "config/gateway.yaml";
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        const std::string prefix = "--config=";
        if (arg.rfind(prefix, 0) == 0) {
            path = arg.substr(prefix.size());
        }
    }
    return path;
}

bool loadConfig(const std::string& path, sg::GatewayConfig& cfg) {
    std::string err;
    if (!sg::ConfigLoader::loadFromFile(path, cfg, err)) {
        sg::Logger::instance().error("config load failed: " + err);
        return false;
    }
    return true;
}

std::filesystem::file_time_type readConfigMtime(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return std::filesystem::file_time_type::min();
    }
    return std::filesystem::last_write_time(path, ec);
}

std::string sanitizeToken(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) {
        return "serial";
    }
    return out;
}

std::string appendSuffixToPath(const std::string& file_path, const std::string& suffix) {
    std::filesystem::path p(file_path);
    const std::string stem = p.stem().string();
    const std::string ext = p.extension().string();
    p.replace_filename(stem + "_" + suffix + ext);
    return p.string();
}

int addPortOffset(int base_port, std::size_t idx) {
    const std::size_t candidate = static_cast<std::size_t>(base_port) + idx;
    if (candidate > 65535) {
        return base_port;
    }
    return static_cast<int>(candidate);
}

sg::GatewayConfig buildInstanceConfig(const sg::GatewayConfig& base,
                                      const sg::GatewayConfig::SerialInstanceConfig& serial_item,
                                      std::size_t idx,
                                      std::size_t total) {
    sg::GatewayConfig cfg = base;
    cfg.serial = serial_item.serial;
    cfg.serial.instance_name = serial_item.name.empty() ? serial_item.serial.instance_name : serial_item.name;

    const std::string token = sanitizeToken(cfg.serial.instance_name);
    cfg.uploader.disk_cache_file = appendSuffixToPath(base.uploader.disk_cache_file, token);

    if (cfg.uploader.type == "mqtt" && !cfg.uploader.mqtt_client_id.empty()) {
        cfg.uploader.mqtt_client_id = cfg.uploader.mqtt_client_id + "-" + token;
    }
    if (cfg.uploader.type == "mqtt" && !cfg.uploader.mqtt_control_client_id.empty()) {
        cfg.uploader.mqtt_control_client_id = cfg.uploader.mqtt_control_client_id + "-" + token;
    }

    if (idx > 0) {
        cfg.command.enabled = false;
        cfg.heartbeat.enabled = false;
        cfg.mqtt_device_ingress.enabled = false;
        cfg.uploader.mqtt_control_enabled = false;
        if (cfg.monitor.enabled) {
            cfg.monitor.port = addPortOffset(base.monitor.port, idx);
        }
    } else if (cfg.monitor.enabled) {
        cfg.monitor.port = base.monitor.port;
    }

    sg::Logger::instance().info("prepared serial instance=" + cfg.serial.instance_name +
                                " device=" + cfg.serial.device +
                                " monitor_port=" + std::to_string(cfg.monitor.port) +
                                " command_enabled=" + std::string(cfg.command.enabled ? "true" : "false") +
                                " index=" + std::to_string(idx + 1) + "/" + std::to_string(total));
    return cfg;
}

using AppList = std::vector<std::unique_ptr<sg::AppController>>;

void stopApps(AppList& apps) {
    for (auto& app : apps) {
        if (app != nullptr) {
            app->stop();
        }
    }
    apps.clear();
}

bool startApps(const sg::GatewayConfig& config, AppList& out_apps) {
    AppList apps;

    if (config.serials.empty()) {
        auto app = std::make_unique<sg::AppController>(config);
        if (!app->start()) {
            return false;
        }
        apps.push_back(std::move(app));
    } else {
        for (std::size_t i = 0; i < config.serials.size(); ++i) {
            const auto instance_cfg = buildInstanceConfig(config, config.serials[i], i, config.serials.size());
            auto app = std::make_unique<sg::AppController>(instance_cfg);
            if (!app->start()) {
                stopApps(apps);
                return false;
            }
            apps.push_back(std::move(app));
        }
    }

    out_apps = std::move(apps);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const std::string config_path = readConfigPath(argc, argv);

    sg::GatewayConfig config;
    std::string err;
    if (!sg::ConfigLoader::loadFromFile(config_path, config, err)) {
        return 1;
    }

    if (!sg::Logger::instance().init(config.log.file, config.log.level, config.log.also_stdout, err)) {
        return 2;
    }

    std::signal(SIGINT, onStopSignal);
    std::signal(SIGTERM, onStopSignal);
    std::signal(SIGHUP, onReloadSignal);

    AppList apps;
    if (!startApps(config, apps)) {
        return 3;
    }

    auto cfg_mtime = readConfigMtime(config_path);
    auto next_watch = std::chrono::steady_clock::now() + std::chrono::seconds(config.reload.check_interval_sec);

    while (!g_stop.load()) {
        bool should_reload = g_reload.exchange(false);

        if (!should_reload && config.reload.enabled && std::chrono::steady_clock::now() >= next_watch) {
            const auto now_mtime = readConfigMtime(config_path);
            if (now_mtime != std::filesystem::file_time_type::min() && now_mtime > cfg_mtime) {
                should_reload = true;
                cfg_mtime = now_mtime;
            }
            next_watch = std::chrono::steady_clock::now() + std::chrono::seconds(config.reload.check_interval_sec);
        }

        if (should_reload) {
            sg::Logger::instance().info("reload requested, applying config");

            sg::GatewayConfig new_cfg;
            if (!loadConfig(config_path, new_cfg)) {
                sg::Logger::instance().warn("reload skipped: new config invalid");
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            if (!sg::Logger::instance().init(new_cfg.log.file, new_cfg.log.level, new_cfg.log.also_stdout, err)) {
                sg::Logger::instance().warn("reload skipped: logger reinit failed");
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            const sg::GatewayConfig old_cfg = config;
            stopApps(apps);

            if (startApps(new_cfg, apps)) {
                config = new_cfg;
                cfg_mtime = readConfigMtime(config_path);
                next_watch = std::chrono::steady_clock::now() + std::chrono::seconds(config.reload.check_interval_sec);
                sg::Logger::instance().info("reload applied successfully");
            } else {
                sg::Logger::instance().error("reload failed, rolling back to previous config");
                std::string rollback_logger_err;
                if (!sg::Logger::instance().init(old_cfg.log.file,
                                                 old_cfg.log.level,
                                                 old_cfg.log.also_stdout,
                                                 rollback_logger_err)) {
                    sg::Logger::instance().error(
                        "logger rollback failed: " + rollback_logger_err);
                    return 4;
                }
                if (!startApps(old_cfg, apps)) {
                    sg::Logger::instance().error("rollback start failed, exiting");
                    return 4;
                }
                config = old_cfg;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    stopApps(apps);
    return 0;
}
