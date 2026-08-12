#ifndef __SMARTHOME_STATE_H
#define __SMARTHOME_STATE_H

#include <stdint.h>

typedef enum
{
	SMARTHOME_MODE_MANUAL = 0,
	SMARTHOME_MODE_AUTO = 1
} SmartHomeMode;

typedef struct
{
	float temperature;
	float humidity;
	uint16_t light;
	uint8_t led_on;
	uint8_t buzzer_on;
	uint8_t manual_led_on;
	uint8_t manual_buzzer_on;
	uint8_t alarm_on;
	uint8_t mode;
	uint8_t wifi_connected;
	uint8_t mqtt_connected;
	uint8_t sensor_valid;
	uint8_t mqtt_reconnect_fail_count;
} SmartHomeState;

void SmartHomeState_Init(void);
void SmartHomeState_Get(SmartHomeState *out_state);
void SmartHomeState_SetSensor(float temperature, float humidity, uint16_t light, uint8_t sensor_valid);
void SmartHomeState_SetLed(uint8_t on);
void SmartHomeState_SetBuzzer(uint8_t on);
void SmartHomeState_SetManualLed(uint8_t on);
void SmartHomeState_SetManualBuzzer(uint8_t on);
void SmartHomeState_SetAlarm(uint8_t on);
void SmartHomeState_SetMode(uint8_t mode);
void SmartHomeState_SetWiFiConnected(uint8_t connected);
void SmartHomeState_SetMQTTConnected(uint8_t connected);
void SmartHomeState_SetMQTTReconnectFailCount(uint8_t fail_count);

#endif
