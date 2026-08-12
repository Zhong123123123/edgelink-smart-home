#include "app_display.h"

#include "config.h"
#include "dev_display.h"
#include "log.h"
#include "smarthome_state.h"
#include "app_watchdog.h"

#define DISPLAY_HEARTBEAT_MS 5000U
#define DISPLAY_DIAG_HEARTBEAT_MS 30000U

static float absf(float v)
{
	return (v < 0.0f) ? -v : v;
}

static uint8_t display_need_refresh(const DisplayStatus *prev, const DisplayStatus *curr)
{
	if (prev == 0 || curr == 0)
	{
		return 1U;
	}

	if (curr->led_on != prev->led_on ||
		curr->alarm_on != prev->alarm_on ||
		curr->mode != prev->mode ||
		curr->wifi_connected != prev->wifi_connected ||
		curr->mqtt_connected != prev->mqtt_connected ||
		curr->sensor_valid != prev->sensor_valid ||
		curr->mqtt_reconnect_fail_count != prev->mqtt_reconnect_fail_count)
	{
		return 1U;
	}

	if (absf(curr->temperature - prev->temperature) >= 0.5f ||
		absf(curr->humidity - prev->humidity) >= 1.0f)
	{
		return 1U;
	}

	if ((curr->light > prev->light ? (curr->light - prev->light) : (prev->light - curr->light)) >= 20U)
	{
		return 1U;
	}

	return 0U;
}

static void DisplayTask(void *parameter)
{
	ptDisplayDev display_dev = DisplayDev_GetDev(DISPLAY_UART_SIM);
	SmartHomeRuntimeConfig runtime_cfg;
	SmartHomeState current_state;
	DisplayStatus display_status;
	DisplayStatus prev_status = {0};
	uint8_t has_prev = 0U;
	TickType_t last_show_tick = 0;
	TickType_t last_diag_tick = 0;
	TickType_t now_tick;

	(void)parameter;

	if (display_dev == 0 || display_dev->Init(display_dev) != 0)
	{
		LOG_ERROR("DISPLAY", "display init failed");
		vTaskDelete(0);
		return;
	}

	LOG_INFO("DISPLAY", "display task started");
	last_diag_tick = xTaskGetTickCount();

	while (1)
	{
		Watchdog_Kick(WD_TASK_DISPLAY);
		SmartHomeConfig_GetRuntime(&runtime_cfg);
		SmartHomeState_Get(&current_state);
		now_tick = xTaskGetTickCount();

		display_status.temperature = current_state.temperature;
		display_status.humidity = current_state.humidity;
		display_status.light = current_state.light;
		display_status.led_on = current_state.led_on;
		display_status.alarm_on = current_state.alarm_on;
		display_status.mode = current_state.mode;
		display_status.wifi_connected = current_state.wifi_connected;
		display_status.mqtt_connected = current_state.mqtt_connected;
		display_status.sensor_valid = current_state.sensor_valid;
		display_status.mqtt_reconnect_fail_count = current_state.mqtt_reconnect_fail_count;

		if (!has_prev ||
			display_need_refresh(&prev_status, &display_status) ||
			(now_tick - last_show_tick >= pdMS_TO_TICKS(DISPLAY_HEARTBEAT_MS)))
		{
			if (display_dev->Show(display_dev, &display_status) != 0)
			{
				LOG_WARN("DISPLAY", "display show failed");
			}
			prev_status = display_status;
			has_prev = 1U;
			last_show_tick = now_tick;
		}

		if (now_tick - last_diag_tick >= pdMS_TO_TICKS(DISPLAY_DIAG_HEARTBEAT_MS))
		{
			LOG_INFO("DISPLAY", "diag stack_hwm=%u period_ms=%u",
				(unsigned int)uxTaskGetStackHighWaterMark(NULL),
				(unsigned int)runtime_cfg.display_period_ms);
			last_diag_tick = now_tick;
		}

		vTaskDelay(pdMS_TO_TICKS(runtime_cfg.display_period_ms));
	}
}

void vStartDisplayTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority)
{
	if (xTaskCreate(
			DisplayTask,
			"Display",
			usTaskStackSize,
			0,
			uxTaskPriority,
			0) == pdPASS)
	{
		LOG_INFO("DISPLAY", "create task success");
	}
	else
	{
		LOG_ERROR("DISPLAY", "create task failed");
	}
}



