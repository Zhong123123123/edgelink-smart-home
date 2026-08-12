#pragma once

// Safe repository defaults. Put site-specific values in config_local.h.
#if defined(__has_include)
#  if __has_include("config_local.h")
#    include "config_local.h"
#  endif
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "CHANGE_ME_SSID"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "CHANGE_ME_PASSWORD"
#endif
#ifndef GATEWAY_HOST
#define GATEWAY_HOST "192.168.1.100"
#endif
#ifndef GATEWAY_PORT
#define GATEWAY_PORT 9100
#endif
#ifndef MQTT_HOST
#define MQTT_HOST "192.168.1.100"
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1884
#endif
#ifndef MQTT_CLIENT_ID
#define MQTT_CLIENT_ID "esp32-wifi-node-02"
#endif
#ifndef MQTT_USERNAME
#define MQTT_USERNAME ""
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif
#ifndef GATEWAY_ID
#define GATEWAY_ID "gw001"
#endif

#ifndef COMM_MODE_TCP_JSON
#define COMM_MODE_TCP_JSON 0
#endif
#ifndef COMM_MODE_MQTT
#define COMM_MODE_MQTT 1
#endif
#ifndef ACTIVE_COMM_MODE
#define ACTIVE_COMM_MODE COMM_MODE_MQTT
#endif

// OTA switch: 0=simulate OTA states only, 1=real HTTP download + Update API
#ifndef ESP32_OTA_REAL
#define ESP32_OTA_REAL 0
#endif

// Firmware semantic version used in heartbeat/status
#ifndef FW_VERSION
#define FW_VERSION "1.0.0"
#endif

// Device identity
#ifndef DEVICE_ID
#define DEVICE_ID 2
#endif
#ifndef DEVICE_NAME
#define DEVICE_NAME "wifi-node-02"
#endif

// MQ-2 / LD2402 GPIO integration
#ifndef MQ2_DO_PIN
#define MQ2_DO_PIN 4
#endif
#ifndef LD2402_IO_PIN
#define LD2402_IO_PIN 5
#endif
#ifndef SENSOR_SIM_MODE
#define SENSOR_SIM_MODE 0
#endif
#ifndef MQ2_ACTIVE_LOW
#define MQ2_ACTIVE_LOW 1
#endif
#ifndef LD2402_ACTIVE_HIGH
#define LD2402_ACTIVE_HIGH 1
#endif
#ifndef MQ2_WARMUP_MS
#define MQ2_WARMUP_MS 60000
#endif

// Sensor debug logs over Serial
#ifndef SENSOR_DEBUG_LOG
#define SENSOR_DEBUG_LOG 1
#endif
#ifndef SENSOR_DEBUG_LOG_INTERVAL_MS
#define SENSOR_DEBUG_LOG_INTERVAL_MS 2000
#endif
