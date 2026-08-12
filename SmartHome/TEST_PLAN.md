# SmartHome Test Plan

## Version

- Scope: `v0.4.1`
- Date: `2026-04-23`

## 1. Build Check

0. Run repository preflight checks:
   - `./scripts/preflight_check.sh`
1. Add all newly introduced source files into project groups.
2. Rebuild full project after `SmartHomeState` struct updates.
3. Confirm no undefined reference for:
   - `vStartSensorTasks`
   - `vStartDisplayTasks`
   - `vStartAlarmTasks`
   - `Mqtt_RequestImmediateReport`

## 2. Boot and Task Startup

1. Power on board and open UART log.
2. Expect startup logs for modules:
   - `MAIN`
   - `NET`
   - `MQTT`
   - `SENSOR`
   - `DISPLAY`
   - `ALARM`
3. Confirm scheduler runs and display line prints periodically.

## 3. Sensor and Display

1. Observe periodic display output includes:
   - Temperature/Humidity/Light
   - Mode/LED/Alarm
   - WiFi/MQTT state
   - `MFAIL` (mqtt reconnect fail count)
2. Verify sensor stub occasionally reports invalid state path and alarm logic can react.

## 4. MQTT Protocol Uplink

1. Subscribe cloud client to:
   - `SH_TOPIC_STATUS`
   - `SH_TOPIC_KEY_EVENT`
   - `SH_TOPIC_ALARM`
2. Confirm status uplink fields include:
   - `temperature`
   - `humidity`
   - `led_on`
   - `alarm_on`
   - `mode`
   - `wifi_connected`
   - `mqtt_connected`
   - `mqtt_reconnect_fail_count`
3. Press key and verify key event JSON uplink.

## 5. MQTT Protocol Downlink

Send commands to `SH_TOPIC_CMD`:

1. `{"cmd":"set_led","value":1}`
2. `{"cmd":"set_led","value":0}`
3. `{"cmd":"set_mode","value":1}`
4. `{"cmd":"set_mode","value":0}`
5. `{"cmd":"set_threshold","temperature_high":33.5,"humidity_high":75.0}`
6. `{"cmd":"get_status"}`
7. `{"cmd":"set_log_level","value":1}`
8. `{"cmd":"set_log_level","level":"error"}`

Expected:

1. State changes reflected in display and status uplink.
2. `get_status` triggers immediate status publish.
3. Legacy command compatibility remains:
   - `led on`
   - `led off`

## 6. Alarm and Mode Behavior

1. Force threshold crossing (by lowering threshold values).
2. Verify `alarm_on` becomes 1 and alarm topic publishes event.
3. In auto mode:
   - LED follows alarm status automatically.
   - Buzzer state flag follows alarm status.
4. In manual mode:
   - Alarm status updates, but no forced actuator override.

## 7. Key Long-Press Mode Switch

1. Long-press key (`>=2000ms`).
2. Confirm mode toggles manual/auto.
3. Confirm immediate status report after mode change.

## 8. Reconnect Behavior

1. Start connected state and verify `mqtt_connected=1`.
2. Disconnect network/router temporarily.
3. Verify:
   - MQTT disconnect detected (`mqtt_connected=0`)
   - Reconnect attempts happen with backoff
   - `MFAIL` increments on failures
4. Restore network.
5. Verify:
   - Reconnect success (`mqtt_connected=1`)
   - `MFAIL` resets to 0
   - Status uplink resumes

## 9. Pass Criteria

1. All core tasks run without blocking/reset loops.
2. JSON downlink commands are parsed and executed.
3. Status/Key/Alarm uplink works as expected.
4. Alarm logic and mode switching are consistent.
5. Reconnect path recovers automatically after network restoration.
6. `./scripts/preflight_check.sh` passes before release packaging.

## 10. FreeRTOS Runtime Health Check (60s)

1. Observe UART logs for periodic diagnostics:
   - `GATEWAY diag stack_hwm=...`
   - `SENSOR diag stack_hwm=...`
   - `ALARM diag stack_hwm=...`
   - `DISPLAY diag stack_hwm=...`
2. Confirm there is no:
   - `RTOS stack overflow task=...`
   - `RTOS malloc failed free_heap=...`
3. On gateway monitor, count frames in a 60s window:
   - `device_id=1,event_type=sensor_data` should be about `20` (period 3000ms)
   - `device_id=1,event_type=heartbeat` should be about `6` (period 10000ms)
4. If `sensor_data` count is lower than expected, check jitter warning:
   - `sensor report jitter=... target=3000`
5. Record final result:
   - pass/fail with snapshot logs and 60s counters.

### 10.1 Regression Result (2026-05-06)

- Verdict: `PASS`
- Evidence pack: `/tmp/sg_final_pack`

1. 60s counter and CRC
- `sensor_data_60s=20` (target about 20/min, pass)
- `heartbeat_60s=6` (target about 6/min, pass)
- `sg_frames_crc_fail_total{instance="primary"} 0` (pass)
- Evidence:
  - `/tmp/sg_final_pack/counter_60s.txt`
  - `/tmp/sg_final_pack/crc_check.txt`

2. Command stress (`set_led`/`set_mode`/`get_status`, 20 times each)
- Total `OK ack`: `60`
- Total `ERR`: `0`
- `ack_avg_ms=191.033`
- `ack_max_ms=216`
- `ack_ok=60`
- Conclusion: no ACK loss, stable latency
- Evidence:
  - `/tmp/sg_final_pack/cmd_stress.log`
  - `/tmp/sg_final_pack/cmd_latency_stats.txt`

3. Recovery check (gateway down 30s then resume)
- After recovery: `sg_frames_rx_total{instance="primary"} 11`
- Conclusion: gateway continues receiving frames after service recovery
- Evidence:
  - `/tmp/sg_final_pack/metrics_recover.txt`
  - `/tmp/sg_final_pack/recover_check.txt`

4. FreeRTOS health logs
- Gateway-side log grep: no `stack overflow` / `malloc failed`
- STM32 UART diagnostic lines captured:
  - `GATEWAY diag stack_hwm=396 report_ms=3000 hb_ms=10000`
  - `ALARM diag stack_hwm=103 period_ms=500`
  - `SENSOR diag stack_hwm=110 period_ms=2000`
  - `DISPLAY diag stack_hwm=49 period_ms=1000`
- Evidence:
  - `/tmp/sg_final_pack/rtos_health_from_gateway.txt`
  - STM32 UART monitor screenshot/log capture (local session)
