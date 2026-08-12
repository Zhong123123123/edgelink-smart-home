#include "app_sensor.h"

#include "app_gateway.h"
#include "app_storage.h"
#include "config.h"
#include "dev_sensor.h"
#include "driver_light.h"
#include "log.h"
#include "smarthome_state.h"
#include "app_watchdog.h"

#define SENSOR_FAIL_TOLERANCE_COUNT 3U
#define SENSOR_LOG_HEARTBEAT_MS 30000U
#define STM32_FLASH_BASE 0x08000000UL
#define STM32_FLASH_END  0x08100000UL

static uint8_t sensor_fnptr_valid(const void *fn)
{
	uint32_t addr = (uint32_t)fn;
	/* ARM Thumb function pointer must have bit0=1 and point to flash text. */
	if ((addr & 0x1UL) == 0U)
	{
		return 0U;
	}
	addr &= ~0x1UL;
	return (addr >= STM32_FLASH_BASE && addr < STM32_FLASH_END) ? 1U : 0U;
}

static void SensorTask(void *parameter)
{
	ptSensorDev sensor_dev = SensorDev_GetDev(SENSOR_DHT11);
	SensorData data = {0};
	SmartHomeRuntimeConfig runtime_cfg;
	SmartHomeState current_state;
	TickType_t now_tick;
	TickType_t last_log_tick = 0;
	uint8_t fail_count = 0;
	uint8_t sensor_valid = 0U;

	(void)parameter;

	if (sensor_dev == 0 || sensor_fnptr_valid((const void *)sensor_dev->Init) == 0U ||
		sensor_fnptr_valid((const void *)sensor_dev->Read) == 0U)
	{
		LOG_ERROR("SENSOR", "sensor dev invalid dev=%p init=%p read=%p",
			(void *)sensor_dev,
			(sensor_dev != 0) ? (void *)sensor_dev->Init : 0,
			(sensor_dev != 0) ? (void *)sensor_dev->Read : 0);
		vTaskDelete(0);
		return;
	}

	if (sensor_dev->Init(sensor_dev) != 0)
	{
		LOG_ERROR("SENSOR", "sensor init failed");
		vTaskDelete(0);
		return;
	}

	LOG_INFO("SENSOR", "sensor task started");

	while (1)
	{
#if ENABLE_WDG_INJECTION
		static uint8_t injected = 0U;
		if (injected == 0U)
		{
			injected = 1U;
			LOG_WARN("SENSOR", "wdg injection enabled, stop kick");
		}
#else
		Watchdog_Kick(WD_TASK_SENSOR);
#endif
		SmartHomeConfig_GetRuntime(&runtime_cfg);
		now_tick = xTaskGetTickCount();

		if (sensor_fnptr_valid((const void *)sensor_dev->Read) == 0U)
		{
			LOG_ERROR("SENSOR", "sensor read fn invalid read=%p", (void *)sensor_dev->Read);
			vTaskDelay(pdMS_TO_TICKS(runtime_cfg.sensor_period_ms));
			continue;
		}

		if (sensor_dev->Read(sensor_dev, &data) == 0)
		{
			uint8_t recovered = (sensor_valid == 0U);
			fail_count = 0;
			sensor_valid = 1U;
			SmartHomeState_SetSensor(data.temperature, data.humidity, data.light, 1U);

			if (recovered)
			{
				LOG_INFO("SENSOR", "sensor recovered");
				(void)Storage_WriteEvent("event=sensor_recovered");
				Gateway_ReportAlarmEvent(4U);
			}
			if (recovered || (now_tick - last_log_tick >= pdMS_TO_TICKS(SENSOR_LOG_HEARTBEAT_MS)))
			{
				LOG_INFO("SENSOR", "sample t=%.1f h=%.1f l=%u", data.temperature, data.humidity, data.light);
				LOG_INFO("SENSOR", "diag stack_hwm=%u period_ms=%u",
					(unsigned int)uxTaskGetStackHighWaterMark(NULL),
					(unsigned int)runtime_cfg.sensor_period_ms);
				last_log_tick = now_tick;
			}
		}
		else
		{
			uint16_t light_fallback = Driver_Light_Read();
			uint8_t reported_valid;
			SmartHomeState_Get(&current_state);

			if (fail_count < 0xFFU)
			{
				fail_count++;
			}
			reported_valid = current_state.sensor_valid;
			if (fail_count >= SENSOR_FAIL_TOLERANCE_COUNT)
			{
				reported_valid = 0U;
			}
			SmartHomeState_SetSensor(
				current_state.temperature,
				current_state.humidity,
				light_fallback,
				reported_valid);

			if (fail_count == 1U)
			{
				LOG_WARN("SENSOR", "read failed");
			}

			if (fail_count >= SENSOR_FAIL_TOLERANCE_COUNT)
			{
				if (sensor_valid != 0U)
				{
					sensor_valid = 0U;
					LOG_WARN("SENSOR", "DHT invalid, light-only fallback active");
					(void)Storage_WriteEvent("event=sensor_invalid");
					Gateway_ReportAlarmEvent(3U);
					last_log_tick = now_tick;
				}
			}
		}

		vTaskDelay(pdMS_TO_TICKS(runtime_cfg.sensor_period_ms));
	}
}

void vStartSensorTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority)
{
	if (xTaskCreate(
			SensorTask,
			"Sensor",
			usTaskStackSize,
			0,
			uxTaskPriority,
			0) == pdPASS)
	{
		LOG_INFO("SENSOR", "create task success");
	}
	else
	{
		LOG_ERROR("SENSOR", "create task failed");
	}
}



