#include <HTTPClient.h>
#include <PubSubClient.h>
#include <Update.h>
#include <WiFi.h>
#if __has_include("config.h")
#include "config.h"
#endif

// =========================
// Basic configuration
// =========================
#ifndef WIFI_SSID
#define WIFI_SSID "CHANGE_ME_SSID"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "CHANGE_ME_PASSWORD"
#endif
#ifndef GATEWAY_HOST
#define GATEWAY_HOST "192.168.1.100"
#endif
#ifndef GATEWAY_PORT
#define GATEWAY_PORT 9100
#endif
#ifndef MQTT_HOST
#define MQTT_HOST "192.168.1.100"
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef MQTT_CLIENT_ID
#define MQTT_CLIENT_ID "esp32-wifi-node-02"
#endif
#ifndef MQTT_USERNAME
#define MQTT_USERNAME ""
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif
#ifndef GATEWAY_ID
#define GATEWAY_ID "gw001"
#endif
#ifndef COMM_MODE_TCP_JSON
#define COMM_MODE_TCP_JSON 0
#endif
#ifndef COMM_MODE_MQTT
#define COMM_MODE_MQTT 1
#endif
#ifndef ACTIVE_COMM_MODE
#define ACTIVE_COMM_MODE COMM_MODE_MQTT
#endif
#ifndef ESP32_OTA_REAL
#define ESP32_OTA_REAL 0
#endif
#ifndef FW_VERSION
#define FW_VERSION "1.0.0"
#endif
#ifndef DEVICE_ID
#define DEVICE_ID 2
#endif
#ifndef DEVICE_NAME
#define DEVICE_NAME "wifi-node-02"
#endif

const int RGB_PIN = 48;

#ifndef MQ2_DO_PIN
#define MQ2_DO_PIN 4
#endif

#ifndef LD2402_IO_PIN
#define LD2402_IO_PIN 5
#endif

#ifndef SENSOR_SIM_MODE
#define SENSOR_SIM_MODE 0  // 0=read real GPIO, 1=simulate mq2/ld2402
#endif

#ifndef MQ2_ACTIVE_LOW
#define MQ2_ACTIVE_LOW 1
#endif

#ifndef LD2402_ACTIVE_HIGH
#define LD2402_ACTIVE_HIGH 1
#endif

#ifndef MQ2_WARMUP_MS
#define MQ2_WARMUP_MS 60000
#endif

#ifndef SENSOR_DEBUG_LOG
#define SENSOR_DEBUG_LOG 1  // 1=print raw gpio and parsed sensor states
#endif

#ifndef SENSOR_DEBUG_LOG_INTERVAL_MS
#define SENSOR_DEBUG_LOG_INTERVAL_MS 2000
#endif

// =========================
// Timing configuration
// =========================
const uint32_t WIFI_RETRY_INTERVAL_MS = 3000;
const uint32_t GATEWAY_RETRY_INTERVAL_MS = 1000;
const uint32_t MQTT_RETRY_INTERVAL_MS = 3000;
const uint32_t MQTT_CONNECT_TIMEOUT_MS = 800;
const uint16_t MQTT_SOCKET_TIMEOUT_S = 5;
const uint32_t SENSOR_SAMPLE_INTERVAL_MS = 1000;
const uint32_t HEARTBEAT_INTERVAL_MS = 10000;
const uint32_t MIN_REPORT_INTERVAL_MS = 200;

// =========================
// Global objects and shared state
// =========================
WiFiClient tcpClient;
WiFiClient mqttNetClient;
PubSubClient mqttClient(mqttNetClient);

SemaphoreHandle_t clientMutex = NULL;
SemaphoreHandle_t stateMutex = NULL;
SemaphoreHandle_t sensorMutex = NULL;

struct SensorData_t {
  float temperature;
  float humidity;
  float voltage;
  int mq2_alarm;
  int ld2402_presence;
};

SensorData_t sensorData = {
  26.0f,
  60.0f,
  3.3f,
  0,
  0
};

uint32_t sensorBootMs = 0;

uint32_t seq_id = 1000;
uint32_t report_interval_ms = 3000;
int led_state = 0;
int rgb_override_mode = 0;  // 0=auto, 1=force green, 2=force off
String fw_version = FW_VERSION;

struct OtaRuntime_t {
  bool in_progress;
  uint32_t command_id;
  String version_from;
  String version_to;
  String firmware_url;
  uint32_t size;
  String crc32;
};

OtaRuntime_t otaState = {false, 0, FW_VERSION, FW_VERSION, "", 0, ""};

String topicTelemetry;
String topicStatus;
String topicEvent;
String topicCommandDown;
String topicCommandAck;
String topicOtaStart;
String topicOtaStatus;
uint32_t mqttLastConnectAttemptMs = 0;
bool mqttOnlinePublished = false;
uint32_t mqttLastLoopLogMs = 0;

enum TopicKind {
  TOPIC_TELEMETRY,
  TOPIC_STATUS,
  TOPIC_EVENT,
  TOPIC_COMMAND_ACK,
  TOPIC_OTA_STATUS
};

