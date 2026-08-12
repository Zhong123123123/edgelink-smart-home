#include "driver_display.h"

#include <stdio.h>
#include "driver_oled_ssd1306.h"

static uint8_t g_oled_available;

int Driver_Display_Init(void)
{
	g_oled_available = (Driver_OLED_Init() == 0) ? 1U : 0U;
	printf("[DSP] oled init %s\r\n", g_oled_available ? "ok" : "fail");
	return 0;
}

int Driver_Display_Show(const DisplayStatus *status)
{
	const char *mode_str;
	const char *wifi_str;
	const char *mqtt_str;
	const char *sensor_str;

	if (status == 0)
	{
		return -1;
	}

	mode_str = (status->mode == 1U) ? "AUTO" : "MAN";
	wifi_str = status->wifi_connected ? "UP" : "DN";
	mqtt_str = status->mqtt_connected ? "UP" : "DN";
	sensor_str = status->sensor_valid ? "OK" : "BAD";

	printf("[DSP] T:%5.1f H:%5.1f L:%4u | LED:%u ALM:%u M:%s W:%s Q:%s S:%s R:%u\r\n",
		status->temperature,
		status->humidity,
		status->light,
		status->led_on,
		status->alarm_on,
		mode_str,
		wifi_str,
		mqtt_str,
		sensor_str,
		status->mqtt_reconnect_fail_count);

	if (g_oled_available && Driver_OLED_ShowStatus(status) != 0)
	{
		g_oled_available = 0U;
		return -1;
	}

	return 0;
}
