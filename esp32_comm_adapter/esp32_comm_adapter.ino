#include <Arduino.h>
#include <WiFi.h>
#include <errno.h>

#include "config.h"

/*
 * Merged ESP32 communication adapter.
 *
 * Default behavior:
 * - UART -> TCP: parse AA 55 framed protocol, verify CRC16-Modbus, then forward to TCP.
 * - TCP -> UART: transparent forwarding.
 * - TCP adapter-control frames can be consumed locally to enter/leave OTA bridge mode.
 *
 * Optional compile-time knobs can be placed in config.h:
 *   #define BRIDGE_ADAPTER_ID 1
 *   #define BRIDGE_ENABLE_OTA_CONTROL 1
 *   #define BRIDGE_OTA_DEFAULT_TIMEOUT_MS 120000UL
 *   #define BRIDGE_TCP_RX_PARTIAL_TIMEOUT_MS 200UL
 *   #define BRIDGE_STATS_INTERVAL_MS 5000UL
 */

#ifndef BRIDGE_ADAPTER_ID
#define BRIDGE_ADAPTER_ID 1
#endif

#ifndef BRIDGE_ENABLE_OTA_CONTROL
#define BRIDGE_ENABLE_OTA_CONTROL 1
#endif

#ifndef BRIDGE_OTA_DEFAULT_TIMEOUT_MS
#define BRIDGE_OTA_DEFAULT_TIMEOUT_MS 120000UL
#endif

#ifndef BRIDGE_TCP_RX_PARTIAL_TIMEOUT_MS
#define BRIDGE_TCP_RX_PARTIAL_TIMEOUT_MS 200UL
#endif

#ifndef BRIDGE_STATS_INTERVAL_MS
#define BRIDGE_STATS_INTERVAL_MS 60000UL
#endif

#ifndef OTA_TRACE_ACK
#define OTA_TRACE_ACK 1
#endif

#ifndef BRIDGE_CRC_DEBUG_DUMP_ENABLE
#define BRIDGE_CRC_DEBUG_DUMP_ENABLE 1
#endif

#ifndef BRIDGE_CRC_DEBUG_DUMP_INTERVAL_MS
#define BRIDGE_CRC_DEBUG_DUMP_INTERVAL_MS 1000UL
#endif

#ifndef BRIDGE_CRC_DEBUG_DUMP_BYTES
#define BRIDGE_CRC_DEBUG_DUMP_BYTES 24U
#endif

#ifndef BRIDGE_CRC_ERROR_LOG_INTERVAL_MS
#define BRIDGE_CRC_ERROR_LOG_INTERVAL_MS 1000UL
#endif

#ifndef BRIDGE_NORMAL_FRAME_LOG_ENABLE
#define BRIDGE_NORMAL_FRAME_LOG_ENABLE 0
#endif

#ifndef BRIDGE_ENABLE_LEGACY_LEN_COMPAT
#define BRIDGE_ENABLE_LEGACY_LEN_COMPAT 0
#endif

#ifndef BRIDGE_OTA_CRC_ERROR_STREAK_LIMIT
#define BRIDGE_OTA_CRC_ERROR_STREAK_LIMIT 3U
#endif

#ifndef BRIDGE_OTA_CRC_ERROR_STREAK_WINDOW_MS
#define BRIDGE_OTA_CRC_ERROR_STREAK_WINDOW_MS 1500UL
#endif

#ifndef BRIDGE_QUEUE_OVERFLOW_CLOSE_INTERVAL_MS
#define BRIDGE_QUEUE_OVERFLOW_CLOSE_INTERVAL_MS 1000UL
#endif

