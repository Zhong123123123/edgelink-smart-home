cmake_minimum_required(VERSION 3.16)
project(serial_gateway VERSION 0.1.0 LANGUAGES CXX)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SERIAL_GATEWAY_BUILD_TOOLS "Build helper tools" ON)
option(SERIAL_GATEWAY_BUILD_TESTS "Build tests" ON)
option(SERIAL_GATEWAY_ENABLE_MQTT "Enable real MQTT backend via libmosquitto when available" ON)
set(SG_HAVE_MOSQUITTO OFF)

add_library(gateway_core
    src/app/app_controller.cpp
    src/common/logger.cpp
    src/common/mqtt_runtime.cpp
    src/config/config_loader.cpp
    src/device/device_registry.cpp
    src/protocol/crc16.cpp
    src/protocol/frame_parser.cpp
    src/runtime/resource_usage.cpp
    src/runtime/runtime_stats.cpp
    src/serial/serial_port.cpp
    src/ota/firmware_store.cpp
    src/ota/ota_manifest.cpp
    src/ota/ota_metrics.cpp
    src/ota/esp32_ota_adapter.cpp
    src/ota/ota_state_machine.cpp
    src/ota/ota_task.cpp
    src/ota/ota_task_manager.cpp
    src/ota/stm32_ota_adapter.cpp
    src/storage/sqlite_store.cpp
    src/uploader/cached_uploader.cpp
    src/uploader/mqtt_uploader.cpp
    src/uploader/sensor_record_codec.cpp
    src/uploader/tcp_uploader.cpp
    src/uploader/uploader_factory.cpp
    src/storage/history_store.cpp
)

target_include_directories(gateway_core PUBLIC include)

target_compile_options(gateway_core PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(gateway_core PUBLIC pthread)
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
    target_include_directories(gateway_core PUBLIC ${SQLite3_INCLUDE_DIRS})
    target_link_libraries(gateway_core PUBLIC ${SQLite3_LIBRARIES})
else()
    find_library(SQLITE3_LIBRARY sqlite3)
    if(SQLITE3_LIBRARY)
        target_link_libraries(gateway_core PUBLIC ${SQLITE3_LIBRARY})
    else()
        message(FATAL_ERROR "sqlite3 library not found, please install sqlite3 dev package")
    endif()
endif()

if(SERIAL_GATEWAY_ENABLE_MQTT)
    find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h)
    find_library(MOSQUITTO_LIBRARY mosquitto)
    if(MOSQUITTO_INCLUDE_DIR AND MOSQUITTO_LIBRARY)
        set(SG_HAVE_MOSQUITTO ON)
        target_compile_definitions(gateway_core PRIVATE SG_HAVE_MOSQUITTO=1)
        target_include_directories(gateway_core PRIVATE ${MOSQUITTO_INCLUDE_DIR})
        target_link_libraries(gateway_core PRIVATE ${MOSQUITTO_LIBRARY})
        message(STATUS "MQTT backend enabled (libmosquitto found)")
    else()
        message(WARNING "libmosquitto not found, MQTT uploader will run in placeholder mode")
    endif()
endif()

add_executable(serial_gateway src/main.cpp)
target_link_libraries(serial_gateway PRIVATE gateway_core)

install(TARGETS serial_gateway
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES config/gateway.yaml
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/serial-gateway
    RENAME gateway.yaml.example
)

install(FILES systemd/serial-gateway.env.example
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/serial-gateway
)

install(FILES systemd/serial-gateway.service
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/serial-gateway/systemd
)

install(FILES cmake/toolchains/arm-linux-gnueabihf.cmake
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/serial-gateway/toolchains
)

if(SERIAL_GATEWAY_BUILD_TOOLS)
    add_executable(fake_sensor tools/fake_sensor.cpp src/protocol/crc16.cpp)
    target_include_directories(fake_sensor PRIVATE include)
    target_link_libraries(fake_sensor PRIVATE pthread)

    add_executable(tcp_receiver tools/tcp_receiver.cpp)
    target_include_directories(tcp_receiver PRIVATE include)
    target_link_libraries(tcp_receiver PRIVATE pthread)

    add_executable(pty_bridge tools/pty_bridge.cpp)
    target_include_directories(pty_bridge PRIVATE include)
    target_link_libraries(pty_bridge PRIVATE pthread util)

    add_executable(command_sender tools/command_sender.cpp)
    target_include_directories(command_sender PRIVATE include)
    target_link_libraries(command_sender PRIVATE pthread)

    add_executable(mock_wifi_node tools/mock_wifi_node.cpp)
    target_include_directories(mock_wifi_node PRIVATE include)
    target_link_libraries(mock_wifi_node PRIVATE pthread)

    add_executable(mock_serial_node tools/mock_serial_node.cpp src/protocol/crc16.cpp)
    target_include_directories(mock_serial_node PRIVATE include)
    target_link_libraries(mock_serial_node PRIVATE pthread)

    add_executable(otactl tools/otactl.cpp)
    target_include_directories(otactl PRIVATE include)
    target_link_libraries(otactl PRIVATE gateway_core)

    add_executable(fake_bootloader tools/fake_bootloader.cpp src/protocol/crc16.cpp)
    target_include_directories(fake_bootloader PRIVATE include)
    target_link_libraries(fake_bootloader PRIVATE pthread)

    add_executable(stm32_ota_bridge tools/stm32_ota_bridge.cpp)
    target_include_directories(stm32_ota_bridge PRIVATE include)
    target_link_libraries(stm32_ota_bridge PRIVATE pthread)

    add_executable(migrate_ota_store
        tools/migrate_ota_store.cpp
        src/storage/sqlite_store.cpp
        src/ota/ota_task.cpp
    )
    target_include_directories(migrate_ota_store PRIVATE include)
    if(SQLite3_FOUND)
        target_include_directories(migrate_ota_store PRIVATE ${SQLite3_INCLUDE_DIRS})
        target_link_libraries(migrate_ota_store PRIVATE ${SQLite3_LIBRARIES})
    elseif(SQLITE3_LIBRARY)
        target_link_libraries(migrate_ota_store PRIVATE ${SQLITE3_LIBRARY})
    endif()

    if(SG_HAVE_MOSQUITTO)
        add_executable(mqtt_receiver tools/mqtt_receiver.cpp)
        target_include_directories(mqtt_receiver PRIVATE include ${MOSQUITTO_INCLUDE_DIR})
        target_link_libraries(mqtt_receiver PRIVATE pthread ${MOSQUITTO_LIBRARY})

        add_executable(mqtt_command_bridge tools/mqtt_command_bridge.cpp)
        target_include_directories(mqtt_command_bridge PRIVATE include ${MOSQUITTO_INCLUDE_DIR})
        target_link_libraries(mqtt_command_bridge PRIVATE pthread ${MOSQUITTO_LIBRARY})
    endif()
