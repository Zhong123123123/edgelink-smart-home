#pragma once

// Copy this file to `config_local.h` for site-specific credentials and host overrides.
// The repository default keeps only safe placeholders.

#define WIFI_SSID "CHANGE_ME"
#define WIFI_PASSWORD "CHANGE_ME"
#define GATEWAY_HOST "192.168.1.100"
#define GATEWAY_TCP_BINARY_PORT 9101

#define UART_RX_PIN 16
#define UART_TX_PIN 17
#define UART_BAUD 115200

#define TCP_RECONNECT_MS 1000
#define WIFI_RECONNECT_MS 2000
#define BRIDGE_HEARTBEAT_MS 3000
#define FRAME_QUEUE_DEPTH 32
#define MAX_FRAME_SIZE 256
#define LOG_LEVEL 2
#define BRIDGE_ADAPTER_ID 1   // 设备1写1，设备2写2

#if defined(__has_include)
#  if __has_include("config_local.h")
#    include "config_local.h"
#  endif
#endif