namespace {

constexpr uint8_t kHeader1 = 0xAA;
constexpr uint8_t kHeader2 = 0x55;
constexpr uint8_t kTypeAdapterHeartbeat = 0x7E;
constexpr size_t kMinFrameSize = 6;

#if BRIDGE_ENABLE_OTA_CONTROL
constexpr uint8_t kTypeAdapterControl = 0x7D;
constexpr uint8_t kCmdEnterOtaMode = 0x01;
constexpr uint8_t kCmdLeaveOtaMode = 0x02;
#endif

HardwareSerial& kUart = Serial2;
WiFiClient g_tcp;
QueueHandle_t g_frame_q = nullptr;
SemaphoreHandle_t g_tcp_lock = nullptr;
portMUX_TYPE g_stats_mux = portMUX_INITIALIZER_UNLOCKED;

volatile uint64_t g_valid_frames = 0;
volatile uint64_t g_crc_errors = 0;
volatile uint64_t g_dropped_frames = 0;
volatile uint64_t g_uart_rx_bytes = 0;
volatile uint64_t g_tcp_tx_bytes = 0;
volatile uint64_t g_tcp_rx_bytes = 0;
volatile uint64_t g_ack_like_uart_seen = 0;
volatile uint64_t g_ack_like_tcp_fwd_ok = 0;
volatile uint64_t g_ack_like_tcp_fwd_fail = 0;
volatile uint64_t g_queue_overflow_events = 0;
volatile uint32_t g_last_stats_ms = 0;
volatile uint32_t g_last_crc_dump_ms = 0;
volatile uint32_t g_last_crc_error_log_ms = 0;
volatile uint32_t g_last_queue_overflow_close_ms = 0;
volatile bool g_tcp_close_requested = false;

enum BridgeMode : uint8_t {
  BRIDGE_MODE_NORMAL = 0,
  BRIDGE_MODE_OTA = 1
};

volatile BridgeMode g_bridge_mode = BRIDGE_MODE_NORMAL;
volatile uint32_t g_ota_session_id = 0;
volatile uint32_t g_ota_deadline_ms = 0;
volatile uint32_t g_ota_crc_error_streak = 0;
volatile uint32_t g_ota_last_crc_error_ms = 0;

bool bridgeIsOtaMode();
bool isAckLikeType(uint8_t type);
bool isOtaCmdType(uint8_t type);
bool isOtaRespType(uint8_t type);

void statsAdd(volatile uint64_t* value, uint64_t delta = 1) {
  portENTER_CRITICAL(&g_stats_mux);
  *value += delta;
  portEXIT_CRITICAL(&g_stats_mux);
}

uint64_t statsRead(const volatile uint64_t* value) {
  portENTER_CRITICAL(&g_stats_mux);
  const uint64_t out = *value;
  portEXIT_CRITICAL(&g_stats_mux);
  return out;
}

void statsSet(volatile uint32_t* value, uint32_t next) {
  portENTER_CRITICAL(&g_stats_mux);
  *value = next;
  portEXIT_CRITICAL(&g_stats_mux);
}

uint32_t statsReadU32(const volatile uint32_t* value) {
  portENTER_CRITICAL(&g_stats_mux);
  const uint32_t out = *value;
  portEXIT_CRITICAL(&g_stats_mux);
  return out;
}

struct FramePacket {
  uint16_t len;
  uint8_t data[MAX_FRAME_SIZE];
};

uint16_t crc16_modbus(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int j = 0; j < 8; ++j) {
      crc = (crc & 1U) ? static_cast<uint16_t>((crc >> 1U) ^ 0xA001U)
                       : static_cast<uint16_t>(crc >> 1U);
    }
  }
  return crc;
}

bool elapsedSince(uint32_t now, uint32_t since, uint32_t interval_ms) {
  return static_cast<uint32_t>(now - since) >= interval_ms;
}

bool deadlineReached(uint32_t now, uint32_t deadline_ms) {
  return static_cast<int32_t>(now - deadline_ms) >= 0;
}

bool tcpWriteAll(const uint8_t* p, size_t n, size_t* sent_out, int* last_wn_out) {
  size_t sent = 0;
  int last_wn = 0;
  while (sent < n) {
    int wn = g_tcp.write(p + sent, n - sent);
    last_wn = wn;
    if (wn <= 0) {
      if (sent_out) *sent_out = sent;
      if (last_wn_out) *last_wn_out = last_wn;
      return false;
    }
    sent += static_cast<size_t>(wn);
    if (sent < n) {
      Serial.printf("[BRIDGE] tcp short write retry sent=%u remain=%u\n",
                    static_cast<unsigned>(sent),
                    static_cast<unsigned>(n - sent));
    }
  }
  statsAdd(&g_tcp_tx_bytes, sent);
  if (sent_out) *sent_out = sent;
  if (last_wn_out) *last_wn_out = last_wn;
  return true;
}

bool tcpWriteAll(const uint8_t* p, size_t n) {
  return tcpWriteAll(p, n, nullptr, nullptr);
}

void enqueueFrameDropOldest(const uint8_t* p, size_t n) {
  if (n > MAX_FRAME_SIZE || g_frame_q == nullptr) {
    return;
  }

  FramePacket pkt{};
  pkt.len = static_cast<uint16_t>(n);
  memcpy(pkt.data, p, n);

  if (xQueueSend(g_frame_q, &pkt, 0) == pdPASS) {
    return;
  }

  FramePacket dropped{};
  if (xQueueReceive(g_frame_q, &dropped, 0) == pdPASS) {
    statsAdd(&g_dropped_frames);
    Serial.printf("[BRIDGE] queue full drop_oldest dropped=%llu\n",
                  static_cast<unsigned long long>(statsRead(&g_dropped_frames)));
  }
  (void)xQueueSend(g_frame_q, &pkt, 0);
}

bool enqueueFrameNoDrop(const uint8_t* p, size_t n) {
  if (n > MAX_FRAME_SIZE || g_frame_q == nullptr) {
    return false;
  }

  FramePacket pkt{};
  pkt.len = static_cast<uint16_t>(n);
  memcpy(pkt.data, p, n);
  const bool ok = (xQueueSend(g_frame_q, &pkt, 0) == pdPASS);
#if OTA_TRACE_ACK
  if (bridgeIsOtaMode() && n >= 4 && (p[3] == 0x22U || p[3] == 0x23U)) {
    Serial.printf("[BRIDGE][OTA] enqueue ack-like type=0x%02X len=%u q=%u ok=%d\n",
                  p[3],
                  static_cast<unsigned>(n),
                  static_cast<unsigned>(uxQueueMessagesWaiting(g_frame_q)),
                  ok ? 1 : 0);
  }
#endif
  return ok;
}

bool requeueFrameToFront(const FramePacket& pkt) {
  if (g_frame_q == nullptr) {
    return false;
  }
  return xQueueSendToFront(g_frame_q, &pkt, 0) == pdPASS;
}

bool bridgeIsOtaMode() {
  return g_bridge_mode == BRIDGE_MODE_OTA;
}

