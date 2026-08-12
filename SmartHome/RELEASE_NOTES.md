# SmartHome Release Notes

## Version

- Release: `v0.4.4`
- Date: `2026-04-23`
- Baseline: STM32F103 + FreeRTOS + ESP8266 + MQTT

## Summary

This project has been upgraded from a classroom LED/key demo into an interview-oriented smart home terminal with:

1. Unified runtime state management
2. Sensor collection task framework
3. Display task framework (UART simulation)
4. JSON-based MQTT protocol (with legacy command compatibility)
5. Alarm task and dual mode (manual/auto)
6. Centralized configuration and leveled logging
7. MQTT reconnect strategy with backoff and periodic WiFi reinit
8. Reconnect failure telemetry (`mqtt_reconnect_fail_count`)
9. Reduced log noise and improved serial readability
10. Runtime-adjustable log level via MQTT command

## v0.4.4 Highlights

1. Added runtime log level control API:
   - `Log_SetLevel(...)`
   - `Log_GetLevel()`
   - `Log_LevelToString(...)`
2. Logging macros switched from compile-time filtering to runtime filtering.
3. Added MQTT command `set_log_level`:
   - Supports integer level (`0/1/2`)
   - Supports string level (`error/warn/info`)
4. Added `log_level` field in status uplink payload.
5. Main boot log now prints current log level.

## v0.4.3 Highlights

1. Optimized MQTT RX logs to summary mode:
   - Log `topic + cmd` for JSON command packets
   - Log `topic + legacy_cmd` for old text commands
   - Log `topic + payload length` for unknown packets
2. Reduced parse-fail log payload length (truncated) to avoid serial flooding.

## v0.4.2 Highlights

1. Reduced sensor log frequency:
   - Keep state-change logs (`recovered`, `invalid`)
   - Keep heartbeat sample log only at interval
2. Reduced display refresh frequency:
   - Refresh on meaningful state changes
   - Keep periodic heartbeat refresh
3. Improved display line readability:
   - Compact fixed-format output
   - Short status tags (`AUTO/MAN`, `UP/DN`, `OK/BAD`)

## Existing Features (Cumulative)

### 1. Unified State

- Introduced `SmartHomeState` global state center
- Added thread-safe state read/write interfaces
- State shared by MQTT, Sensor, Alarm, Display, Key, LED tasks

### 2. Sensor Task

- Added `SensorTask`
- Layered sensor path: `app -> dev -> platform -> driver`
- Current driver uses DHT11 stub data with periodic failure injection

### 3. Display Task

- Added `DisplayTask`
- Layered display path: `app -> dev -> platform -> driver`
- Current display outputs status via UART

### 4. MQTT Protocol Upgrade

- Command path upgraded to JSON protocol
- Supported commands:
  1. `set_led`
  2. `set_buzzer`
  3. `set_mode`
  4. `set_threshold`
  5. `get_status`
  6. `set_log_level`
- Legacy compatibility retained:
  - `led on`
  - `led off`
- Added periodic state report JSON
- Added key event JSON report
- Added alarm event JSON report

### 5. Alarm and Mode

- Added `AlarmTask`
- Alarm conditions:
  1. Temperature above threshold
  2. Humidity above threshold
  3. Sensor invalid
- Added manual/auto mode behavior split
- Added key long-press (`>=2000ms`) mode switching

### 6. Config and Logging

- Added centralized config module (`config.h/.c`)
- Added leveled log macros (`LOG_INFO/WARN/ERROR`)

### 7. Reconnect Strategy

- Added MQTT reconnect loop on disconnect
- Added exponential backoff retry interval
- Added periodic WiFi reinit before reconnect attempts
- Added reconnect fail counter into state and status payload

## Current Task Set

Registered in `1_App/main.c`:

1. `MqttTask` (priority 10)
2. `AlarmTask` (priority 4)
3. `SensorTask` (priority 3)
4. `DisplayTask` (priority 2)
5. `KeyTask` (priority 2)
6. `LedTask` (priority 1)

## MQTT Topics (Default)

Defined in `config.h`:

1. Command downlink: `SH_TOPIC_CMD`
2. Status uplink: `SH_TOPIC_STATUS`
3. Key event uplink: `SH_TOPIC_KEY_EVENT`
4. Alarm uplink: `SH_TOPIC_ALARM`

## Known Limitations

1. Sensor and display drivers are currently stub/UART simulation implementations.
2. No persistent storage for alarm history yet.
3. Reconnect strategy is functional but can still be improved with advanced jitter/telemetry.

## Next Planned Items

1. Replace DHT11 stub with real timing-based driver implementation.
2. Add real buzzer device/driver path and bind alarm actions to hardware.
3. Add optional low-frequency diagnostic snapshot uplink for long-run stability analysis.
