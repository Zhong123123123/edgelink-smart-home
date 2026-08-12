# EdgeOTA Design

## Scope
EdgeOTA extends `serial-gateway` into a firmware lifecycle manager for heterogeneous edge nodes:
- STM32 serial nodes (binary OTA transport).
- ESP32 WiFi nodes (JSON command + URL OTA).

## Architecture

### Gateway modules
- `FirmwareStore`: firmware package validation and repository (`data/firmware`).
- `OtaTaskManager`: task CRUD/state transition/event persistence.
- `OtaStateMachine`: unified lifecycle model across device types.
- `Stm32OtaAdapter`: AA55 frame OTA with seq/offset/retry/ACK/NACK.
- `Esp32OtaAdapter`: `ota_start` generation and `ota_status` tracking.
- `PersistentStore`: JsonFileStore implementation with SQLite-compatible interface naming.
- `Ota metrics`: `ota_*` series exposed via `/metrics`.

### Device side
- STM32 APP side adds `REBOOT_TO_BOOTLOADER` command handling and delayed `APP_CONFIRM` report.
- STM32 bootloader skeleton includes partition constants, metadata CRC validation and app jump skeleton.
- ESP32 side includes `ota_start` handling, `ota_status` reporting, and `ESP32_OTA_REAL=0/1` mode.

## Unified OTA Task States
`CREATED -> WAIT_DEVICE_ONLINE -> PRECHECK -> PREPARE_DEVICE -> TRANSFERRING -> VERIFYING -> COMMITTING -> REBOOTING -> VERSION_CHECK -> HEALTH_CONFIRM -> SUCCESS`

Error exits: `FAILED | ROLLBACK | CANCELED`

## Three Links
1. STM32 serial OTA: `AA55 + LEN + TYPE + PAYLOAD + CRC16` with ACK/NACK and retransmit.
2. ESP32 WiFi OTA: gateway `ota_start` -> node `ota_status` progress/terminal state.
3. Gateway state machine: transport-independent task/event/metrics model.

## Data Flow
1. Register firmware package (`manifest.json` + image) into `data/firmware/{firmware_id}`.
2. Create OTA task via CLI or HTTP API.
3. Gateway asynchronously dispatches to STM32/ESP32 adapter by `device_type`.
4. Adapter progress and transitions are persisted as task events.
5. API and `/metrics` expose runtime/result observability.

## Error Handling
- Adapter-level failure writes `FAILED` with `last_error`.
- HTTP/CLI supports `cancel` and `retry`.
- Cooperative cancel flag interrupts in-flight STM32/ESP32 adapter loops.
- Failure injection supported via `fake_bootloader` (NACK/disconnect/verify fail).

## Current Status (Implemented)
- Firmware repository, manifest parsing, CRC32 validation.
- Unified task state machine and persistent task/event storage.
- `otactl`: firmware add/list, ota start/list/status/events/cancel/retry.
- STM32 fake OTA e2e via `fake_bootloader`.
- ESP32 OTA adapter JSON handling.
- HTTP OTA APIs: list/get/events/create/cancel/retry.
- OTA metrics exposure in `/metrics`.

## Pending / Hardware Validation
- STM32 bootloader real flash erase/write/verify/rollback on hardware.
- ESP32 real OTA (`ESP32_OTA_REAL=1`) full field validation in hardware network.
- HTTP direct firmware file serving (`/fw/{firmware_id}/app.bin`) integration in monitor server.

## Non-Exaggeration Boundary
- Current gateway OTA execution path is production-shaped but still uses simulation tooling for most validation.
- Bootloader and dual-slot rollback logic is structurally present, not yet hardware-closed-loop verified.
