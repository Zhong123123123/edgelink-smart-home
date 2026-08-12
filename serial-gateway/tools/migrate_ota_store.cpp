#include "storage/sqlite_store.hpp"
#include "ota/ota_task.hpp"

#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

int main(int argc, char** argv) {
    const std::string old_json = (argc > 1) ? argv[1] : "data/ota_tasks.json";
    const std::string sqlite_db = (argc > 2) ? argv[2] : "data/ota_tasks.db";

    std::ifstream in(old_json);
    if (!in.is_open()) {
        std::cerr << "open old json failed: " << old_json << "\n";
        return 1;
    }

    std::ostringstream oss;
    oss << in.rdbuf();
    const std::string text = oss.str();

    sg::storage::PersistentStore store(sqlite_db);
    std::string err;
    std::size_t task_count = 0;
    std::size_t event_count = 0;

    const std::regex task_re("\\{\\\"task_uuid\\\":\\\"([^\\\"]+)\\\",\\\"device_id\\\":([0-9]+),\\\"device_type\\\":\\\"([^\\\"]*)\\\",\\\"firmware_id\\\":\\\"([^\\\"]*)\\\",\\\"state\\\":\\\"([^\\\"]+)\\\",\\\"last_error\\\":\\\"([^\\\"]*)\\\"\\}");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), task_re); it != std::sregex_iterator(); ++it) {
        sg::ota::OtaTask t;
        t.task_uuid = (*it)[1].str();
        t.device_id = static_cast<std::uint8_t>(std::stoi((*it)[2].str()));
        t.device_type = (*it)[3].str();
        t.firmware_id = (*it)[4].str();
        t.last_error = (*it)[6].str();
        if (!sg::ota::fromString((*it)[5].str(), t.state)) {
            t.state = sg::ota::OtaTaskState::CREATED;
        }
        if (!store.saveTask(t, err)) {
            std::cerr << "saveTask failed: " << err << " task_uuid=" << t.task_uuid << "\n";
            return 2;
        }
        ++task_count;
    }

    const std::regex ev_re("\\{\\\"task_uuid\\\":\\\"([^\\\"]+)\\\",\\\"ts_unix_ms\\\":([0-9]+),\\\"state\\\":\\\"([^\\\"]+)\\\",\\\"detail\\\":\\\"([^\\\"]*)\\\"\\}");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), ev_re); it != std::sregex_iterator(); ++it) {
        sg::storage::OtaTaskEvent e;
        e.task_uuid = (*it)[1].str();
        e.ts_unix_ms = static_cast<std::uint64_t>(std::stoull((*it)[2].str()));
        e.state = (*it)[3].str();
        e.detail = (*it)[4].str();
        if (!store.appendEvent(e, err)) {
            std::cerr << "appendEvent failed: " << err << " task_uuid=" << e.task_uuid << "\n";
            return 3;
        }
        ++event_count;
    }

    std::cout << "migration done: tasks=" << task_count << ", events=" << event_count
              << ", sqlite=" << sqlite_db << "\n";
    return 0;
}
