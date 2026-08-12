/* Standard includes. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "MQTTClient.h"
#include "main.h"
#include "board_pins.h"
#include "config.h"
#include "dev_io.h"
#include "driver_led_key.h"
#include "log.h"
#include "smarthome_state.h"

extern TaskHandle_t ledTaskHandle;
extern QueueHandle_t xKeyQueue;
void messageArrived(MessageData *data);

static volatile uint8_t g_force_report = 0U;

void Mqtt_RequestImmediateReport(void)
{
	g_force_report = 1U;
}

static int json_get_int(const char *payload, const char *key, int *out_value)
{
	char pattern[40] = {0};
	char *pos;
	char *colon;

	if (payload == 0 || key == 0 || out_value == 0)
	{
		return -1;
	}

	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	pos = strstr((char *)payload, pattern);
	if (pos == 0)
	{
		return -1;
	}

	colon = strchr(pos, ':');
	if (colon == 0)
	{
		return -1;
	}

	if (sscanf(colon + 1, "%d", out_value) == 1)
	{
		return 0;
	}

	return -1;
}

static int json_get_string(const char *payload, const char *key, char *out_buf, uint16_t out_len)
{
	char pattern[40] = {0};
	char *pos;
	char *colon;
	char *start;
	char *end;
	uint16_t len;

	if (payload == 0 || key == 0 || out_buf == 0 || out_len < 2U)
	{
		return -1;
	}

	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	pos = strstr((char *)payload, pattern);
	if (pos == 0)
	{
		return -1;
	}

	colon = strchr(pos, ':');
	if (colon == 0)
	{
		return -1;
	}

	start = colon + 1;
	while (*start == ' ' || *start == '\t')
	{
		start++;
	}
	if (*start != '"')
	{
		return -1;
	}
	start++;

	end = strchr(start, '"');
	if (end == 0)
	{
		return -1;
	}

	len = (uint16_t)(end - start);
	if (len >= out_len)
	{
		len = out_len - 1U;
	}

	memcpy(out_buf, start, len);
	out_buf[len] = '\0';

	return 0;
}

static uint8_t str_ieq(const char *a, const char *b)
{
	char ca;
	char cb;

	if (a == 0 || b == 0)
	{
		return 0U;
	}

	while (*a != '\0' && *b != '\0')
	{
		ca = *a;
		cb = *b;
		if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
		if (ca != cb)
		{
			return 0U;
		}
		a++;
		b++;
	}

	return (*a == '\0' && *b == '\0') ? 1U : 0U;
}

static int json_get_float(const char *payload, const char *key, float *out_value)
{
	char pattern[40] = {0};
	char *pos;
	char *colon;

	if (payload == 0 || key == 0 || out_value == 0)
	{
		return -1;
	}

	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	pos = strstr((char *)payload, pattern);
	if (pos == 0)
	{
		return -1;
	}

	colon = strchr(pos, ':');
	if (colon == 0)
	{
		return -1;
	}

	if (sscanf(colon + 1, "%f", out_value) == 1)
	{
		return 0;
	}

	return -1;
}

static int json_has_cmd(const char *payload, const char *cmd)
{
	char pattern[64] = {0};

	if (payload == 0 || cmd == 0)
	{
		return 0;
	}

	snprintf(pattern, sizeof(pattern), "\"cmd\":\"%s\"", cmd);
	if (strstr(payload, pattern) != 0)
	{
		return 1;
	}

	snprintf(pattern, sizeof(pattern), "\"cmd\" : \"%s\"", cmd);
	if (strstr(payload, pattern) != 0)
	{
		return 1;
	}

	return 0;
}

static int mqtt_publish_text(MQTTClient *client, const char *topic, const char *payload)
{
	MQTTMessage message;

	if (client == 0 || topic == 0 || payload == 0)
	{
		return -1;
	}

	message.qos = 0;
	message.retained = 0;
	message.payload = (void *)payload;
	message.payloadlen = (int)strlen(payload);

	return MQTTPublish(client, topic, &message);
}

static int mqtt_publish_status(MQTTClient *client)
{
	SmartHomeState state;
	char payload[256];
	const char *mode_str;
	const char *log_level_str;

	if (client == 0)
	{
		return -1;
	}

	SmartHomeState_Get(&state);
	mode_str = (state.mode == SMARTHOME_MODE_AUTO) ? "auto" : "manual";
	log_level_str = Log_LevelToString(Log_GetLevel());

	snprintf(payload, sizeof(payload),
		"{\"temperature\":%.1f,\"humidity\":%.1f,\"led_on\":%u,\"alarm_on\":%u,\"mode\":\"%s\",\"wifi_connected\":%u,\"mqtt_connected\":%u,\"mqtt_reconnect_fail_count\":%u,\"log_level\":\"%s\"}",
		state.temperature,
		state.humidity,
		state.led_on,
		state.alarm_on,
		mode_str,
		state.wifi_connected,
		state.mqtt_connected,
		state.mqtt_reconnect_fail_count,
		log_level_str);

	return mqtt_publish_text(client, SH_TOPIC_STATUS, payload);
}

static int mqtt_connect_and_subscribe(MQTTClient *client, Network *network, MQTTPacket_connectData *connectData)
{
	int rc;

	rc = NetworkConnect(network, (char *)SH_MQTT_BROKER_ADDR, SH_MQTT_BROKER_PORT);
	if (rc != 0)
	{
		SmartHomeState_SetWiFiConnected(0U);
		SmartHomeState_SetMQTTConnected(0U);
		LOG_WARN("MQTT", "network connect failed rc=%d", rc);
		return -1;
	}

	SmartHomeState_SetWiFiConnected(1U);

	rc = MQTTConnect(client, connectData);
	if (rc != 0)
	{
		SmartHomeState_SetMQTTConnected(0U);
		LOG_WARN("MQTT", "mqtt connect failed rc=%d", rc);
		return -1;
	}

	rc = MQTTSubscribe(client, SH_TOPIC_CMD, 0, messageArrived);
	if (rc != 0)
	{
		SmartHomeState_SetMQTTConnected(0U);
		LOG_WARN("MQTT", "subscribe failed rc=%d", rc);
		return -1;
	}

	SmartHomeState_SetMQTTConnected(1U);
	LOG_INFO("MQTT", "reconnected and subscribed");
	return 0;
}

static uint32_t mqtt_next_backoff_ms(uint32_t base_ms, uint8_t fail_count)
{
	uint32_t backoff = base_ms;
	uint8_t i;

	if (backoff < 500U)
	{
		backoff = 500U;
	}

	for (i = 0U; i < fail_count && i < 5U; i++)
	{
		backoff <<= 1;
	}

	if (backoff > 30000U)
	{
		backoff = 30000U;
	}

	return backoff;
}

static void handle_mqtt_command(const char *topic, const char *payload)
{
	int value = 0;
	float temp_high = 0.0f;
	float humi_high = 0.0f;
	char log_level_buf[16] = {0};
	uint8_t handled = 0U;

	if (topic == 0 || payload == 0)
	{
		return;
	}

	if (strcmp(topic, SH_TOPIC_CMD) != 0)
	{
		return;
	}

	/* Legacy protocol compatibility. */
	if (strstr(payload, "led on") != 0)
	{
		SmartHomeState_SetManualLed(1U);
		LOG_INFO("MQTT", "legacy cmd: led on");
		g_force_report = 1U;
		return;
	}
	if (strstr(payload, "led off") != 0)
	{
		SmartHomeState_SetManualLed(0U);
		LOG_INFO("MQTT", "legacy cmd: led off");
		g_force_report = 1U;
		return;
	}

	if (json_has_cmd(payload, "set_led"))
	{
		if (json_get_int(payload, "value", &value) == 0)
		{
			SmartHomeState_SetManualLed((uint8_t)(value ? 1 : 0));
			LOG_INFO("MQTT", "set_led=%d", value ? 1 : 0);
			handled = 1U;
		}
	}
	else if (json_has_cmd(payload, "set_buzzer"))
	{
		if (json_get_int(payload, "value", &value) == 0)
		{
			SmartHomeState_SetManualBuzzer((uint8_t)(value ? 1 : 0));
			LOG_INFO("MQTT", "set_buzzer=%d", value ? 1 : 0);
			handled = 1U;
		}
	}
	else if (json_has_cmd(payload, "set_mode"))
	{
		if (json_get_int(payload, "value", &value) == 0)
		{
			SmartHomeState_SetMode((uint8_t)(value ? SMARTHOME_MODE_AUTO : SMARTHOME_MODE_MANUAL));
			LOG_INFO("MQTT", "set_mode=%s", value ? "auto" : "manual");
			handled = 1U;
		}
		else if (strstr(payload, "manual") != 0)
		{
			SmartHomeState_SetMode(SMARTHOME_MODE_MANUAL);
			LOG_INFO("MQTT", "set_mode=manual");
			handled = 1U;
		}
		else if (strstr(payload, "auto") != 0)
		{
			SmartHomeState_SetMode(SMARTHOME_MODE_AUTO);
			LOG_INFO("MQTT", "set_mode=auto");
			handled = 1U;
		}
	}
	else if (json_has_cmd(payload, "set_threshold"))
	{
		if (json_get_float(payload, "temperature_high", &temp_high) != 0)
		{
			json_get_float(payload, "temp", &temp_high);
		}
		if (json_get_float(payload, "humidity_high", &humi_high) != 0)
		{
			json_get_float(payload, "humi", &humi_high);
		}

		if (temp_high > 0.0f && humi_high > 0.0f)
		{
			SmartHomeConfig_SetThreshold(temp_high, humi_high);
			LOG_INFO("MQTT", "set_threshold temp=%.1f humi=%.1f", temp_high, humi_high);
			handled = 1U;
		}
	}
	else if (json_has_cmd(payload, "get_status"))
	{
		LOG_INFO("MQTT", "get_status requested");
		handled = 1U;
	}
	else if (json_has_cmd(payload, "set_log_level"))
	{
		if (json_get_int(payload, "value", &value) == 0 || json_get_int(payload, "level", &value) == 0)
		{
			if (value < (int)LOG_LEVEL_ERROR) value = (int)LOG_LEVEL_ERROR;
			if (value > (int)LOG_LEVEL_INFO) value = (int)LOG_LEVEL_INFO;
			Log_SetLevel((LogLevel)value);
			LOG_PRINT("INFO", "MQTT", "log level set to %s", Log_LevelToString(Log_GetLevel()));
			handled = 1U;
		}
		else if (json_get_string(payload, "value", log_level_buf, (uint16_t)sizeof(log_level_buf)) == 0 ||
				 json_get_string(payload, "level", log_level_buf, (uint16_t)sizeof(log_level_buf)) == 0)
		{
			if (str_ieq(log_level_buf, "error"))
			{
				Log_SetLevel(LOG_LEVEL_ERROR);
				handled = 1U;
			}
			else if (str_ieq(log_level_buf, "warn") || str_ieq(log_level_buf, "warning"))
			{
				Log_SetLevel(LOG_LEVEL_WARN);
				handled = 1U;
			}
			else if (str_ieq(log_level_buf, "info"))
			{
				Log_SetLevel(LOG_LEVEL_INFO);
				handled = 1U;
			}

			if (handled)
			{
				LOG_PRINT("INFO", "MQTT", "log level set to %s", Log_LevelToString(Log_GetLevel()));
			}
		}
	}

	if (handled)
	{
		g_force_report = 1U;
	}
	else
	{
		LOG_WARN("MQTT", "cmd parse failed payload=%.80s", payload);
	}
}

