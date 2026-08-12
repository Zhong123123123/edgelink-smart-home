#include "ota/firmware_store.hpp"
#include "ota/ota_state_machine.hpp"
#include "ota/ota_task_manager.hpp"
#include "ota/stm32_ota_adapter.hpp"
#include "storage/sqlite_store.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> readFile(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool parseHostPort(const std::string& s, std::string& host, int& port) {
    const auto pos = s.find(':');
    if (pos == std::string::npos) return false;
    host = s.substr(0, pos);
    port = std::stoi(s.substr(pos + 1));
    return true;
}

} // namespace

int main(int argc, char** argv) {
    sg::ota::FirmwareStore fw_store("data/firmware");
    sg::storage::PersistentStore pst("data/ota_tasks.json");
    sg::ota::OtaTaskManager task_mgr(pst);

    if (argc >= 3 && std::string(argv[1]) == "firmware" && std::string(argv[2]) == "list") {
        const auto list = fw_store.list();
        for (const auto& f : list) {
            std::cout << f.manifest.firmware_id << " " << f.manifest.device_type << " " << f.manifest.version << "\n";
        }
        return 0;
    }
    if (argc >= 6 && std::string(argv[1]) == "firmware" && std::string(argv[2]) == "add") {
        std::string manifest;
        std::string image;
        for (int i = 3; i + 1 < argc; ++i) {
            if (std::string(argv[i]) == "--manifest") manifest = argv[++i];
            else if (std::string(argv[i]) == "--image") image = argv[++i];
        }
        std::string err;
        if (!fw_store.registerFirmware(manifest, image, err)) {
            std::cerr << "register failed: " << err << "\n";
            return 1;
        }
        std::cout << "ok\n";
        return 0;
    }

    if (argc >= 3 && std::string(argv[1]) == "ota" && std::string(argv[2]) == "list") {
        for (const auto& t : task_mgr.list()) {
            std::cout << t.task_uuid << " device=" << static_cast<int>(t.device_id)
                      << " fw=" << t.firmware_id << " state=" << sg::ota::toString(t.state)
                      << " err=" << t.last_error << "\n";
        }
        return 0;
    }

    if (argc >= 3 && std::string(argv[1]) == "ota" && std::string(argv[2]) == "start") {
        int device_id = -1;
        std::string firmware_id;
        std::string target = "127.0.0.1:19090";
        std::string transport = "serial";
        for (int i = 3; i + 1 < argc; ++i) {
            const std::string k = argv[i];
            if (k == "--device-id") device_id = std::stoi(argv[++i]);
            else if (k == "--firmware-id") firmware_id = argv[++i];
            else if (k == "--target") target = argv[++i];
            else if (k == "--transport") transport = argv[++i];
        }
        if (device_id < 0 || firmware_id.empty()) {
            std::cerr << "missing --device-id or --firmware-id\n";
            return 1;
        }
        if (transport != "serial" && transport != "tcp_binary") {
            std::cerr << "invalid --transport, expect serial|tcp_binary\n";
            return 1;
        }

        std::string err;
        auto fw = fw_store.get(firmware_id);
        if (!fw) {
            std::cerr << "firmware not found\n";
            return 1;
        }

        const auto task = task_mgr.create(static_cast<std::uint8_t>(device_id), fw->manifest.device_type, firmware_id, err);
        if (!err.empty()) {
            std::cerr << "task create error: " << err << "\n";
            return 1;
        }

        task_mgr.updateState(task.task_uuid, sg::ota::OtaTaskState::WAIT_DEVICE_ONLINE, "device online", err);
        task_mgr.updateState(task.task_uuid, sg::ota::OtaTaskState::PRECHECK, "precheck ok", err);
        task_mgr.updateState(task.task_uuid, sg::ota::OtaTaskState::PREPARE_DEVICE, "prepare ota", err);
        task_mgr.updateState(task.task_uuid, sg::ota::OtaTaskState::TRANSFERRING, "start transfer", err);

        std::string host;
        int port = 0;
        if (!parseHostPort(target, host, port)) {
            std::cerr << "invalid --target, expect host:port\n";
            return 1;
        }

        sg::ota::Stm32OtaAdapter adapter;
        sg::ota::Stm32OtaOptions opt;
        opt.host = host;
        opt.port = port;
        opt.timeout_ms = 5000;
        if (!fw->manifest.entry_addr.empty()) {
            try {
                opt.target_base = static_cast<std::uint32_t>(std::stoul(fw->manifest.entry_addr, nullptr, 0));
            } catch (...) {
                opt.target_base = 0U;
            }
        }
        const auto img = readFile(fw->image_path);
        const std::uint32_t crc = sg::ota::FirmwareStore::crc32File(fw->image_path, err);
        if (!err.empty()) {
            std::cerr << err << "\n";
            return 1;
        }

        bool ok = adapter.run(img, crc, opt, err, [&](int pct) {
            task_mgr.updateState(task.task_uuid, sg::ota::OtaTaskState::TRANSFERRING, "progress=" + std::to_string(pct), err);
        });
        if (!ok) {
            task_mgr.fail(task.task_uuid, err, err);
            std::cerr << "ota failed task=" << task.task_uuid << " err=" << err << "\n";
            return 1;
        }

        task_mgr.updateState(task.task_uuid, sg::ota::OtaTaskState::VERIFYING, "verify ok", err);
        task_mgr.updateState(task.task_uuid, sg::ota::OtaTaskState::COMMITTING, "commit ok", err);
        task_mgr.updateState(task.task_uuid, sg::ota::OtaTaskState::REBOOTING, "rebooting", err);
        task_mgr.updateState(task.task_uuid, sg::ota::OtaTaskState::VERSION_CHECK, "version checked", err);
        task_mgr.updateState(task.task_uuid, sg::ota::OtaTaskState::HEALTH_CONFIRM, "health confirmed", err);
        task_mgr.updateState(task.task_uuid, sg::ota::OtaTaskState::SUCCESS, "ota success", err);

        std::cout << "task=" << task.task_uuid << " state=SUCCESS transport=" << transport << "\n";
        return 0;
    }

    if (argc >= 5 && std::string(argv[1]) == "ota" && std::string(argv[2]) == "status" && std::string(argv[3]) == "--task") {
        auto t = task_mgr.get(argv[4]);
        if (!t) {
            std::cerr << "task not found\n";
            return 1;
        }
        std::cout << t->task_uuid << " state=" << sg::ota::toString(t->state) << " error=" << t->last_error << "\n";
        return 0;
    }

    if (argc >= 5 && std::string(argv[1]) == "ota" && std::string(argv[2]) == "events" && std::string(argv[3]) == "--task") {
        for (const auto& e : task_mgr.events(argv[4])) {
            std::cout << e.ts_unix_ms << " " << e.state << " " << e.detail << "\n";
        }
        return 0;
    }

    if (argc >= 5 && std::string(argv[1]) == "ota" && std::string(argv[2]) == "cancel" && std::string(argv[3]) == "--task") {
        std::string err;
        if (!task_mgr.cancel(argv[4], err)) {
            std::cerr << "cancel failed: " << err << "\n";
            return 1;
        }
        std::cout << "ok\n";
        return 0;
    }

    if (argc >= 5 && std::string(argv[1]) == "ota" && std::string(argv[2]) == "retry" && std::string(argv[3]) == "--task") {
        std::string err;
        if (!task_mgr.retry(argv[4], err)) {
            std::cerr << "retry failed: " << err << "\n";
            return 1;
        }
        std::cout << "ok\n";
        return 0;
    }

    std::cout << "usage:\n"
              << "  otactl firmware add --manifest manifest.json --image app.bin\n"
              << "  otactl firmware list\n"
              << "  otactl ota start --device-id 1 --firmware-id fw-1.0.0 --target 127.0.0.1:19090 --transport serial\n"
              << "  otactl ota list\n"
              << "  otactl ota status --task ota-xxx\n"
              << "  otactl ota events --task ota-xxx\n"
              << "  otactl ota cancel --task ota-xxx\n"
              << "  otactl ota retry --task ota-xxx\n";
    return 0;
}
