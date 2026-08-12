# EdgeOTA Demo Guide

## Build
```bash
cmake -S . -B build
cmake --build build -j
```

## End-to-end local demo (STM32 OTA simulated)
```bash
./scripts/run_ota_local_demo.sh
```

The script will:
1. Build binaries.
2. Create sample firmware package.
3. Register firmware into `data/firmware/`.
4. Start `fake_bootloader`.
5. Launch OTA task via `otactl`.
6. Print task list and final state.

## Manual commands
```bash
./build/otactl firmware list
./build/otactl ota list
./build/otactl ota status --task <task_uuid>
./build/otactl ota events --task <task_uuid>
```

## HTTP API demo
Create OTA task:
```bash
curl -X POST http://127.0.0.1:9010/api/ota/tasks \
  -H 'Content-Type: application/json' \
  -d '{"device_id":1,"device_type":"stm32f407-smarthome","firmware_id":"stm32f407-smarthome-1.0.1","target":"127.0.0.1:19090"}'
```

Query task and events:
```bash
curl http://127.0.0.1:9010/api/ota/tasks
curl http://127.0.0.1:9010/api/ota/tasks/<task_uuid>
curl http://127.0.0.1:9010/api/ota/tasks/<task_uuid>/events
```

Cancel and retry:
```bash
curl -X POST http://127.0.0.1:9010/api/ota/tasks/<task_uuid>/cancel
curl -X POST http://127.0.0.1:9010/api/ota/tasks/<task_uuid>/retry
```