bool isAckLikeType(uint8_t type) {
  return type == 0x22U || type == 0x23U;
}

bool isOtaCmdType(uint8_t type) {
  return type >= 0x31U && type <= 0x36U;
}

bool isOtaRespType(uint8_t type) {
  return type == 0x20U || type == 0x22U || type == 0x23U;
}

uint16_t readU16Le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8U);
}

void requestTcpClose() {
  g_tcp_close_requested = true;
}

void onQueueOverflow(const char* reason) {
  const uint32_t now = millis();
  statsAdd(&g_queue_overflow_events);
  const uint32_t last_close_ms = statsReadU32(&g_last_queue_overflow_close_ms);
  if (elapsedSince(now, last_close_ms, static_cast<uint32_t>(BRIDGE_QUEUE_OVERFLOW_CLOSE_INTERVAL_MS))) {
    Serial.printf("[BRIDGE] queue overflow reason=%s queue=%u request tcp reconnect\n",
                  reason ? reason : "unknown",
                  static_cast<unsigned>(uxQueueMessagesWaiting(g_frame_q)));
    statsSet(&g_last_queue_overflow_close_ms, now);
    requestTcpClose();
  }
}

void bridgeEnterOtaMode(uint32_t session_id, uint32_t timeout_ms) {
  const uint32_t effective_timeout =
      timeout_ms > 0 ? timeout_ms : static_cast<uint32_t>(BRIDGE_OTA_DEFAULT_TIMEOUT_MS);

  g_bridge_mode = BRIDGE_MODE_OTA;
  g_ota_session_id = session_id;
  g_ota_deadline_ms = millis() + effective_timeout;
  g_ota_crc_error_streak = 0;
  g_ota_last_crc_error_ms = 0;

  Serial.printf("[BRIDGE] enter ota mode session=%lu timeout=%lu\n",
                static_cast<unsigned long>(session_id),
                static_cast<unsigned long>(effective_timeout));
}

void bridgeLeaveOtaMode(const char* reason) {
  if (bridgeIsOtaMode()) {
    Serial.printf("[BRIDGE] leave ota mode reason=%s session=%lu\n",
                  reason ? reason : "unknown",
                  static_cast<unsigned long>(g_ota_session_id));
  }

  g_bridge_mode = BRIDGE_MODE_NORMAL;
  g_ota_session_id = 0;
  g_ota_deadline_ms = 0;
  g_ota_crc_error_streak = 0;
  g_ota_last_crc_error_ms = 0;
}

#if BRIDGE_ENABLE_OTA_CONTROL
bool handleAdapterControlFrame(const uint8_t* frame_buf, size_t total) {
  // Frame format: AA 55 len type payload... crc_lo crc_hi
  // Adapter-control payload:
  //   cmd[1], session_id[4] little-endian, timeout_ms[4] little-endian
  constexpr size_t kControlPayloadSize = 1 + 4 + 4;
  const size_t expected_total = 2 + 1 + 1 + kControlPayloadSize + 2;
  if (total < expected_total) {
    return false;
  }

  const uint8_t* p = frame_buf + 4;
  const uint8_t cmd = p[0];
  const uint32_t session_id = static_cast<uint32_t>(p[1]) |
                              (static_cast<uint32_t>(p[2]) << 8U) |
                              (static_cast<uint32_t>(p[3]) << 16U) |
                              (static_cast<uint32_t>(p[4]) << 24U);
  const uint32_t timeout_ms = static_cast<uint32_t>(p[5]) |
                              (static_cast<uint32_t>(p[6]) << 8U) |
                              (static_cast<uint32_t>(p[7]) << 16U) |
                              (static_cast<uint32_t>(p[8]) << 24U);

  if (cmd == kCmdEnterOtaMode) {
    bridgeEnterOtaMode(session_id, timeout_ms);
    return true;
  }

  if (cmd == kCmdLeaveOtaMode) {
    bridgeLeaveOtaMode("control");
    return true;
  }

  return false;
}
#endif

