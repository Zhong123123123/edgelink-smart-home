cmake_minimum_required(VERSION 3.16)
project(gateway_qt_monitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()

qt_add_executable(gateway_qt_monitor
    src/main.cpp
    src/GatewayApiClient.cpp
)

target_include_directories(gateway_qt_monitor PRIVATE src)

target_link_libraries(gateway_qt_monitor
    PRIVATE
    Qt6::Widgets
    Qt6::Network
)

install(TARGETS gateway_qt_monitor RUNTIME DESTINATION bin)