void messageArrived(MessageData *data)
{
	char topic_buf[128] = {0};
	char payload_buf[256] = {0};
	char cmd_buf[32] = {0};
	int topic_len;
	int payload_len;

	topic_len = data->topicName->lenstring.len;
	if (topic_len >= (int)sizeof(topic_buf))
	{
		topic_len = (int)sizeof(topic_buf) - 1;
	}
	memcpy(topic_buf, data->topicName->lenstring.data, (size_t)topic_len);
	topic_buf[topic_len] = '\0';

	payload_len = data->message->payloadlen;
	if (payload_len >= (int)sizeof(payload_buf))
	{
		payload_len = (int)sizeof(payload_buf) - 1;
	}
	memcpy(payload_buf, data->message->payload, (size_t)payload_len);
	payload_buf[payload_len] = '\0';

	if (json_get_string(payload_buf, "cmd", cmd_buf, (uint16_t)sizeof(cmd_buf)) == 0)
	{
		LOG_INFO("MQTT", "rx topic=%s cmd=%s", topic_buf, cmd_buf);
	}
	else if (strstr(payload_buf, "led on") != 0 || strstr(payload_buf, "led off") != 0)
	{
		LOG_INFO("MQTT", "rx topic=%s legacy_cmd", topic_buf);
	}
	else
	{
		LOG_INFO("MQTT", "rx topic=%s len=%d", topic_buf, payload_len);
	}
	handle_mqtt_command(topic_buf, payload_buf);
}

