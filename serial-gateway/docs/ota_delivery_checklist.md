# EdgeOTA Delivery Checklist

## Completed
- Linux gateway compiles and tests pass.
- OTA modules are integrated into build (`include/ota`, `src/ota`, `storage`, `tools`).
- Firmware packager available: `tools/package_firmware.py`.
- CLI available: `otactl` (firmware/task/status/events/cancel/retry).
- Fake bootloader available: `tools/fake_bootloader.cpp` executable.
- Simulated STM32 OTA success path works.
- Fault injection scripts available (`crc_fail`, `disconnect`, `batch`).
- OTA task query/event APIs available in monitor server.
- `/metrics` includes `ota_*` indicators.
- ESP32 sketch includes OTA command/status and config safety.
- STM32 bootloader skeleton includes partition/meta/jump/protocol constants.

## Tested
- `ctest --test-dir build --output-on-failure`: all tests pass.
- `scripts/run_ota_local_demo.sh`: simulated success.
- `scripts/run_ota_crc_fail.sh` and `scripts/run_ota_disconnect.sh`: failure paths.

## Pending Hardware Validation
- STM32 real flash write/verify and rollback decision on board.
- ESP32 real HTTP Update API path with real firmware image (`ESP32_OTA_REAL=1`).
- End-to-end gateway + physical serial + WiFi node soak test.

## Interview Highlights
- Unified OTA state machine across heterogeneous transport/protocols.
- Serial OTA transport design: frame structure, seq/offset, ACK/NACK, retries.
- Lifecycle persistence and restart-resilient task/event modeling.
- Observability integration: API + metrics + fault injection scripts.
- Engineering boundary control: simulated proof chain vs hardware truth separation.

## Do Not Overstate
- Do not claim production-hardened bootloader rollback until board-level validation is complete.
- Do not claim ESP32 real OTA readiness unless `ESP32_OTA_REAL=1` path is hardware verified.