void processCommand(const String& line, const String& sourceTopic = "");

// =========================
// Small shared-state helpers
// =========================
uint32_t nextSeq() {
  uint32_t seq;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  seq = seq_id++;
  xSemaphoreGive(stateMutex);
  return seq;
}

uint32_t getReportIntervalMs() {
  uint32_t interval;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  interval = report_interval_ms;
  xSemaphoreGive(stateMutex);
  return interval;
}

void setReportIntervalMs(uint32_t interval) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  report_interval_ms = interval;
  xSemaphoreGive(stateMutex);
}

int getRgbOverrideMode() {
  int mode;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  mode = rgb_override_mode;
  xSemaphoreGive(stateMutex);
  return mode;
}

void setRgbOverrideMode(int mode) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  rgb_override_mode = mode;
  led_state = (mode == 1) ? 1 : 0;
  xSemaphoreGive(stateMutex);
}

void setLedState(int value) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  led_state = value ? 1 : 0;
  rgb_override_mode = led_state ? 1 : 2;
  xSemaphoreGive(stateMutex);
}

SensorData_t getSensorDataCopy() {
  SensorData_t copy;
  xSemaphoreTake(sensorMutex, portMAX_DELAY);
  copy = sensorData;
  xSemaphoreGive(sensorMutex);
  return copy;
}

void updateSensorData(const SensorData_t& data) {
  xSemaphoreTake(sensorMutex, portMAX_DELAY);
  sensorData = data;
  xSemaphoreGive(sensorMutex);
}

bool isGatewayConnected() {
  bool ok;
  xSemaphoreTakeRecursive(clientMutex, portMAX_DELAY);
  ok = tcpClient.connected();
  xSemaphoreGiveRecursive(clientMutex);
  return ok;
}

bool isMqttConnected() {
  bool ok;
  xSemaphoreTakeRecursive(clientMutex, portMAX_DELAY);
  ok = mqttClient.connected();
  xSemaphoreGiveRecursive(clientMutex);
  return ok;
}

bool isTransportConnected() {
#if ACTIVE_COMM_MODE == COMM_MODE_MQTT
  return isMqttConnected();
#else
  return isGatewayConnected();
#endif
}

const char* activeCommModeName() {
#if ACTIVE_COMM_MODE == COMM_MODE_MQTT
  return "MQTT";
#else
  return "TCP_JSON";
#endif
}

String makeTopic(const char* suffix) {
  String topic = "gateway/";
  topic += GATEWAY_ID;
  topic += "/device/";
  topic += String(DEVICE_ID);
  topic += "/";
  topic += suffix;
  return topic;
}

void initTopics() {
  topicTelemetry = makeTopic("telemetry");
  topicStatus = makeTopic("status");
  topicEvent = makeTopic("event");
  topicCommandDown = makeTopic("command/down");
  topicCommandAck = makeTopic("command/ack");
  topicOtaStart = makeTopic("ota/start");
  topicOtaStatus = makeTopic("ota/status");
}

// =========================
// RGB LED helpers
// =========================
void setRgbColor(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(RGB_PIN, r, g, b);
}

// =========================
// TCP send helper
// =========================
bool sendLine(const String& line) {
  bool ok = false;

  xSemaphoreTakeRecursive(clientMutex, portMAX_DELAY);
  if (tcpClient.connected()) {
    tcpClient.print(line);
    tcpClient.print('\n');
    ok = true;
  }
  xSemaphoreGiveRecursive(clientMutex);

  if (ok) {
    Serial.print("[TX] ");
    Serial.println(line);
  } else {
    Serial.print("[TX-DROP] gateway not connected: ");
    Serial.println(line);
  }

  return ok;
}

const String& topicForKind(TopicKind kind) {
  switch (kind) {
    case TOPIC_TELEMETRY:
      return topicTelemetry;
    case TOPIC_STATUS:
      return topicStatus;
    case TOPIC_EVENT:
      return topicEvent;
    case TOPIC_COMMAND_ACK:
      return topicCommandAck;
    case TOPIC_OTA_STATUS:
      return topicOtaStatus;
    default:
      return topicCommandAck;
  }
}

bool publishMqttMessage(TopicKind kind, const String& payload, bool retained = false) {
  bool ok = false;
  const String& topic = topicForKind(kind);

  xSemaphoreTakeRecursive(clientMutex, portMAX_DELAY);
  if (mqttClient.connected()) {
    ok = mqttClient.publish(topic.c_str(), payload.c_str(), retained);
  }
  xSemaphoreGiveRecursive(clientMutex);

  if (ok) {
    Serial.print("[MQTT-TX] ");
    Serial.print(topic);
    Serial.print(" => ");
    Serial.println(payload);
  } else {
    Serial.print("[MQTT-TX-FAIL] ");
    Serial.print(topic);
    Serial.print(" => ");
    Serial.println(payload);
  }

  return ok;
}

