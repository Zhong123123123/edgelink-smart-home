#ifndef __CONFIG_H
#define __CONFIG_H

#include <stdint.h>

#define SH_USE_GATEWAY_MODE       1
#define SH_USE_MQTT_MODE          0

/* MQTT static config */
#if defined(__has_include)
#if __has_include("config_private.h")
#include "config_private.h"
#endif
#endif

#ifndef SH_MQTT_BROKER_ADDR
#define SH_MQTT_BROKER_ADDR        "example.com"
#endif
#ifndef SH_MQTT_BROKER_PORT
#define SH_MQTT_BROKER_PORT        1883
#endif
#ifndef SH_MQTT_CLIENT_ID
#define SH_MQTT_CLIENT_ID          "smarthome-client"
#endif
#ifndef SH_MQTT_USERNAME
#define SH_MQTT_USERNAME           "smarthome-user"
#endif
#ifndef SH_MQTT_PASSWORD
#define SH_MQTT_PASSWORD           "change-me"
#endif

#ifndef SH_TOPIC_CMD
#define SH_TOPIC_CMD               "/ipioqFsPt70/MiniBoard/user/getledCmd"
#endif
#ifndef SH_TOPIC_STATUS
#define SH_TOPIC_STATUS            "/ipioqFsPt70/MiniBoard/user/status"
#endif
#ifndef SH_TOPIC_KEY_EVENT
#define SH_TOPIC_KEY_EVENT         "/ipioqFsPt70/MiniBoard/user/keyInfo"
#endif
#ifndef SH_TOPIC_ALARM
#define SH_TOPIC_ALARM             "/ipioqFsPt70/MiniBoard/user/alarm"
#endif

/* WiFi static config */
#ifndef SH_WIFI_SSID
#define SH_WIFI_SSID               "change-me"
#endif
#ifndef SH_WIFI_PASSWORD
#define SH_WIFI_PASSWORD           "change-me"
#endif

/* Runtime default config */
#define SH_DEFAULT_TEMP_THRESHOLD  35.0f
#define SH_DEFAULT_HUMI_THRESHOLD  80.0f

#define SH_SENSOR_PERIOD_MS        2000U
#define SH_DISPLAY_PERIOD_MS       1000U
#define SH_MQTT_REPORT_MS          3000U
#define SH_ALARM_CHECK_MS          500U
#define SH_WIFI_RECONNECT_MS       5000U
#define SH_MQTT_RECONNECT_MS       3000U

#ifndef ENABLE_IWDG
#define ENABLE_IWDG 0
#endif

#ifndef ENABLE_WDG_INJECTION
#define ENABLE_WDG_INJECTION 0
#endif

#ifndef ENABLE_HARDFAULT_INJECTION
#define ENABLE_HARDFAULT_INJECTION 0
#endif

typedef struct
{
	float temperature_high;
	float humidity_high;
	uint16_t sensor_period_ms;
	uint16_t display_period_ms;
	uint16_t mqtt_report_period_ms;
	uint16_t alarm_check_period_ms;
	uint16_t wifi_reconnect_period_ms;
	uint16_t mqtt_reconnect_period_ms;
} SmartHomeRuntimeConfig;

void SmartHomeConfig_Init(void);
void SmartHomeConfig_GetRuntime(SmartHomeRuntimeConfig *out_cfg);
void SmartHomeConfig_SetThreshold(float temperature_high, float humidity_high);

#endif


