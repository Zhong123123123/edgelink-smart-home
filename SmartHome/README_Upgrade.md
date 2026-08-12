# SmartHome Upgrade Guide

## 1. Project Positioning

This project is upgraded from a demo into a resume-oriented embedded system:

`STM32F103 + FreeRTOS + Serial Gateway`

Target: Smart home monitoring and alarm terminal with cloud interaction.

## 2. Current Task Topology

Registered in `1_App/main.c`:

1. `GatewayTask` (priority 10): UART frame rx/tx for `serial-gateway`
2. `AlarmTask` (priority 4): threshold check and auto-mode control
3. `SensorTask` (priority 3): periodic sensor collection (stub now)
4. `DisplayTask` (priority 2): periodic status display (UART simulation)
5. `KeyTask` (priority 2): key event upload + long-press mode switch
6. `LedTask` (priority 1): actuator execution by task notify

## 3. State and Config

- Unified state: `smarthome_state.h/.c`
- Runtime config: `config.h/.c`
- Log macro: `log.h`

Important runtime config fields:

- `sensor_period_ms`
- `display_period_ms`
- `mqtt_report_period_ms`
- `alarm_check_period_ms`
- `wifi_reconnect_period_ms`
- `mqtt_reconnect_period_ms`

## 4. Gateway Serial Protocol

Frame format:

`AA 55 | LEN | TYPE | PAYLOAD | CRC16(modbus, LE)`

Uplink frames currently used:

- `0x01 SENSOR_DATA`
- `0x03 HEARTBEAT`
- `0x05 DEVICE_STATUS`

Downlink command:

- `0x10 COMMAND_REQ`

Ack:

- `0x04 COMMAND_ACK`

`COMMAND_REQ` payload:

- `device_id (u8)`
- `command_id (u16, LE)`
- `command_type (u8)`
- `command_args (...)`

Supported `command_type`:

1. `0x01 set_led` (`arg: 0|1`)
2. `0x02 set_buzzer` (`arg: 0|1`)
3. `0x03 set_mode` (`arg: 0|1`)
4. `0x04 get_status`
5. `0x05 set_threshold` (`temp_x10(u16), humi_x10(u16)`)
6. `0x06 set_log_level` (`0:error,1:warn,2:info`)

`COMMAND_ACK` result codes:

- `0` OK
- `1` BAD_PAYLOAD
- `2` UNKNOWN_CMD

## 5. Legacy MQTT Topics (Code Kept)

Default topics in `config.h`:

- Command downlink: `SH_TOPIC_CMD`
- Status uplink: `SH_TOPIC_STATUS`
- Key event uplink: `SH_TOPIC_KEY_EVENT`
- Alarm uplink: `SH_TOPIC_ALARM`

## 6. Legacy MQTT Payload Protocol

### 5.1 Status uplink

```json
{
  "temperature": 26.5,
  "humidity": 61.0,
  "led_on": 1,
  "alarm_on": 0,
  "mode": "auto",
  "wifi_connected": 1,
  "mqtt_connected": 1
}
```

### 5.2 Key event uplink

```json
{
  "event": "key",
  "num": 1,
  "press_ms": 88
}
```

### 5.3 Alarm uplink

```json
{
  "alarm_on": 1,
  "temperature": 36.2,
  "humidity": 82.0
}
```

### 5.4 Command downlink

Supported commands:

1. `set_led`
2. `set_buzzer`
3. `set_mode`
4. `set_threshold`
5. `get_status`
6. `set_log_level`

Examples:

```json
{"cmd":"set_led","value":1}
{"cmd":"set_buzzer","value":0}
{"cmd":"set_mode","value":1}
{"cmd":"set_threshold","temperature_high":33.5,"humidity_high":75.0}
{"cmd":"get_status"}
{"cmd":"set_log_level","value":1}
{"cmd":"set_log_level","level":"error"}
```

Legacy compatibility retained:

- `led on`
- `led off`

## 7. Mode Behavior

- `manual`: cloud/key controls apply directly, no forced auto-action
- `auto`: `AlarmTask` controls LED/Buzzer by threshold and sensor validity

Key long-press (`>=2000ms`) toggles mode.

## 8. Reconnect Strategy (Legacy MQTT Path)

In `app_mqtt.c`:

1. If MQTT disconnected, retry connect+subscribe periodically
2. Retry interval uses exponential backoff (capped)
3. WiFi is reinitialized periodically before reconnect attempts

## 9. Build Integration Checklist

1. Add new source files into Keil project groups
2. Ensure include paths cover `1_App/2_Device/5_Platform/6_ModuleDrives`
3. Rebuild all after config struct changes

## 10. Bring-up Checklist

1. Boot logs include `MAIN`, `NET`, `MQTT`, `SENSOR`, `DISPLAY`, `ALARM`
2. Receive periodic status uplink
3. Send downlink command and verify actuator/state response
4. Trigger threshold and verify alarm uplink and auto-mode behavior
5. Disconnect network and verify reconnect logs and recovery
