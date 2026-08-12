#ifndef __DRIVER_DISPLAY_H
#define __DRIVER_DISPLAY_H

#include <stdint.h>

typedef struct
{
	float temperature;
	float humidity;
	uint16_t light;
	uint8_t led_on;
	uint8_t alarm_on;
	uint8_t mode;
	uint8_t wifi_connected;
	uint8_t mqtt_connected;
	uint8_t sensor_valid;
	uint8_t mqtt_reconnect_fail_count;
} DisplayStatus;

int Driver_Display_Init(void);
int Driver_Display_Show(const DisplayStatus *status);

#endif
