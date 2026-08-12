# SmartHome Integration (Plan A)

## 1. Scope

This document describes the direct integration:

- Device side: `SmartHome` firmware outputs UART binary frames and handles downlink command frames.
- Gateway side: `serial-gateway` reads UART, uploads events, and sends tracked commands to device.

## 2. UART Contract

Frame format:

`AA 55 | LEN | TYPE | PAYLOAD | CRC16(modbus, LE)`

SmartHome uplink frame types:

- `0x01 SENSOR_DATA`
- `0x03 HEARTBEAT`
- `0x05 DEVICE_STATUS`
- `0x04 COMMAND_ACK`

Gateway downlink frame type:

- `0x10 COMMAND_REQ`

## 3. SmartHome Side

Current implementation:

- Task: `GatewayTask` on USART3, 115200 8N1 (`PB10 TX`, `PB11 RX`)
- Source file: `1_App/app_mqtt.c`
- Startup hook: `1_App/main.c` (`vStartGatewayTasks(...)`)

Supported downlink command types:

1. `0x01 set_led` (`arg: 0|1`)
2. `0x02 set_buzzer` (`arg: 0|1`)
3. `0x03 set_mode` (`arg: 0|1`)
4. `0x04 get_status`
5. `0x05 set_threshold` (`temp_x10(u16), humi_x10(u16)`)
6. `0x06 set_log_level` (`0:error, 1:warn, 2:info`)

## 4. Gateway Side

In `config/gateway.yaml`:

- `serial.device`: set to the actual UART device (for example `/dev/ttyUSB0`)
- `serial.baudrate`: `115200`
- `devices.id`: include the SmartHome device id (`1` by default)

Build and run:

```bash
cmake -S . -B build
cmake --build build -j
./build/serial_gateway --config=config/gateway.yaml
```

## 5. Command Bring-up

`command_sender` supports `DEVCMD` mode and auto-builds command frames + CRC:

```bash
./build/command_sender 127.0.0.1 9001 DEVCMD 1 100 2000 set_led 1
./build/command_sender 127.0.0.1 9001 DEVCMD 1 101 2000 set_mode 1
./build/command_sender 127.0.0.1 9001 DEVCMD 1 102 2000 set_threshold 35.0 80.0
./build/command_sender 127.0.0.1 9001 DEVCMD 1 103 2000 get_status
./build/command_sender 127.0.0.1 9001 DEVCMD 1 104 2000 set_log_level 2
```

## 6. Validation Checklist

1. `serial_gateway` logs show parsed `sensor_data` and `heartbeat`.
2. `command_sender ... DEVCMD ...` returns `OK ack ...`.
3. `GET /api/devices` shows device online.
4. `GET /api/recent` includes `event_type` updates after command execution.
