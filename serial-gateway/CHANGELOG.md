# Changelog

## v0.4.0 - P4 completion

### Added
- Multi-serial instance bootstrap from config (`serials`) with per-instance isolation:
  - auto cache-file suffixing
  - auto MQTT client-id suffixing
  - instance-tagged logs and monitor status output
- Tracked downlink command flow with ACK wait/timeout:
  - `TRACK_HEX` / `TRACK_TEXT`
  - pending command correlation by `device_id + command_id`
- Deployment/ops guide: `docs/deployment.md`
- Fault injection script: `scripts/run_fault_injection.sh`
- Stress test script: `scripts/run_stress_test.sh`
- Monitor API extensions:
  - `GET /api/devices`
  - `GET /metrics` (Prometheus text format)
- `test_device_registry` unit test.
- Multi-instance monitor enhancement:
  - monitor port auto-offset per instance (`base + index`)
  - `tests/multi_instance_smoke.sh`
  - `scripts/collect_multi_instance_status.sh`
  - `scripts/collect_multi_instance_status.sh --watch` continuous sampling mode
  - `scripts/collect_multi_instance_status.sh --csv-file` CSV summary export
  - `scripts/multi_instance_fault_injection.sh` isolation fault verification (tcp/mqtt mode + recovery phase)

### Changed
- Protocol parser now supports extended frame types (`SENSOR_DATA`, `ALARM_EVENT`, `HEARTBEAT`, `COMMAND_ACK`, `DEVICE_STATUS`) and unknown-type tolerance.
- Runtime metrics expanded with parser/upload/cache/device/reconnect dimensions and Prometheus-style key naming in logs.
- Device online/offline registry integrated with timeout transitions and structured status visibility.

### Verified
- `cmake --build build -j` pass.
- `ctest --test-dir build --output-on-failure` pass.

## v0.3.0 - P3 completion

### Added
- Process resource usage metrics (`rss_kb`, `vms_kb`, `user_cpu_ms`, `sys_cpu_ms`, `threads`) in status logs and heartbeat payloads.
- ARM cross-compilation toolchain preset (`cmake/toolchains/arm-linux-gnueabihf.cmake`).
- `test_resource_usage` for runtime resource sampling checks.
- systemd env template support (`serial-gateway.env.example`).
- systemd resource limit helper script (`scripts/set_systemd_limits.sh`).

### Changed
- systemd unit now loads optional env file (`/etc/serial-gateway/serial-gateway.env`) and sets baseline hardening options (`NoNewPrivileges`, `LimitNOFILE`).
- install/uninstall scripts now manage env template and drop-in cleanup.
- CMake install rules now include env template and toolchain file.

### Verified
- `ctest` all pass.
- `heartbeat_smoke.sh` pass.
- install staging verification pass.

## v0.2.0 - P2 completion

### Added
- Real MQTT backend integration path via libmosquitto (with placeholder fallback when unavailable).
- TCP command server for serial downlink (`HEX` / `TEXT`) and command sender tool.
- Gateway heartbeat thread and payload.
- Hot reload support via `SIGHUP` and optional file polling.

### Added tests
- `mqtt_smoke.sh`
- `command_smoke.sh`
- `heartbeat_smoke.sh`
- `reload_smoke.sh`

## v0.1.0 - MVP + P1

### Added
- Multithread serial ingest, frame parser, TCP uploader.
- Config, logging, runtime stats.
- Auto reconnect and retry.
- Persistent disk cache for failed uploads with replay.
- Multi-device mapping and unknown-device handling.
- Local demo tools (`fake_sensor`, `tcp_receiver`, `pty_bridge`).

### Added tests
- `test_protocol`
- `test_config`
- `test_record_codec`
- `test_cached_uploader`
- `smoke.sh`
- `recovery.sh`