bool publishMessage(TopicKind kind, const String& payload, bool retained = false) {
#if ACTIVE_COMM_MODE == COMM_MODE_MQTT
  return publishMqttMessage(kind, payload, retained);
#else
  (void)kind;
  (void)retained;
  return sendLine(payload);
#endif
}

String canonicalCommandName(const String& raw) {
  if (raw == "status_get") {
    return "get_status";
  }
  if (raw == "led_set") {
    return "set_led";
  }
  if (raw == "report_interval_set") {
    return "set_report_interval";
  }
  return raw;
}

String publicCommandName(const String& canonical) {
  if (canonical == "get_status") {
    return "status_get";
  }
  if (canonical == "set_led") {
    return "led_set";
  }
  if (canonical == "set_report_interval") {
    return "report_interval_set";
  }
  return canonical;
}

String buildPresencePayload(const char* status) {
  String msg = "{";
  msg += "\"gateway_id\":\"" + String(GATEWAY_ID) + "\",";
  msg += "\"device_id\":" + String(DEVICE_ID) + ",";
  msg += "\"device_name\":\"" + String(DEVICE_NAME) + "\",";
  msg += "\"msg_type\":\"presence\",";
  msg += "\"status\":\"" + String(status) + "\"";
  msg += "}";
  return msg;
}

void sendOtaStatus(const char* ota_state, int progress, int error_code) {
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  uint32_t seq = nextSeq();

  String msg = "{";
  msg += "\"gateway_id\":\"" + String(GATEWAY_ID) + "\",";
  msg += "\"device_id\":" + String(DEVICE_ID) + ",";
  msg += "\"device_name\":\"" + String(DEVICE_NAME) + "\",";
  msg += "\"msg_type\":\"ota_status\",";
  msg += "\"event_type\":\"ota_status\",";
  msg += "\"cmd\":\"ota_start\",";
  msg += "\"timestamp\":0,";
  msg += "\"uptime_ms\":" + String(millis()) + ",";
  msg += "\"command_id\":" + String(otaState.command_id) + ",";
  msg += "\"ota_state\":\"" + String(ota_state) + "\",";
  msg += "\"progress\":" + String(progress) + ",";
  msg += "\"version_from\":\"" + otaState.version_from + "\",";
  msg += "\"version_to\":\"" + otaState.version_to + "\",";
  msg += "\"error_code\":" + String(error_code) + ",";
  msg += "\"firmware_version\":\"" + fw_version + "\",";
  msg += "\"link_type\":\"wifi\",";
  msg += "\"wifi_rssi\":" + String(rssi) + ",";
  msg += "\"seq\":" + String(seq);
  msg += "}";
  publishMessage(TOPIC_OTA_STATUS, msg);
  publishMessage(TOPIC_EVENT, msg);
}

// =========================
// JSON message builders
// =========================
void sendSensorData() {
  SensorData_t data = getSensorDataCopy();
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  uint32_t seq = nextSeq();

  String msg = "{";
  msg += "\"gateway_id\":\"" + String(GATEWAY_ID) + "\",";
  msg += "\"device_id\":" + String(DEVICE_ID) + ",";
  msg += "\"device_name\":\"" + String(DEVICE_NAME) + "\",";
  msg += "\"msg_type\":\"telemetry\",";
  msg += "\"event_type\":\"sensor_data\",";
  msg += "\"timestamp_ms\":0,";
  msg += "\"timestamp\":0,";
  msg += "\"uptime_ms\":" + String(millis()) + ",";
  msg += "\"temperature\":" + String(data.temperature, 1) + ",";
  msg += "\"humidity\":" + String(data.humidity, 1) + ",";
  msg += "\"voltage\":" + String(data.voltage, 1) + ",";
  msg += "\"status\":1,";
  msg += "\"link_type\":\"wifi\",";
  msg += "\"wifi_rssi\":" + String(rssi) + ",";
  msg += "\"mq2_alarm\":" + String(data.mq2_alarm) + ",";
  msg += "\"ld2402_presence\":" + String(data.ld2402_presence) + ",";
  msg += "\"seq\":" + String(seq);
  msg += "}";

  publishMessage(TOPIC_TELEMETRY, msg);
}

void sendHeartbeat() {
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  uint32_t seq = nextSeq();

  String msg = "{";
  msg += "\"gateway_id\":\"" + String(GATEWAY_ID) + "\",";
  msg += "\"device_id\":" + String(DEVICE_ID) + ",";
  msg += "\"device_name\":\"" + String(DEVICE_NAME) + "\",";
  msg += "\"msg_type\":\"heartbeat\",";
  msg += "\"event_type\":\"heartbeat\",";
  msg += "\"timestamp\":0,";
  msg += "\"uptime_ms\":" + String(millis()) + ",";
  msg += "\"firmware_version\":\"" + fw_version + "\",";
  msg += "\"build_time\":\"" + String(__DATE__) + " " + String(__TIME__) + "\",";
  msg += "\"ota_capable\":1,";
  msg += "\"link_type\":\"wifi\",";
  msg += "\"wifi_rssi\":" + String(rssi) + ",";
  msg += "\"seq\":" + String(seq);
  msg += "}";

  publishMessage(TOPIC_STATUS, msg);
}

