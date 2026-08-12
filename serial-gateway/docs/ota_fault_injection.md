# OTA Fault Injection

## Prerequisites
```bash
cmake -S . -B build
cmake --build build -j
```

## Scenario 1: Verify/CRC failure
```bash
./scripts/run_ota_crc_fail.sh
```
Mechanism: `fake_bootloader --verify-fail` returns NACK on verify phase.
Expected: task enters `FAILED`; events show verify failure reason.

## Scenario 2: Mid-transfer disconnect
```bash
./scripts/run_ota_disconnect.sh
```
Mechanism: `fake_bootloader --disconnect-at-seq 3` drops connection during `OTA_DATA`.
Expected: task enters `FAILED`; error contains connect/ack/timeout context.

## Scenario 3: Batch fault suite
```bash
./scripts/run_ota_fault_injection.sh
```
Runs multiple fault scripts sequentially for quick regression.

## Scenario 4: Manual cancel during transfer
1. Create a task with HTTP or `otactl ota start`.
2. Before completion, call:
```bash
curl -X POST http://127.0.0.1:9010/api/ota/tasks/<task_uuid>/cancel
```
Expected: cooperative cancellation, task transitions to `CANCELED`.

## Observe Results
- Task summary: `./build/otactl ota list`
- Task details: `./build/otactl ota events --task <task_uuid>`
- Metrics: `curl http://127.0.0.1:9010/metrics | rg '^ota_'`
- Fake logs: `tmp_demo/fake_bl*.log`

## Coverage Note
Current injection suite fully covers gateway-side protocol/timeout/retry/cancel behavior.
Hardware-specific flash integrity and rollback physics still require STM32 target validation.
