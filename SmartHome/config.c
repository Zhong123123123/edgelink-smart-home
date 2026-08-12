#include "config.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

static SmartHomeRuntimeConfig g_runtime_cfg;

static void SmartHomeConfig_Lock(void)
{
	taskENTER_CRITICAL();
}

static void SmartHomeConfig_Unlock(void)
{
	taskEXIT_CRITICAL();
}

void SmartHomeConfig_Init(void)
{
	SmartHomeConfig_Lock();
	memset(&g_runtime_cfg, 0, sizeof(g_runtime_cfg));
	g_runtime_cfg.temperature_high = SH_DEFAULT_TEMP_THRESHOLD;
	g_runtime_cfg.humidity_high = SH_DEFAULT_HUMI_THRESHOLD;
	g_runtime_cfg.sensor_period_ms = SH_SENSOR_PERIOD_MS;
	g_runtime_cfg.display_period_ms = SH_DISPLAY_PERIOD_MS;
	g_runtime_cfg.mqtt_report_period_ms = SH_MQTT_REPORT_MS;
	g_runtime_cfg.alarm_check_period_ms = SH_ALARM_CHECK_MS;
	g_runtime_cfg.wifi_reconnect_period_ms = SH_WIFI_RECONNECT_MS;
	g_runtime_cfg.mqtt_reconnect_period_ms = SH_MQTT_RECONNECT_MS;
	SmartHomeConfig_Unlock();
}

void SmartHomeConfig_GetRuntime(SmartHomeRuntimeConfig *out_cfg)
{
	if (out_cfg == NULL)
	{
		return;
	}

	SmartHomeConfig_Lock();
	*out_cfg = g_runtime_cfg;
	SmartHomeConfig_Unlock();
}

void SmartHomeConfig_SetThreshold(float temperature_high, float humidity_high)
{
	SmartHomeConfig_Lock();
	g_runtime_cfg.temperature_high = temperature_high;
	g_runtime_cfg.humidity_high = humidity_high;
	SmartHomeConfig_Unlock();
}