void sendAdapterHeartbeat() {
  uint8_t payload[24] = {0};
  const uint32_t uptime = millis();
  const int32_t rssi = WiFi.RSSI();

  payload[0] = static_cast<uint8_t>(BRIDGE_ADAPTER_ID);
  payload[1] = static_cast<uint8_t>(uptime & 0xFF);
  payload[2] = static_cast<uint8_t>((uptime >> 8) & 0xFF);
  payload[3] = static_cast<uint8_t>((uptime >> 16) & 0xFF);
  payload[4] = static_cast<uint8_t>((uptime >> 24) & 0xFF);
  payload[5] = static_cast<uint8_t>(rssi & 0xFF);
  payload[6] = static_cast<uint8_t>((rssi >> 8) & 0xFF);

  const uint32_t rx = static_cast<uint32_t>(statsRead(&g_uart_rx_bytes));
  const uint32_t tx = static_cast<uint32_t>(statsRead(&g_tcp_tx_bytes));
  const uint32_t ok = static_cast<uint32_t>(statsRead(&g_valid_frames));
  const uint32_t crc = static_cast<uint32_t>(statsRead(&g_crc_errors));
  const uint16_t qd = static_cast<uint16_t>(uxQueueMessagesWaiting(g_frame_q));
  const uint16_t drop = static_cast<uint16_t>(statsRead(&g_dropped_frames));

  memcpy(&payload[7], &rx, 4);
  memcpy(&payload[11], &tx, 4);
  memcpy(&payload[15], &ok, 4);
  payload[19] = static_cast<uint8_t>(crc & 0xFF);
  payload[20] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  payload[21] = static_cast<uint8_t>(qd & 0xFF);
  payload[22] = static_cast<uint8_t>((qd >> 8) & 0xFF);
  payload[23] = static_cast<uint8_t>(drop & 0xFF);

  uint8_t frame[2 + 1 + 1 + sizeof(payload) + 2] = {0};
  frame[0] = kHeader1;
  frame[1] = kHeader2;
  frame[2] = static_cast<uint8_t>(1 + sizeof(payload));
  frame[3] = kTypeAdapterHeartbeat;
  memcpy(frame + 4, payload, sizeof(payload));

  const uint16_t crc16 = crc16_modbus(frame + 2, 1 + frame[2]);
  const size_t total = 2 + 1 + frame[2] + 2;
  frame[total - 2] = static_cast<uint8_t>(crc16 & 0xFF);
  frame[total - 1] = static_cast<uint8_t>((crc16 >> 8) & 0xFF);
  (void)tcpWriteAll(frame, total);
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.printf("[BRIDGE] wifi connected ssid=%s\n", WIFI_SSID);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[BRIDGE] wifi got ip=%s rssi=%d\n",
                    WiFi.localIP().toString().c_str(),
                    WiFi.RSSI());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[BRIDGE] wifi disconnected reason=%u\n",
                    static_cast<unsigned>(info.wifi_sta_disconnected.reason));
      break;
    default:
      break;
  }
}

void flushTcpBufferToUart(uint8_t* buf, size_t& len) {
  for (size_t i = 0; i < len; ++i) {
    kUart.write(buf[i]);
  }
  len = 0;
}

void dumpBytesHex(const uint8_t* p, size_t n) {
  static const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < n; ++i) {
    char out[4];
    out[0] = hex[(p[i] >> 4) & 0x0F];
    out[1] = hex[p[i] & 0x0F];
    out[2] = (i + 1 < n) ? ' ' : '\0';
    out[3] = '\0';
    Serial.print(out);
  }
  Serial.println();
}