void sendAck(int command_id, const String& commandName, int result, const char* message = nullptr) {
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  uint32_t seq = nextSeq();
  const String publicCmd = publicCommandName(commandName);
  const char* text = (message != nullptr) ? message : (result == 0 ? "ok" : "error");

  String msg = "{";
  msg += "\"gateway_id\":\"" + String(GATEWAY_ID) + "\",";
  msg += "\"device_id\":" + String(DEVICE_ID) + ",";
  msg += "\"device_name\":\"" + String(DEVICE_NAME) + "\",";
  msg += "\"msg_type\":\"command_ack\",";
  msg += "\"event_type\":\"command_ack\",";
  msg += "\"timestamp\":0,";
  msg += "\"uptime_ms\":" + String(millis()) + ",";
  msg += "\"command_id\":" + String(command_id) + ",";
  msg += "\"cmd\":\"" + publicCmd + "\",";
  msg += "\"result\":\"" + String(result == 0 ? "ok" : "error") + "\",";
  msg += "\"code\":" + String(result == 0 ? 0 : -1) + ",";
  msg += "\"message\":\"" + String(text) + "\",";
  msg += "\"command_result\":" + String(result) + ",";
  msg += "\"payload_summary\":\"cmd_id=" + String(command_id) + ",result=" + String(result) + "\",";
  msg += "\"link_type\":\"wifi\",";
  msg += "\"wifi_rssi\":" + String(rssi) + ",";
  msg += "\"seq\":" + String(seq);
  msg += "}";

  publishMessage(TOPIC_COMMAND_ACK, msg);
}

void sendStatus() {
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  uint32_t interval = getReportIntervalMs();
  int mode = getRgbOverrideMode();
  SensorData_t data = getSensorDataCopy();
  uint32_t seq = nextSeq();

  String msg = "{";
  msg += "\"gateway_id\":\"" + String(GATEWAY_ID) + "\",";
  msg += "\"device_id\":" + String(DEVICE_ID) + ",";
  msg += "\"device_name\":\"" + String(DEVICE_NAME) + "\",";
  msg += "\"msg_type\":\"status\",";
  msg += "\"event_type\":\"status\",";
  msg += "\"timestamp\":0,";
  msg += "\"uptime_ms\":" + String(millis()) + ",";
  msg += "\"firmware_version\":\"" + fw_version + "\",";
  msg += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? 1 : 0) + ",";
  msg += "\"gateway_connected\":" + String(isTransportConnected() ? 1 : 0) + ",";
  msg += "\"mqtt_connected\":" + String(isMqttConnected() ? 1 : 0) + ",";
  msg += "\"wifi_rssi\":" + String(rssi) + ",";
  msg += "\"rgb_mode\":" + String(mode) + ",";
  msg += "\"report_interval_ms\":" + String(interval) + ",";
  msg += "\"temperature\":" + String(data.temperature, 1) + ",";
  msg += "\"humidity\":" + String(data.humidity, 1) + ",";
  msg += "\"mq2_alarm\":" + String(data.mq2_alarm) + ",";
  msg += "\"ld2402_presence\":" + String(data.ld2402_presence) + ",";
  msg += "\"link_type\":\"wifi\",";
  msg += "\"seq\":" + String(seq);
  msg += "}";

  publishMessage(TOPIC_STATUS, msg);
}

// =========================
// Simple JSON field readers
// This keeps the sketch dependency-free.
// It accepts spaces around ':' better than the original version.
// =========================
int findValueStart(const String& s, const char* key) {
  String k = String("\"") + key + "\"";
  int p = s.indexOf(k);
  if (p < 0) return -1;

  p += k.length();
  while (p < (int)s.length() && isspace((unsigned char)s[p])) p++;
  if (p >= (int)s.length() || s[p] != ':') return -1;

  p++;
  while (p < (int)s.length() && isspace((unsigned char)s[p])) p++;
  return p;
}

int findIntField(const String& s, const char* key, int defaultValue = 0) {
  int p = findValueStart(s, key);
  if (p < 0) return defaultValue;

  int e = p;
  while (e < (int)s.length() && (isDigit(s[e]) || s[e] == '-')) e++;
  if (e == p) return defaultValue;

  return s.substring(p, e).toInt();
}

String findStringField(const String& s, const char* key) {
  int p = findValueStart(s, key);
  if (p < 0) return "";
  if (p >= (int)s.length() || s[p] != '"') return "";

  int e = s.indexOf('"', p + 1);
  if (e < 0) return "";

  return s.substring(p + 1, e);
}

