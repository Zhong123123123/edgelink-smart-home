# EdgeOTA Protocol

## STM32 Binary OTA Frame
Common frame:
- `HEAD`: `0xAA 0x55`
- `LEN`: `TYPE + PAYLOAD` bytes
- `TYPE`: 1 byte
- `PAYLOAD`: variable
- `CRC16`: Modbus CRC16 over `LEN+TYPE+PAYLOAD` (little-endian)

Upstream (`device -> gateway`):
- `0x20 BOOT_HELLO`
- `0x21 VERSION_REPORT`
- `0x22 OTA_ACK`
- `0x23 OTA_NACK`
- `0x24 OTA_PROGRESS`
- `0x25 BOOT_REPORT`
- `0x26 APP_CONFIRM`
- `0x27 CRASH_REPORT`

Downstream (`gateway -> device`):
- `0x30 OTA_QUERY`
- `0x31 OTA_PREPARE`
- `0x32 OTA_DATA`
- `0x33 OTA_VERIFY`
- `0x34 OTA_COMMIT`
- `0x35 OTA_ABORT`
- `0x36 REBOOT_TO_BOOTLOADER`
- `0x37 GET_VERSION`

Current simulated adapter payloads:
- `OTA_PREPARE`: `seq(u16), image_size(u32), image_crc32(u32), chunk_size(u16)`
- `OTA_DATA`: `seq(u16), offset(u32), chunk_len(u16), chunk_bytes`
- `OTA_VERIFY`: `seq(u16), image_crc32(u32)`
- `OTA_COMMIT`: `seq(u16)`
- `OTA_ACK/NACK`: `seq(u16)`

## ESP32 OTA JSON
`ota_start` command:
```json
{
  "type": "command",
  "command_id": 3001,
  "command_type": "ota_start",
  "device_id": 2,
  "firmware_url": "http://gateway:9080/fw/esp32-wifi-node-1.1.0/app.bin",
  "version": "1.1.0",
  "size": 1048576,
  "crc32": "0x12345678",
  "force": false
}
```

## HTTP OTA API (gateway monitor extension)
- `GET /api/ota/tasks`
- `GET /api/ota/tasks/{task_uuid}`
- `GET /api/ota/tasks/{task_uuid}/events`
- `POST /api/ota/tasks`
- `POST /api/ota/tasks/{task_uuid}/cancel`
- `POST /api/ota/tasks/{task_uuid}/retry`
- `GET /fw/{firmware_id}/app.bin`
- `GET /fw/{firmware_id}/manifest.json`

Create task body:
```json
{
  "device_id": 1,
  "device_type": "stm32f407-smarthome",
  "firmware_id": "stm32f407-smarthome-1.0.1",
  "target": "127.0.0.1:19090",
  "firmware_url": "http://gateway:9080/fw/esp32-wifi-node-1.1.0/app.bin"
}
```

`target` is optional. When omitted, gateway uses `127.0.0.1:19090` for STM32 OTA adapter execution.
`firmware_url` is optional for ESP32; if omitted, gateway derives `http://gateway:9080/fw/{firmware_id}/app.bin`.

`ota_status` event:
```json
{
  "device_id": 2,
  "event_type": "ota_status",
  "command_id": 3001,
  "ota_state": "downloading",
  "progress": 42,
  "version_from": "1.0.0",
  "version_to": "1.1.0",
  "error_code": 0,
  "seq": 1201
}
```
