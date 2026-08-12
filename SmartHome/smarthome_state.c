#include "smarthome_state.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

static SmartHomeState g_smart_home_state;

static void SmartHomeState_Lock(void)
{
	taskENTER_CRITICAL();
}

static void SmartHomeState_Unlock(void)
{
	taskEXIT_CRITICAL();
}

void SmartHomeState_Init(void)
{
	SmartHomeState_Lock();
	memset(&g_smart_home_state, 0, sizeof(g_smart_home_state));
	g_smart_home_state.mode = SMARTHOME_MODE_MANUAL;
	SmartHomeState_Unlock();
}

void SmartHomeState_Get(SmartHomeState *out_state)
{
	if (out_state == NULL)
	{
		return;
	}

	SmartHomeState_Lock();
	*out_state = g_smart_home_state;
	SmartHomeState_Unlock();
}

void SmartHomeState_SetSensor(float temperature, float humidity, uint16_t light, uint8_t sensor_valid)
{
	SmartHomeState_Lock();
	g_smart_home_state.temperature = temperature;
	g_smart_home_state.humidity = humidity;
	g_smart_home_state.light = light;
	g_smart_home_state.sensor_valid = sensor_valid;
	SmartHomeState_Unlock();
}

void SmartHomeState_SetLed(uint8_t on)
{
	SmartHomeState_Lock();
	g_smart_home_state.led_on = on ? 1U : 0U;
	SmartHomeState_Unlock();
}

void SmartHomeState_SetBuzzer(uint8_t on)
{
	SmartHomeState_Lock();
	g_smart_home_state.buzzer_on = on ? 1U : 0U;
	SmartHomeState_Unlock();
}

void SmartHomeState_SetManualLed(uint8_t on)
{
	SmartHomeState_Lock();
	g_smart_home_state.manual_led_on = on ? 1U : 0U;
	SmartHomeState_Unlock();
}

void SmartHomeState_SetManualBuzzer(uint8_t on)
{
	SmartHomeState_Lock();
	g_smart_home_state.manual_buzzer_on = on ? 1U : 0U;
	SmartHomeState_Unlock();
}

void SmartHomeState_SetAlarm(uint8_t on)
{
	SmartHomeState_Lock();
	g_smart_home_state.alarm_on = on ? 1U : 0U;
	SmartHomeState_Unlock();
}

void SmartHomeState_SetMode(uint8_t mode)
{
	SmartHomeState_Lock();
	g_smart_home_state.mode = (mode == SMARTHOME_MODE_AUTO) ? SMARTHOME_MODE_AUTO : SMARTHOME_MODE_MANUAL;
	SmartHomeState_Unlock();
}

void SmartHomeState_SetWiFiConnected(uint8_t connected)
{
	SmartHomeState_Lock();
	g_smart_home_state.wifi_connected = connected ? 1U : 0U;
	SmartHomeState_Unlock();
}

void SmartHomeState_SetMQTTConnected(uint8_t connected)
{
	SmartHomeState_Lock();
	g_smart_home_state.mqtt_connected = connected ? 1U : 0U;
	SmartHomeState_Unlock();
}

void SmartHomeState_SetMQTTReconnectFailCount(uint8_t fail_count)
{
	SmartHomeState_Lock();
	g_smart_home_state.mqtt_reconnect_fail_count = fail_count;
	SmartHomeState_Unlock();
}