bool runOtaFlow(const String& firmware_url, const String& version_to, uint32_t size_hint, const String& crc32_hint) {
  (void)size_hint;
  (void)crc32_hint;
  otaState.in_progress = true;
  otaState.version_from = fw_version;
  otaState.version_to = version_to;
  otaState.firmware_url = firmware_url;
  sendOtaStatus("received", 1, 0);

#if ESP32_OTA_REAL
  HTTPClient http;
  sendOtaStatus("downloading", 10, 0);
  if (!http.begin(firmware_url)) {
    sendOtaStatus("failed", 0, 1001);
    otaState.in_progress = false;
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    sendOtaStatus("failed", 0, 1002);
    http.end();
    otaState.in_progress = false;
    return false;
  }
  const int total_len = http.getSize();
  WiFiClient* stream = http.getStreamPtr();
  if (!Update.begin(total_len > 0 ? total_len : UPDATE_SIZE_UNKNOWN)) {
    sendOtaStatus("failed", 0, 1003);
    http.end();
    otaState.in_progress = false;
    return false;
  }
  sendOtaStatus("writing", 50, 0);
  const size_t written = Update.writeStream(*stream);
  if (written == 0 || !Update.end(true)) {
    sendOtaStatus("failed", 0, 1004);
    http.end();
    otaState.in_progress = false;
    return false;
  }
  http.end();
  sendOtaStatus("verifying", 95, 0);
#else
  sendOtaStatus("downloading", 25, 0);
  delay(300);
  sendOtaStatus("writing", 60, 0);
  delay(300);
  sendOtaStatus("verifying", 90, 0);
  delay(300);
  fw_version = version_to;
#endif

  sendOtaStatus("rebooting", 100, 0);
  delay(200);
  otaState.in_progress = false;
  return true;
}

void mqttMessageCallback(char* topic, byte* payload, unsigned int length) {
  String body;
  body.reserve(length);
  for (unsigned int i = 0; i < length; ++i) {
    body += static_cast<char>(payload[i]);
  }

  Serial.print("[MQTT-RX] ");
  Serial.print(topic);
  Serial.print(" => ");
  Serial.println(body);

  processCommand(body, String(topic));
}

void setupMqtt() {
  mqttNetClient.setConnectionTimeout(MQTT_CONNECT_TIMEOUT_MS);
  mqttNetClient.setTimeout(MQTT_SOCKET_TIMEOUT_S);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttMessageCallback);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);
  mqttClient.setBufferSize(1024);
}

bool publishOnlineStatus() {
  mqttOnlinePublished = publishMqttMessage(TOPIC_STATUS, buildPresencePayload("online"), true);
  return mqttOnlinePublished;
}

void disconnectMqtt() {
  xSemaphoreTakeRecursive(clientMutex, portMAX_DELAY);
  if (mqttClient.connected()) {
    mqttClient.disconnect();
  }
  xSemaphoreGiveRecursive(clientMutex);
  mqttOnlinePublished = false;
}

bool ensureMqttConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  xSemaphoreTakeRecursive(clientMutex, portMAX_DELAY);
  const bool alreadyConnected = mqttClient.connected();
  xSemaphoreGiveRecursive(clientMutex);
  if (alreadyConnected) {
    return true;
  }

  const uint32_t now = millis();
  if (now - mqttLastConnectAttemptMs < MQTT_RETRY_INTERVAL_MS) {
    return false;
  }
  mqttLastConnectAttemptMs = now;

  Serial.print("[MQTT] Connecting to ");
  Serial.print(MQTT_HOST);
  Serial.print(":");
  Serial.println(MQTT_PORT);

  const String willPayload = buildPresencePayload("offline");
  bool ok = false;

  xSemaphoreTakeRecursive(clientMutex, portMAX_DELAY);
  mqttNetClient.stop();
  Serial.println("[MQTT] TCP connect start");
  if (String(MQTT_USERNAME).length() > 0) {
    ok = mqttClient.connect(
      MQTT_CLIENT_ID,
      MQTT_USERNAME,
      MQTT_PASSWORD,
      topicStatus.c_str(),
      0,
      true,
      willPayload.c_str()
    );
  } else {
    ok = mqttClient.connect(
      MQTT_CLIENT_ID,
      topicStatus.c_str(),
      0,
      true,
      willPayload.c_str()
    );
  }
  Serial.print("[MQTT] connect() returned ");
  Serial.println(ok ? "true" : "false");

  if (ok) {
    Serial.print("[MQTT] Subscribing to ");
    Serial.println(topicCommandDown);
    ok = mqttClient.subscribe(topicCommandDown.c_str());
    Serial.print("[MQTT] subscribe() returned ");
    Serial.println(ok ? "true" : "false");
  }
  if (ok) {
    Serial.print("[MQTT] Subscribing to ");
    Serial.println(topicOtaStart);
    ok = mqttClient.subscribe(topicOtaStart.c_str());
    Serial.print("[MQTT] ota subscribe() returned ");
    Serial.println(ok ? "true" : "false");
  }
  xSemaphoreGiveRecursive(clientMutex);

  if (!ok) {
    Serial.print("[MQTT] Connect failed, state=");
    Serial.println(mqttClient.state());
    mqttNetClient.stop();
    mqttOnlinePublished = false;
    return false;
  }

  Serial.println("[MQTT] Connected");
  mqttOnlinePublished = false;
  publishOnlineStatus();
  sendStatus();
  return true;
}