static void prvMQTTEchoTask(void *pvParameters)
{
	KeyEvent key = {0};
	MQTTClient client;
	Network network;
	unsigned char sendbuf[512], readbuf[512];
	int rc = 0;
	MQTTPacket_connectData connectData = MQTTPacket_connectData_initializer;
	SmartHomeRuntimeConfig runtime_cfg;
	SmartHomeState state_snapshot;
	TickType_t last_report_tick;
	TickType_t last_reconnect_try_tick;
	TickType_t last_wifi_reinit_tick;
	uint8_t last_alarm_state = 0U;
	uint8_t reconnect_fail_count = 0U;

	pvParameters = 0;
	SmartHomeState_SetWiFiConnected(0U);
	SmartHomeState_SetMQTTConnected(0U);
	SmartHomeState_SetMQTTReconnectFailCount(0U);

	NetworkInit(&network);
	MQTTClientInit(&client, &network, 30000, sendbuf, sizeof(sendbuf), readbuf, sizeof(readbuf));

#if defined(MQTT_TASK)
	if ((rc = MQTTStartTask(&client)) != pdPASS)
	{
		LOG_ERROR("MQTT", "start mqtt task failed rc=%d", rc);
	}
#endif

	connectData.MQTTVersion = 3;
	connectData.clientID.cstring = (char *)SH_MQTT_CLIENT_ID;
	connectData.username.cstring = (char *)SH_MQTT_USERNAME;
	connectData.password.cstring = (char *)SH_MQTT_PASSWORD;

	rc = mqtt_connect_and_subscribe(&client, &network, &connectData);
	if (rc != 0)
	{
		LOG_WARN("MQTT", "initial connect failed, waiting for retry");
	}

	last_report_tick = xTaskGetTickCount();
	last_reconnect_try_tick = xTaskGetTickCount();
	last_wifi_reinit_tick = xTaskGetTickCount();
	g_force_report = 1U;
	SmartHomeState_Get(&state_snapshot);
	last_alarm_state = state_snapshot.alarm_on;

	while (1)
	{
		SmartHomeConfig_GetRuntime(&runtime_cfg);

		if (xKeyQueue != NULL && xQueueReceive(xKeyQueue, (uint8_t *)&key, 10) == pdPASS)
		{
			char payload[96] = {0};
			snprintf(payload, sizeof(payload), "{\"event\":\"key\",\"num\":%u,\"press_ms\":%u}", key.num, key.time);
			if ((rc = mqtt_publish_text(&client, SH_TOPIC_KEY_EVENT, payload)) != 0)
			{
				LOG_WARN("MQTT", "key event publish failed rc=%d", rc);
			}
		}

		if (g_force_report ||
			(xTaskGetTickCount() - last_report_tick >= pdMS_TO_TICKS(runtime_cfg.mqtt_report_period_ms)))
		{
			rc = mqtt_publish_status(&client);
			if (rc != 0)
			{
				LOG_WARN("MQTT", "status publish failed rc=%d", rc);
			}
			last_report_tick = xTaskGetTickCount();
			g_force_report = 0U;
		}

		SmartHomeState_Get(&state_snapshot);
		if (state_snapshot.alarm_on != last_alarm_state)
		{
			char alarm_payload[96] = {0};
			snprintf(alarm_payload, sizeof(alarm_payload),
				"{\"alarm_on\":%u,\"temperature\":%.1f,\"humidity\":%.1f}",
				state_snapshot.alarm_on,
				state_snapshot.temperature,
				state_snapshot.humidity);
			rc = mqtt_publish_text(&client, SH_TOPIC_ALARM, alarm_payload);
			if (rc != 0)
			{
				LOG_WARN("MQTT", "alarm publish failed rc=%d", rc);
			}
			last_alarm_state = state_snapshot.alarm_on;
		}

		SmartHomeState_Get(&state_snapshot);
#if !defined(MQTT_TASK)
		if (state_snapshot.mqtt_connected)
		{
			rc = MQTTYield(&client, 100);
			if (rc != 0)
			{
				SmartHomeState_SetMQTTConnected(0U);
				LOG_WARN("MQTT", "yield rc=%d", rc);
			}
		}
#endif
		SmartHomeState_Get(&state_snapshot);
		if (!state_snapshot.mqtt_connected &&
			(xTaskGetTickCount() - last_reconnect_try_tick >= pdMS_TO_TICKS(mqtt_next_backoff_ms(runtime_cfg.mqtt_reconnect_period_ms, reconnect_fail_count))))
		{
			TickType_t now_tick = xTaskGetTickCount();

			last_reconnect_try_tick = xTaskGetTickCount();
			if (now_tick - last_wifi_reinit_tick >= pdMS_TO_TICKS(runtime_cfg.wifi_reconnect_period_ms))
			{
				last_wifi_reinit_tick = now_tick;
				LOG_WARN("MQTT", "wifi reinit before reconnect");
				NetworkInit(&network);
			}

				if (mqtt_connect_and_subscribe(&client, &network, &connectData) == 0)
				{
					reconnect_fail_count = 0U;
					SmartHomeState_SetMQTTReconnectFailCount(reconnect_fail_count);
					g_force_report = 1U;
				}
				else if (reconnect_fail_count < 0xFFU)
				{
					reconnect_fail_count++;
					SmartHomeState_SetMQTTReconnectFailCount(reconnect_fail_count);
				}
			}
			else if (state_snapshot.mqtt_connected)
			{
				reconnect_fail_count = 0U;
				SmartHomeState_SetMQTTReconnectFailCount(reconnect_fail_count);
			}
		vTaskDelay(1);
	}
}

void vStartMQTTTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority)
{
	BaseType_t x = 0L;

	if (xTaskCreate(
			prvMQTTEchoTask,
			"MQTTEcho0",
			usTaskStackSize,
			(void *)x,
			uxTaskPriority,
			NULL) == pdPASS)
	{
		LOG_INFO("MQTT", "create task success");
	}
	else
	{
		LOG_ERROR("MQTT", "create task failed");
	}
}

