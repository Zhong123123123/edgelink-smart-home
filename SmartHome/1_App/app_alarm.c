#include "app_alarm.h"

#include "app_gateway.h"
#include "app_storage.h"
#include "config.h"
#include "log.h"
#include "smarthome_state.h"
#include "app_watchdog.h"
#include "driver_led_key.h"

#include <stdio.h>

extern TaskHandle_t ledTaskHandle;
#define ALARM_DIAG_HEARTBEAT_MS 30000U

static void alarm_set_buzzer_output(uint8_t on)
{
	static uint8_t inited = 0U;
	if (inited == 0U)
	{
		(void)Driver_Beep_Init();
		inited = 1U;
	}
	(void)Driver_Beep_WriteStatus((uint8_t)(on ? 1U : 0U));
}

static void AlarmTask(void *parameter)
{
	SmartHomeRuntimeConfig runtime_cfg;
	SmartHomeState state;
	uint8_t prev_alarm = 0U;
	uint8_t alarm_now;
	uint8_t desired_led;
	uint8_t desired_buzzer;
	TickType_t last_diag_tick = 0;

	(void)parameter;

	LOG_INFO("ALARM", "alarm task started");
	last_diag_tick = xTaskGetTickCount();

	while (1)
	{
		Watchdog_Kick(WD_TASK_ALARM);
		SmartHomeConfig_GetRuntime(&runtime_cfg);
		SmartHomeState_Get(&state);

		alarm_now = 0U;
		if (state.sensor_valid == 0U)
		{
			alarm_now = 1U;
		}
		if (state.temperature >= runtime_cfg.temperature_high)
		{
			alarm_now = 1U;
		}
		if (state.humidity >= runtime_cfg.humidity_high)
		{
			alarm_now = 1U;
		}

		SmartHomeState_SetAlarm(alarm_now);

		if (state.mode == SMARTHOME_MODE_AUTO)
		{
			desired_led = alarm_now;
			desired_buzzer = alarm_now;
		}
		else
		{
			desired_led = state.manual_led_on;
			desired_buzzer = state.manual_buzzer_on;
		}

		if (state.led_on != desired_led)
		{
			SmartHomeState_SetLed(desired_led);
			xTaskNotify(ledTaskHandle, desired_led, eSetValueWithOverwrite);
		}
		if (state.buzzer_on != desired_buzzer)
		{
			SmartHomeState_SetBuzzer(desired_buzzer);
			alarm_set_buzzer_output(desired_buzzer);
		}

		if (alarm_now != prev_alarm)
		{
			char event[96];
			LOG_WARN("ALARM", "alarm=%u mode=%s t=%.1f h=%.1f valid=%u",
				alarm_now,
				(state.mode == SMARTHOME_MODE_AUTO) ? "auto" : "manual",
				state.temperature,
				state.humidity,
				state.sensor_valid);
			(void)snprintf(event, sizeof(event),
				"event=%s mode=%s t=%.1f h=%.1f valid=%u",
				(alarm_now != 0U) ? "alarm_enter" : "alarm_recover",
				(state.mode == SMARTHOME_MODE_AUTO) ? "auto" : "manual",
				state.temperature,
				state.humidity,
				state.sensor_valid);
			(void)Storage_WriteEvent(event);
			Gateway_ReportAlarmEvent((alarm_now != 0U) ? 1U : 2U);
			Gateway_RequestImmediateReport();
			prev_alarm = alarm_now;
		}

		if (xTaskGetTickCount() - last_diag_tick >= pdMS_TO_TICKS(ALARM_DIAG_HEARTBEAT_MS))
		{
			LOG_INFO("ALARM", "diag stack_hwm=%u period_ms=%u",
				(unsigned int)uxTaskGetStackHighWaterMark(NULL),
				(unsigned int)runtime_cfg.alarm_check_period_ms);
			last_diag_tick = xTaskGetTickCount();
		}

		vTaskDelay(pdMS_TO_TICKS(runtime_cfg.alarm_check_period_ms));
	}
}

void vStartAlarmTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority)
{
	if (xTaskCreate(
			AlarmTask,
			"Alarm",
			usTaskStackSize,
			0,
			uxTaskPriority,
			0) == pdPASS)
	{
		LOG_INFO("ALARM", "create task success");
	}
	else
	{
		LOG_ERROR("ALARM", "create task failed");
	}
}