void pumpTcpToUart() {
#if !BRIDGE_ENABLE_OTA_CONTROL
  while (g_tcp.connected() && g_tcp.available() > 0) {
    const int b = g_tcp.read();
    if (b >= 0) {
      statsAdd(&g_tcp_rx_bytes);
      kUart.write(static_cast<uint8_t>(b));
    }
  }
#else
  // Mostly transparent TCP->UART forwarding, but consume well-formed adapter-control
  // frames locally. Partial AA55 frames are flushed after a short timeout so random
  // transparent bytes cannot get stuck forever behind a header-looking prefix.
  static uint8_t tcp_in_buf[MAX_FRAME_SIZE] = {0};
  static size_t tcp_in_len = 0;
  static uint32_t last_tcp_rx_ms = 0;

  const uint32_t now = millis();
  if (tcp_in_len > 0 &&
      elapsedSince(now, last_tcp_rx_ms, static_cast<uint32_t>(BRIDGE_TCP_RX_PARTIAL_TIMEOUT_MS))) {
    flushTcpBufferToUart(tcp_in_buf, tcp_in_len);
  }

  while (g_tcp.connected() && g_tcp.available() > 0) {
    const int b = g_tcp.read();
    if (b < 0) {
      break;
    }

    statsAdd(&g_tcp_rx_bytes);
    const uint8_t ub = static_cast<uint8_t>(b);
    last_tcp_rx_ms = millis();

    if (tcp_in_len == 0 && ub != kHeader1) {
      kUart.write(ub);
      continue;
    }

    if (tcp_in_len == 1 && ub != kHeader2) {
      kUart.write(tcp_in_buf[0]);
      tcp_in_len = 0;
      kUart.write(ub);
      continue;
    }

    if (tcp_in_len >= MAX_FRAME_SIZE) {
      flushTcpBufferToUart(tcp_in_buf, tcp_in_len);
    }

    tcp_in_buf[tcp_in_len++] = ub;

    if (tcp_in_len < 3) {
      continue;
    }

    const uint8_t wire_len = tcp_in_buf[2];
    const size_t total_main = 2 + 1 + static_cast<size_t>(wire_len) + 2;  // LEN = TYPE + PAYLOAD
    const bool total_main_ok = (total_main <= MAX_FRAME_SIZE && total_main >= kMinFrameSize);
#if BRIDGE_ENABLE_LEGACY_LEN_COMPAT
    const size_t total_legacy = 2 + 1 + 1 + static_cast<size_t>(wire_len) + 2;  // LEN = PAYLOAD
    const bool total_legacy_ok = (total_legacy <= MAX_FRAME_SIZE && total_legacy >= kMinFrameSize);
#endif

#if BRIDGE_ENABLE_LEGACY_LEN_COMPAT
    if (!total_main_ok && !total_legacy_ok) {
#else
    if (!total_main_ok) {
#endif
      flushTcpBufferToUart(tcp_in_buf, tcp_in_len);
      continue;
    }

    size_t total = 0;
    bool frame_ok = false;
    uint16_t wire_crc = 0;
    uint16_t calc_crc = 0;

    if (total_main_ok && tcp_in_len >= total_main) {
      const uint16_t wire_crc_main = static_cast<uint16_t>(tcp_in_buf[total_main - 2]) |
                                     (static_cast<uint16_t>(tcp_in_buf[total_main - 1]) << 8U);
      const uint16_t calc_crc_main = crc16_modbus(tcp_in_buf + 2, 1 + wire_len);
      if (wire_crc_main == calc_crc_main) {
        frame_ok = true;
        total = total_main;
        wire_crc = wire_crc_main;
        calc_crc = calc_crc_main;
      } else {
        wire_crc = wire_crc_main;
        calc_crc = calc_crc_main;
      }
    }

#if BRIDGE_ENABLE_LEGACY_LEN_COMPAT
    if (!frame_ok && total_legacy_ok && tcp_in_len >= total_legacy) {
      const uint16_t wire_crc_legacy = static_cast<uint16_t>(tcp_in_buf[total_legacy - 2]) |
                                       (static_cast<uint16_t>(tcp_in_buf[total_legacy - 1]) << 8U);
      // Legacy format: LEN means PAYLOAD only, CRC covers LEN+TYPE+PAYLOAD.
      const uint16_t calc_crc_legacy = crc16_modbus(tcp_in_buf + 2, 2 + wire_len);
      if (wire_crc_legacy == calc_crc_legacy) {
        frame_ok = true;
        total = total_legacy;
        wire_crc = wire_crc_legacy;
        calc_crc = calc_crc_legacy;
      } else if (!total_main_ok || tcp_in_len < total_main) {
        wire_crc = wire_crc_legacy;
        calc_crc = calc_crc_legacy;
      }
    }
#endif

    const size_t min_total = total_main;
    if (tcp_in_len < min_total) {
      continue;
    }

    if (!frame_ok) {
      flushTcpBufferToUart(tcp_in_buf, tcp_in_len);
      continue;
    }

    if (wire_crc == calc_crc && tcp_in_buf[3] == kTypeAdapterControl &&
        handleAdapterControlFrame(tcp_in_buf, total)) {
      tcp_in_len = 0;
      continue;
    }

#if OTA_TRACE_ACK
    if (bridgeIsOtaMode()) {
      const uint8_t d_type = tcp_in_buf[3];
      const size_t dump_n = (total < 8U) ? total : 8U;
      Serial.printf("[BRIDGE][OTA] tcp->uart frame type=0x%02X len=%u wire_len=%u crc_ok=%d\n",
                    d_type,
                    static_cast<unsigned>(total),
                    static_cast<unsigned>(wire_len),
                    (wire_crc == calc_crc) ? 1 : 0);
      Serial.print("[BRIDGE][OTA] tcp->uart head=");
      dumpBytesHex(tcp_in_buf, dump_n);
    }
#endif

    if (bridgeIsOtaMode()) {
      const uint8_t d_type = tcp_in_buf[3];
      if (!isOtaCmdType(d_type)) {
#if OTA_TRACE_ACK
        Serial.printf("[BRIDGE][OTA] drop tcp->uart non-ota type=0x%02X len=%u\n",
                      d_type,
                      static_cast<unsigned>(total));
#endif
        tcp_in_len = 0;
        continue;
      }
    }

    // Not an adapter-control frame. Preserve original bridge behavior and forward it.
    for (size_t i = 0; i < total; ++i) {
      kUart.write(tcp_in_buf[i]);
    }
    tcp_in_len = 0;
  }
#endif
}

void closeTcpIfRequested() {
  if (!g_tcp_close_requested) {
    return;
  }

  if (xSemaphoreTake(g_tcp_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
    Serial.println("[BRIDGE] tcp close requested");
    g_tcp.stop();
    g_tcp_close_requested = false;
    xSemaphoreGive(g_tcp_lock);
  }
}

void networkTask(void*) {
  uint32_t last_hb_ms = 0;
  uint32_t last_wifi_connect_attempt_ms = 0;

  while (true) {
    closeTcpIfRequested();

    if (WiFi.status() != WL_CONNECTED) {
      const uint32_t now = millis();
      if (elapsedSince(now, last_wifi_connect_attempt_ms, WIFI_RECONNECT_MS)) {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        last_wifi_connect_attempt_ms = now;
        Serial.println("[BRIDGE] wifi reconnect attempt");
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    if (!g_tcp.connected()) {
      g_tcp.stop();
      if (g_tcp.connect(GATEWAY_HOST, GATEWAY_TCP_BINARY_PORT)) {
        Serial.printf("[BRIDGE] tcp connected host=%s port=%d\n",
                      GATEWAY_HOST,
                      GATEWAY_TCP_BINARY_PORT);
      } else {
        vTaskDelay(pdMS_TO_TICKS(TCP_RECONNECT_MS));
        continue;
      }
    }

    pumpTcpToUart();

    FramePacket pkt{};
    if (xQueueReceive(g_frame_q, &pkt, pdMS_TO_TICKS(20)) == pdPASS) {
      const uint8_t pkt_type = (pkt.len >= 4U) ? pkt.data[3] : 0xFFU;
#if OTA_TRACE_ACK
      if (bridgeIsOtaMode() && isAckLikeType(pkt_type)) {
        Serial.printf("[BRIDGE][OTA] dequeue ack-like type=0x%02X len=%u q=%u\n",
                      pkt_type,
                      static_cast<unsigned>(pkt.len),
                      static_cast<unsigned>(uxQueueMessagesWaiting(g_frame_q)));
      }
#endif
      if (xSemaphoreTake(g_tcp_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        size_t sent_bytes = 0;
        int last_wn = 0;
        const bool connected = g_tcp.connected();
        const bool write_ok = connected && tcpWriteAll(pkt.data, pkt.len, &sent_bytes, &last_wn);
#if OTA_TRACE_ACK
        if (bridgeIsOtaMode()) {
          const size_t dump_n = (pkt.len < 8U) ? pkt.len : 8U;
          Serial.printf("[BRIDGE][OTA] tcp send type=0x%02X len=%u sent=%u last_wn=%d errno=%d q=%u\n",
                        pkt_type,
                        static_cast<unsigned>(pkt.len),
                        static_cast<unsigned>(sent_bytes),
                        last_wn,
                        errno,
                        static_cast<unsigned>(uxQueueMessagesWaiting(g_frame_q)));
          Serial.print("[BRIDGE][OTA] tcp send head=");
          dumpBytesHex(pkt.data, dump_n);
          if (isAckLikeType(pkt_type)) {
            const uint16_t ack_seq = (pkt.len >= 6U) ? readU16Le(&pkt.data[4]) : 0U;
            Serial.printf("[BRIDGE][OTA] tcp forward ack-like type=0x%02X wrote=%u\n",
                          pkt_type,
                          static_cast<unsigned>(sent_bytes));
            Serial.printf("[BRIDGE][OTA] tcp fwd ack-like seq=0x%04X t=%lu\n",
                          static_cast<unsigned>(ack_seq),
                          static_cast<unsigned long>(millis()));
            if (write_ok) {
              statsAdd(&g_ack_like_tcp_fwd_ok);
            } else {
              statsAdd(&g_ack_like_tcp_fwd_fail);
            }
          }
        }
#endif
        if (!connected || !write_ok) {
          if (bridgeIsOtaMode()) {
            Serial.printf("[BRIDGE] ota tcp write failed, requeue front and close tcp type=0x%02X len=%u sent=%u last_wn=%d errno=%d\n",
                          pkt_type,
                          static_cast<unsigned>(pkt.len),
                          static_cast<unsigned>(sent_bytes),
                          last_wn,
                          errno);
            if (!requeueFrameToFront(pkt)) {
            Serial.println("[BRIDGE] ota requeue failed");
          }
          bridgeLeaveOtaMode("tcp_write_fail");
        } else {
          Serial.println("[BRIDGE] tcp disconnected reconnecting");
          if (!requeueFrameToFront(pkt)) {
            enqueueFrameDropOldest(pkt.data, pkt.len);
            onQueueOverflow("requeue_front_fail");
          }
        }
        g_tcp.stop();
        }
        xSemaphoreGive(g_tcp_lock);
      } else {
        if (bridgeIsOtaMode()) {
          if (!requeueFrameToFront(pkt)) {
            Serial.println("[BRIDGE] ota tcp lock timeout and requeue failed");
            bridgeLeaveOtaMode("queue_overflow");
            requestTcpClose();
          }
        } else {
          enqueueFrameDropOldest(pkt.data, pkt.len);
          onQueueOverflow("tcp_lock_timeout");
        }
      }
    }

    const uint32_t now = millis();

    if (g_tcp.connected() && elapsedSince(now, last_hb_ms, BRIDGE_HEARTBEAT_MS)) {
      if (xSemaphoreTake(g_tcp_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        sendAdapterHeartbeat();
        xSemaphoreGive(g_tcp_lock);
      }
      last_hb_ms = now;
    }

    if (bridgeIsOtaMode() && g_ota_deadline_ms != 0 && deadlineReached(now, g_ota_deadline_ms)) {
      bridgeLeaveOtaMode("ota_timeout");
      requestTcpClose();
    }

    if (elapsedSince(now, g_last_stats_ms, BRIDGE_STATS_INTERVAL_MS)) {
      g_last_stats_ms = now;
      Serial.printf("[BRIDGE] mode=%s rssi=%d valid_frames=%llu crc_errors=%llu dropped=%llu queue=%u\n",
                    bridgeIsOtaMode() ? "OTA" : "NORMAL",
                    WiFi.RSSI(),
                    static_cast<unsigned long long>(statsRead(&g_valid_frames)),
                    static_cast<unsigned long long>(statsRead(&g_crc_errors)),
                    static_cast<unsigned long long>(statsRead(&g_dropped_frames)),
                    static_cast<unsigned>(uxQueueMessagesWaiting(g_frame_q)));
      Serial.printf("[BRIDGE] queue_overflow_events=%llu tcp_close_pending=%d\n",
                    static_cast<unsigned long long>(statsRead(&g_queue_overflow_events)),
                    g_tcp_close_requested ? 1 : 0);
      if (bridgeIsOtaMode()) {
        Serial.printf("[BRIDGE][OTA] ack-like stats uart_seen=%llu tcp_fwd_ok=%llu tcp_fwd_fail=%llu\n",
                      static_cast<unsigned long long>(statsRead(&g_ack_like_uart_seen)),
                      static_cast<unsigned long long>(statsRead(&g_ack_like_tcp_fwd_ok)),
                      static_cast<unsigned long long>(statsRead(&g_ack_like_tcp_fwd_fail)));
      }
    }
  }
}

void uartTask(void*) {
  uint8_t frame_buf[MAX_FRAME_SIZE] = {0};
  size_t frame_len = 0;

  while (true) {
    while (kUart.available() > 0) {
      const uint8_t b = static_cast<uint8_t>(kUart.read());
      statsAdd(&g_uart_rx_bytes);

      if (frame_len == 0 && b != kHeader1) {
        continue;
      }

      if (frame_len == 1 && b != kHeader2) {
        frame_len = 0;
        continue;
      }

      if (frame_len >= MAX_FRAME_SIZE) {
        frame_len = 0;
      }

      frame_buf[frame_len++] = b;

      if (frame_len < 3) {
        continue;
      }

      const uint8_t wire_len = frame_buf[2];
      const bool ota_mode_now = bridgeIsOtaMode();
      const size_t total_main = 2 + 1 + static_cast<size_t>(wire_len) + 2;  // LEN = TYPE + PAYLOAD
      const bool total_main_ok = (total_main <= MAX_FRAME_SIZE && total_main >= kMinFrameSize);
      const bool try_legacy = (!ota_mode_now);
      const size_t total_legacy = 2 + 1 + 1 + static_cast<size_t>(wire_len) + 2;  // LEN = PAYLOAD
      const bool total_legacy_ok =
          try_legacy && (total_legacy <= MAX_FRAME_SIZE && total_legacy >= kMinFrameSize);

      if (!total_main_ok && !total_legacy_ok) {
        frame_len = 0;
        continue;
      }

      const size_t min_total = (total_main_ok && total_legacy_ok)
                                   ? ((total_main < total_legacy) ? total_main : total_legacy)
                                   : (total_main_ok ? total_main : total_legacy);
      if (frame_len < min_total) {
        continue;
      }

      bool frame_ok = false;
      size_t total = 0;
      uint16_t wire_crc = 0;
      uint16_t calc_crc = 0;

      if (total_main_ok && frame_len >= total_main) {
        const uint16_t wire_crc_main = static_cast<uint16_t>(frame_buf[total_main - 2]) |
                                       (static_cast<uint16_t>(frame_buf[total_main - 1]) << 8U);
        const uint16_t calc_crc_main = crc16_modbus(frame_buf + 2, 1 + wire_len);
        if (wire_crc_main == calc_crc_main) {
          frame_ok = true;
          total = total_main;
          wire_crc = wire_crc_main;
          calc_crc = calc_crc_main;
        } else {
          wire_crc = wire_crc_main;
          calc_crc = calc_crc_main;
        }
      }

      if (!frame_ok && total_legacy_ok && frame_len >= total_legacy) {
        const uint16_t wire_crc_legacy = static_cast<uint16_t>(frame_buf[total_legacy - 2]) |
                                         (static_cast<uint16_t>(frame_buf[total_legacy - 1]) << 8U);
        // Legacy format: LEN means PAYLOAD only, CRC covers LEN+TYPE+PAYLOAD.
        const uint16_t calc_crc_legacy = crc16_modbus(frame_buf + 2, 2 + wire_len);
        if (wire_crc_legacy == calc_crc_legacy) {
          frame_ok = true;
          total = total_legacy;
          wire_crc = wire_crc_legacy;
          calc_crc = calc_crc_legacy;
        } else if (!total_main_ok) {
          wire_crc = wire_crc_legacy;
          calc_crc = calc_crc_legacy;
        }
      }

      // NORMAL mode dual-length parse: if main-length CRC failed, and legacy
      // length is still possible but not complete yet, wait for one more byte.
      if (!frame_ok && total_main_ok && total_legacy_ok && frame_len < total_legacy) {
        continue;
      }

      if (!frame_ok) {
        statsAdd(&g_crc_errors);
        const uint32_t now = millis();
        const bool allow_crc_log =
            elapsedSince(now, g_last_crc_error_log_ms,
                         static_cast<uint32_t>(BRIDGE_CRC_ERROR_LOG_INTERVAL_MS));
        if (allow_crc_log) {
          Serial.printf("[BRIDGE] crc error count=%llu\n",
                        static_cast<unsigned long long>(statsRead(&g_crc_errors)));
          g_last_crc_error_log_ms = now;
        }
#if OTA_TRACE_ACK
        if (bridgeIsOtaMode()) {
          Serial.printf("[BRIDGE][OTA] uart crc fail h0=0x%02X h1=0x%02X wire_len=%u rx_len=%u\n",
                        frame_buf[0],
                        (frame_len > 1U) ? frame_buf[1] : 0U,
                        static_cast<unsigned>(wire_len),
                        static_cast<unsigned>(frame_len));
          if (!elapsedSince(now,
                            g_ota_last_crc_error_ms,
                            static_cast<uint32_t>(BRIDGE_OTA_CRC_ERROR_STREAK_WINDOW_MS))) {
            ++g_ota_crc_error_streak;
          } else {
            g_ota_crc_error_streak = 1;
          }
          g_ota_last_crc_error_ms = now;
          Serial.printf("[BRIDGE][OTA] uart crc fail streak=%lu/%u\n",
                        static_cast<unsigned long>(g_ota_crc_error_streak),
                        static_cast<unsigned>(BRIDGE_OTA_CRC_ERROR_STREAK_LIMIT));
          if (g_ota_crc_error_streak >= static_cast<uint32_t>(BRIDGE_OTA_CRC_ERROR_STREAK_LIMIT)) {
            bridgeLeaveOtaMode("parse_error_streak");
          }
        }
#endif
#if BRIDGE_CRC_DEBUG_DUMP_ENABLE
        {
          if (allow_crc_log &&
              elapsedSince(now, g_last_crc_dump_ms,
                           static_cast<uint32_t>(BRIDGE_CRC_DEBUG_DUMP_INTERVAL_MS))) {
            const size_t dump_n = (frame_len < BRIDGE_CRC_DEBUG_DUMP_BYTES) ? frame_len : BRIDGE_CRC_DEBUG_DUMP_BYTES;
            Serial.printf("[BRIDGE] crc dbg frame_len=%u wire_len=%u wire_crc=0x%04X calc_crc=0x%04X dump_n=%u\n",
                          static_cast<unsigned>(frame_len),
                          static_cast<unsigned>(wire_len),
                          static_cast<unsigned>(wire_crc),
                          static_cast<unsigned>(calc_crc),
                          static_cast<unsigned>(dump_n));
            Serial.print("[BRIDGE] crc dbg bytes=");
            dumpBytesHex(frame_buf, dump_n);
            g_last_crc_dump_ms = now;
          }
        }
#endif

        // Resync by scanning next possible header.
        bool shifted = false;
        for (size_t i = 1; i + 1 < frame_len; ++i) {
          if (frame_buf[i] == kHeader1 && frame_buf[i + 1] == kHeader2) {
            memmove(frame_buf, frame_buf + i, frame_len - i);
            frame_len -= i;
            shifted = true;
            break;
          }
        }

        if (!shifted) {
          frame_len = 0;
        }
        continue;
      }

      statsAdd(&g_valid_frames);
      g_ota_crc_error_streak = 0;
      g_ota_last_crc_error_ms = 0;
#if OTA_TRACE_ACK
      if (bridgeIsOtaMode()) {
        const size_t dump_n = (total < 8U) ? total : 8U;
        Serial.printf("[BRIDGE][OTA] uart frame ok type=0x%02X len=%u mode=%s q=%u\n",
                      frame_buf[3],
                      static_cast<unsigned>(total),
                      bridgeIsOtaMode() ? "OTA" : "NORMAL",
                      static_cast<unsigned>(uxQueueMessagesWaiting(g_frame_q)));
        Serial.print("[BRIDGE][OTA] uart frame head=");
        dumpBytesHex(frame_buf, dump_n);
        if (isAckLikeType(frame_buf[3])) {
          const uint16_t ack_seq = (total >= 6U) ? readU16Le(&frame_buf[4]) : 0U;
          statsAdd(&g_ack_like_uart_seen);
          Serial.printf("[BRIDGE][OTA] uart ack-like frame type=0x%02X len=%u\n",
                        frame_buf[3],
                        static_cast<unsigned>(total));
          Serial.printf("[BRIDGE][OTA] uart ack-like seq=0x%04X t=%lu\n",
                        static_cast<unsigned>(ack_seq),
                        static_cast<unsigned long>(millis()));
        }
      } else
#endif
      {
#if BRIDGE_NORMAL_FRAME_LOG_ENABLE
        Serial.printf("[BRIDGE] frame ok type=%u len=%u queue=%u\n",
                      frame_buf[3],
                      static_cast<unsigned>(total),
                      static_cast<unsigned>(uxQueueMessagesWaiting(g_frame_q)));
#endif
      }

      if (bridgeIsOtaMode()) {
        const uint8_t frame_type = frame_buf[3];
        if (!isOtaRespType(frame_type)) {
#if OTA_TRACE_ACK
          Serial.printf("[BRIDGE][OTA] drop uart->tcp non-ota type=0x%02X len=%u\n",
                        frame_type,
                        static_cast<unsigned>(total));
#endif
          frame_len = 0;
          continue;
        }
        if (!enqueueFrameNoDrop(frame_buf, total)) {
          Serial.println("[BRIDGE] ota mode queue full, request tcp close");
          bridgeLeaveOtaMode("queue_overflow");
          requestTcpClose();
        }
      } else {
        enqueueFrameDropOldest(frame_buf, total);
        if (uxQueueSpacesAvailable(g_frame_q) == 0) {
          onQueueOverflow("uart_rx_queue_full");
        }
      }

      frame_len = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  kUart.setRxBufferSize(4096);
  kUart.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  g_frame_q = xQueueCreate(FRAME_QUEUE_DEPTH, sizeof(FramePacket));
  g_tcp_lock = xSemaphoreCreateMutex();

  Serial.println("[BRIDGE] boot");

  if (g_frame_q == nullptr || g_tcp_lock == nullptr) {
    Serial.println("[BRIDGE] fatal: queue or mutex create failed");
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  bridgeLeaveOtaMode("boot");

  WiFi.mode(WIFI_STA);
  WiFi.onEvent(onWiFiEvent);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  xTaskCreatePinnedToCore(networkTask, "NetworkTask", 8192, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(uartTask, "UartTask", 6144, nullptr, 2, nullptr, 1);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