void serviceMqttLoop() {
  bool connectedBeforeLoop = false;

  xSemaphoreTakeRecursive(clientMutex, portMAX_DELAY);
  connectedBeforeLoop = mqttClient.connected();
  if (connectedBeforeLoop) {
    mqttClient.loop();
  }
  xSemaphoreGiveRecursive(clientMutex);

  const uint32_t now = millis();
  if (now - mqttLastLoopLogMs >= 5000) {
    mqttLastLoopLogMs = now;
    Serial.print("[MQTT] loop connected=");
    Serial.print(connectedBeforeLoop ? 1 : 0);
    Serial.print(", state=");
    Serial.println(mqttClient.state());
  }

  if (connectedBeforeLoop && !isMqttConnected()) {
    Serial.print("[MQTT] loop detected disconnect, state=");
    Serial.println(mqttClient.state());
  }
}

// =========================
// Command processor
// =========================
void processCommand(const String& line, const String& sourceTopic) {
  Serial.print("[RX] ");
  Serial.println(line);

  String type = findStringField(line, "type");
  String legacyCmd = findStringField(line, "command_type");
  String cmd = findStringField(line, "cmd");

  if (cmd.length() == 0) {
    cmd = legacyCmd;
  }
  cmd = canonicalCommandName(cmd);

  if (sourceTopic == topicOtaStart && cmd.length() == 0) {
    cmd = "ota_start";
  }

  if (type.length() == 0 && cmd.length() > 0) {
    type = "command";
  }

  if (type != "command") {
    Serial.println("[CMD] Ignore non-command message");
    return;
  }

  int command_id = findIntField(line, "command_id", 0);
  if (command_id == 0) {
    command_id = static_cast<int>(nextSeq() & 0x7FFFFFFF);
  }

  Serial.print("[CMD] id=");
  Serial.print(command_id);
  Serial.print(", type=");
  Serial.println(cmd);

  int result = 0;
  bool need_status_report = false;
  const char* ackMessage = nullptr;

  if (cmd == "get_status") {
    result = 0;
    need_status_report = true;
    ackMessage = "status queued";
  } else if (cmd == "set_led") {
    int value = findIntField(line, "value", 0);
    setLedState(value ? 1 : 0);

    Serial.print("[RGB] set_led value=");
    Serial.println(value);
    ackMessage = "led updated";
  } else if (cmd == "set_rgb_mode") {
    // value: 0=auto, 1=force green, 2=force off
    int mode = findIntField(line, "value", -1);
    if (mode < 0 || mode > 2) {
      result = 1;
      ackMessage = "invalid rgb mode";
    } else {
      setRgbOverrideMode(mode);
      Serial.print("[RGB] set_rgb_mode=");
      Serial.println(mode);
      ackMessage = "rgb mode updated";
    }
  } else if (cmd == "set_report_interval") {
    int interval = findIntField(line, "interval_ms", -1);
    if (interval < 0) {
      interval = findIntField(line, "value", -1);
    }

    if (interval >= (int)MIN_REPORT_INTERVAL_MS) {
      setReportIntervalMs((uint32_t)interval);
      Serial.print("[REPORT] interval_ms=");
      Serial.println(interval);
      ackMessage = "report interval updated";
    } else {
      result = 1;
      Serial.println("[REPORT] invalid interval");
      ackMessage = "invalid report interval";
    }
  } else if (cmd == "set_log_level") {
    // Reserved command. Keep ACK success for gateway compatibility.
    result = 0;
    ackMessage = "log level noop";
  } else if (cmd == "ota_start") {
    String firmware_url = findStringField(line, "firmware_url");
    String version_to = findStringField(line, "version");
    String crc32_hint = findStringField(line, "crc32");
    int size_value = findIntField(line, "size", 0);
    if (firmware_url.length() == 0 || version_to.length() == 0) {
      result = 1;
      ackMessage = "missing firmware_url or version";
    } else {
      otaState.command_id = (uint32_t)command_id;
      bool ok = runOtaFlow(firmware_url, version_to, (uint32_t)(size_value > 0 ? size_value : 0), crc32_hint);
      result = ok ? 0 : 1;
      ackMessage = ok ? "ota accepted" : "ota failed";
      if (ok) {
        sendOtaStatus("success", 100, 0);
        delay(100);
        ESP.restart();
      }
    }
  } else {
    result = 1;
    ackMessage = "unsupported command";
  }

  Serial.print("[CMD] result=");
  Serial.println(result);

  sendAck(command_id, cmd.length() == 0 ? String("unknown") : cmd, result, ackMessage);

  if (need_status_report && result == 0) {
    sendStatus();
  }
}