endif()

if(SERIAL_GATEWAY_BUILD_TESTS)
    include(CTest)
    enable_testing()

    add_executable(test_protocol tests/test_protocol.cpp src/protocol/crc16.cpp src/protocol/frame_parser.cpp)
    target_include_directories(test_protocol PRIVATE include)
    add_test(NAME test_protocol COMMAND test_protocol)

    add_executable(test_config tests/test_config.cpp src/config/config_loader.cpp)
    target_include_directories(test_config PRIVATE include)
    add_test(NAME test_config COMMAND test_config)

    add_executable(test_record_codec tests/test_record_codec.cpp src/uploader/sensor_record_codec.cpp)
    target_include_directories(test_record_codec PRIVATE include)
    add_test(NAME test_record_codec COMMAND test_record_codec)

    add_executable(test_cached_uploader tests/test_cached_uploader.cpp src/uploader/cached_uploader.cpp src/uploader/sensor_record_codec.cpp src/common/logger.cpp src/runtime/runtime_stats.cpp)
    target_include_directories(test_cached_uploader PRIVATE include)
    target_link_libraries(test_cached_uploader PRIVATE pthread)
    add_test(NAME test_cached_uploader COMMAND test_cached_uploader)

    add_executable(test_resource_usage tests/test_resource_usage.cpp src/runtime/resource_usage.cpp)
    target_include_directories(test_resource_usage PRIVATE include)
    add_test(NAME test_resource_usage COMMAND test_resource_usage)

    add_executable(test_device_registry tests/test_device_registry.cpp src/device/device_registry.cpp)
    target_include_directories(test_device_registry PRIVATE include)
    add_test(NAME test_device_registry COMMAND test_device_registry)

    add_executable(test_ota_manifest tests/test_ota_manifest.cpp src/ota/ota_manifest.cpp)
    target_include_directories(test_ota_manifest PRIVATE include)
    add_test(NAME test_ota_manifest COMMAND test_ota_manifest)

    add_executable(test_ota_state_machine tests/test_ota_state_machine.cpp src/ota/ota_state_machine.cpp)
    target_include_directories(test_ota_state_machine PRIVATE include)
    add_test(NAME test_ota_state_machine COMMAND test_ota_state_machine)

    add_executable(test_stm32_ota_protocol tests/test_stm32_ota_protocol.cpp src/protocol/crc16.cpp)
    target_include_directories(test_stm32_ota_protocol PRIVATE include)
    add_test(NAME test_stm32_ota_protocol COMMAND test_stm32_ota_protocol)

    add_executable(test_firmware_store tests/test_firmware_store.cpp src/ota/firmware_store.cpp src/ota/ota_manifest.cpp)
    target_include_directories(test_firmware_store PRIVATE include)
    add_test(NAME test_firmware_store COMMAND test_firmware_store)

    add_executable(test_ota_task_manager tests/test_ota_task_manager.cpp src/ota/ota_task_manager.cpp src/ota/ota_state_machine.cpp src/ota/ota_task.cpp src/storage/sqlite_store.cpp)
    target_include_directories(test_ota_task_manager PRIVATE include)
    if(SQLite3_FOUND)
        target_include_directories(test_ota_task_manager PRIVATE ${SQLite3_INCLUDE_DIRS})
        target_link_libraries(test_ota_task_manager PRIVATE ${SQLite3_LIBRARIES})
    elseif(SQLITE3_LIBRARY)
        target_link_libraries(test_ota_task_manager PRIVATE ${SQLITE3_LIBRARY})
    endif()
    add_test(NAME test_ota_task_manager COMMAND test_ota_task_manager)

    add_executable(test_esp32_ota_adapter tests/test_esp32_ota_adapter.cpp src/ota/esp32_ota_adapter.cpp)
    target_include_directories(test_esp32_ota_adapter PRIVATE include)
    add_test(NAME test_esp32_ota_adapter COMMAND test_esp32_ota_adapter)
endif()