// =========================
// FreeRTOS tasks
// =========================
void taskWiFi(void* pvParameters) {
  wl_status_t prevState = WL_IDLE_STATUS;
  uint32_t lastAttemptMs = 0;
  bool connectingLogged = false;

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  while (1) {
    wl_status_t state = WiFi.status();
    uint32_t now = millis();

    if (state == WL_CONNECTED) {
      if (prevState != WL_CONNECTED) {
        Serial.println("[WiFi] Connected");
        Serial.print("[WiFi] IP: ");
        Serial.println(WiFi.localIP());
        Serial.print("[WiFi] RSSI: ");
        Serial.println(WiFi.RSSI());
        connectingLogged = false;
      }
    } else {
      if (prevState == WL_CONNECTED) {
        Serial.println("[WiFi] Disconnected");

        xSemaphoreTakeRecursive(clientMutex, portMAX_DELAY);
        tcpClient.stop();
        if (mqttClient.connected()) {
          mqttClient.disconnect();
        }
        xSemaphoreGiveRecursive(clientMutex);
        mqttOnlinePublished = false;
      }

      if (now - lastAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
        lastAttemptMs = now;

        if (!connectingLogged) {
          Serial.print("[WiFi] Connecting to ");
          Serial.println(WIFI_SSID);
          connectingLogged = true;
        } else {
          Serial.println("[WiFi] Reconnecting...");
        }

        WiFi.disconnect(false);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      }
    }

    prevState = state;
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void taskNetwork(void* pvParameters) {
  uint32_t lastAttemptMs = 0;
  bool connectingLogged = false;

  while (1) {
    if (WiFi.status() != WL_CONNECTED) {
      connectingLogged = false;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

#if ACTIVE_COMM_MODE == COMM_MODE_MQTT
    if (!ensureMqttConnected()) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    serviceMqttLoop();
    vTaskDelay(pdMS_TO_TICKS(20));
#else
    if (!isGatewayConnected()) {
      uint32_t now = millis();
      if (now - lastAttemptMs >= GATEWAY_RETRY_INTERVAL_MS) {
        lastAttemptMs = now;

        if (!connectingLogged) {
          Serial.print("[Gateway] Connecting to ");
          Serial.print(GATEWAY_HOST);
          Serial.print(":");
          Serial.println(GATEWAY_PORT);
          connectingLogged = true;
        }

        xSemaphoreTakeRecursive(clientMutex, portMAX_DELAY);
        tcpClient.stop();
        bool ok = tcpClient.connect(GATEWAY_HOST, GATEWAY_PORT);
        tcpClient.setTimeout(50);
        xSemaphoreGiveRecursive(clientMutex);

        if (ok) {
          Serial.println("[Gateway] Connected");
          connectingLogged = false;
        } else {
          Serial.println("[Gateway] Connect failed, retrying...");
        }
      }

      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    String line = "";
    bool hasLine = false;

    xSemaphoreTakeRecursive(clientMutex, portMAX_DELAY);
    if (tcpClient.connected() && tcpClient.available()) {
      line = tcpClient.readStringUntil('\n');
      hasLine = true;
    }
    xSemaphoreGiveRecursive(clientMutex);

    if (hasLine) {
      line.trim();
      if (line.length() > 0) {
        processCommand(line);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
#endif
  }
}

void taskSensor(void* pvParameters) {
  uint32_t lastDebugLogMs = 0;

  while (1) {
    // Keep temperature/humidity placeholders for now; they can be replaced by real sensors later.
    uint32_t t = millis() / 1000;
    float phase = (t % 40) / 40.0f;

    SensorData_t data;
    data.temperature = 26.0f + 4.0f * phase;
    data.humidity = 40.0f + 20.0f * (1.0f - phase);
    data.voltage = 3.3f;

    if (SENSOR_SIM_MODE == 1) {
      data.mq2_alarm = (t / 20) % 2;
      data.ld2402_presence = (t / 7) % 2;
    } else {
      int mq2_raw = digitalRead(MQ2_DO_PIN);
      int ld2402_raw = digitalRead(LD2402_IO_PIN);

#if MQ2_ACTIVE_LOW
      int mq2_alarm = (mq2_raw == LOW) ? 1 : 0;
#else
      int mq2_alarm = (mq2_raw == HIGH) ? 1 : 0;
#endif

#if LD2402_ACTIVE_HIGH
      int ld2402_presence = (ld2402_raw == HIGH) ? 1 : 0;
#else
      int ld2402_presence = (ld2402_raw == LOW) ? 1 : 0;
#endif

      // MQ-2 needs warm-up time after power-on; suppress alarm during warm-up window.
      if (millis() - sensorBootMs < MQ2_WARMUP_MS) {
        mq2_alarm = 0;
      }

      data.mq2_alarm = mq2_alarm;
      data.ld2402_presence = ld2402_presence;

#if SENSOR_DEBUG_LOG
      uint32_t nowMs = millis();
      if (nowMs - lastDebugLogMs >= SENSOR_DEBUG_LOG_INTERVAL_MS) {
        lastDebugLogMs = nowMs;
        Serial.print("[SENSOR] raw mq2_do=");
        Serial.print(mq2_raw);
        Serial.print(" ld2402_io=");
        Serial.print(ld2402_raw);
        Serial.print(" parsed mq2_alarm=");
        Serial.print(data.mq2_alarm);
        Serial.print(" ld2402_presence=");
        Serial.print(data.ld2402_presence);
        Serial.print(" warmup_left_ms=");
        uint32_t warmupLeft = 0;
        if (nowMs - sensorBootMs < MQ2_WARMUP_MS) {
          warmupLeft = MQ2_WARMUP_MS - (nowMs - sensorBootMs);
        }
        Serial.println(warmupLeft);
      }
#endif
    }

    updateSensorData(data);

    vTaskDelay(pdMS_TO_TICKS(SENSOR_SAMPLE_INTERVAL_MS));
  }
}

void taskReport(void* pvParameters) {
  uint32_t lastSensorMs = 0;
  uint32_t lastHeartbeatMs = 0;

  while (1) {
    uint32_t now = millis();

    if (WiFi.status() == WL_CONNECTED && isTransportConnected()) {
      uint32_t interval = getReportIntervalMs();

      if (now - lastSensorMs >= interval) {
        sendSensorData();
        lastSensorMs = now;
      }

      if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
        sendHeartbeat();
        lastHeartbeatMs = now;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void taskLed(void* pvParameters) {
  bool blinkOn = false;

  while (1) {
    int mode = getRgbOverrideMode();

    if (mode == 1) {
      setRgbColor(0, 60, 0);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (mode == 2) {
      setRgbColor(0, 0, 0);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (WiFi.status() != WL_CONNECTED) {
      blinkOn = !blinkOn;
      if (blinkOn) {
        setRgbColor(180, 120, 0);  // WiFi disconnected: yellow blink
      } else {
        setRgbColor(0, 0, 0);
      }
      vTaskDelay(pdMS_TO_TICKS(300));
      continue;
    }

    if (!isTransportConnected()) {
      blinkOn = !blinkOn;
      if (blinkOn) {
        setRgbColor(0, 0, 60);  // Backend disconnected: blue blink
      } else {
        setRgbColor(0, 0, 0);
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    setRgbColor(0, 60, 0);  // WiFi and gateway connected: green
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// =========================
// Arduino entry points
// =========================
void setup() {
  Serial.begin(115200);
  delay(200);

  clientMutex = xSemaphoreCreateRecursiveMutex();
  stateMutex = xSemaphoreCreateMutex();
  sensorMutex = xSemaphoreCreateMutex();

  if (clientMutex == NULL || stateMutex == NULL || sensorMutex == NULL) {
    Serial.println("[FATAL] Failed to create mutex");
    while (1) {
      delay(1000);
    }
  }

  setRgbColor(0, 0, 0);
  pinMode(MQ2_DO_PIN, INPUT_PULLUP);
  pinMode(LD2402_IO_PIN, INPUT);
  sensorBootMs = millis();

  Serial.print("[SENSOR] mode=");
  Serial.println(SENSOR_SIM_MODE == 1 ? "simulated" : "real_gpio");
  Serial.print("[SENSOR] mq2_do_pin=");
  Serial.print(MQ2_DO_PIN);
  Serial.print(" active_low=");
  Serial.println(MQ2_ACTIVE_LOW);
  Serial.print("[SENSOR] ld2402_io_pin=");
  Serial.print(LD2402_IO_PIN);
  Serial.print(" active_high=");
  Serial.println(LD2402_ACTIVE_HIGH);
  Serial.print("[SENSOR] mq2_warmup_ms=");
  Serial.println(MQ2_WARMUP_MS);

  Serial.println();
  Serial.println("================================");
  Serial.println("ESP32 WiFi Node FreeRTOS Start");
  Serial.println("Baudrate: 115200");
  Serial.print("Comm mode: ");
  Serial.println(activeCommModeName());
  Serial.println("Tasks: WiFi, Network, Sensor, Report, LED");
  Serial.println("================================");

  initTopics();
  setupMqtt();

  xTaskCreatePinnedToCore(
    taskWiFi,
    "WiFiTask",
    4096,
    NULL,
    3,
    NULL,
    0
  );

  xTaskCreatePinnedToCore(
    taskNetwork,
    "NetworkTask",
    6144,
    NULL,
    3,
    NULL,
    1
  );

  xTaskCreatePinnedToCore(
    taskSensor,
    "SensorTask",
    4096,
    NULL,
    2,
    NULL,
    1
  );

  xTaskCreatePinnedToCore(
    taskReport,
    "ReportTask",
    6144,
    NULL,
    2,
    NULL,
    1
  );

  xTaskCreatePinnedToCore(
    taskLed,
    "LedTask",
    2048,
    NULL,
    1,
    NULL,
    1
  );
}

void loop() {
  // All application logic runs in FreeRTOS tasks.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
