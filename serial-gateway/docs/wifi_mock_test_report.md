# WiFi Mock Node E2E Test Report

## Scope
- Test date: 2026-04-30
- Validation type: **mock only** (`tools/mock_wifi_node`)
- Explicitly not covered: ESP32 physical hardware validation

## Environment
- Gateway config used: `/tmp/sg_wifi_mock/gateway_wifi_mock.yaml`
- Virtual serial pair: `/tmp/ttyV0 <-> /tmp/ttyV1` via `build/pty_bridge`
- Log directory: `/tmp/sg_wifi_mock`

## Repro Commands
```bash
cmake --build build -j4
bash tools/integration/test_wifi_node.sh
```

## Test Goals and Results
1. Start `tcp_receiver` on 9000: **PASS**
2. Start `serial_gateway` and verify WiFi server listens on 9100: **PASS**
3. Start `mock_wifi_node` with `device_id=2` and connect 9100: **PASS**
4. Verify periodic `sensor_data` and `heartbeat`: **PASS**
5. Verify `tcp_receiver` receives `device_id=2`, `device_name=wifi-node-02`, `link_type=wifi`: **PASS**
6. Use `command_sender` to send `get_status` and `set_led` to `device_id=2`: **PASS**
7. Verify WiFi command path + `command_ack result=0`: **PASS**
8. Verify `/api/devices` includes `device_id=2`, `link_type=wifi`, `online=true`, `wifi_connected=true`, `wifi_rssi` value: **PASS**
9. Verify metrics include `wifi_clients_connected=1`, `wifi_devices_online=1`, and `devices_online` including WiFi node: **PASS**
10. Produce logs and this report: **PASS**

## Key Log Evidence

### A. Gateway startup and WiFi listener
From `/tmp/sg_wifi_mock/gateway.log`:
```text
2026-04-30 09:58:36 [INFO] app started, instance=primary serial=/tmp/ttyV0 uploader.type=tcp
2026-04-30 09:58:36 [INFO] instance=primary command server listening on 127.0.0.1:9001
2026-04-30 09:58:36 [INFO] wifi device server listening on 0.0.0.0:9100
2026-04-30 09:58:37 [INFO] wifi client connected from 127.0.0.1
2026-04-30 09:58:37 [INFO] instance=primary device online device_id=2 name=wifi-node-02
```

### B. tcp_receiver payloads (sensor_data/heartbeat/ack)
From `/tmp/sg_wifi_mock/tcp_receiver.log`:
```text
{"device_id":2,"device_name":"wifi-node-02","link_type":"wifi","event_type":"sensor_data",...}
{"device_id":2,"device_name":"wifi-node-02","link_type":"wifi","event_type":"heartbeat",...}
{"device_id":2,"device_name":"wifi-node-02","link_type":"wifi","event_type":"command_ack","command_id":301,"command_result":0,...}
{"device_id":2,"device_name":"wifi-node-02","link_type":"wifi","event_type":"command_ack","command_id":302,"command_result":0,...}
```

### C. Command responses (`command_sender`)
From `/tmp/sg_wifi_mock/cmd_get_status.txt` and `/tmp/sg_wifi_mock/cmd_set_led.txt`:
```text
OK ack device_id=2 command_id=301 result=0
OK ack device_id=2 command_id=302 result=0
```

### D. Monitor API `/api/devices`
From `/tmp/sg_wifi_mock/devices.json`:
```json
[{"device_id":2,"device_name":"wifi-node-02",...,"online":true,"link_type":"wifi","wifi_rssi":-65,"wifi_connected":true,...}, ...]
```

### E. Metrics
From `/tmp/sg_wifi_mock/metrics.txt`:
```text
sg_devices_online{instance="primary"} 1
sg_wifi_clients_connected{instance="primary"} 1
sg_wifi_devices_online{instance="primary"} 1
```

## Notes
- This test does not modify STM32 UART protocol behavior.
- This test does not change the `device_id=1` serial chain logic; serial node stays configured and isolated from WiFi path verification.

## Reliability Extension (Mock Only)
- Added reliability scripts under `tools/integration/`:
  - `test_wifi_ack_cases.sh`
  - `test_wifi_reconnect.sh`
  - `test_wifi_json_robustness.sh`
  - `test_command_key_isolation.sh`
  - `test_multi_transport_mock.sh`
  - `test_wifi_weak_network_cache.sh`
  - `test_wifi_stability_smoke.sh`
  - `run_wifi_reliability_suite.sh`
- Added mock serial source `tools/mock_serial_node.cpp` for multi-transport verification.
- All above remain mock-based gateway validation and do not represent ESP32 hardware acceptance.

## Reliability Suite Run (2026-04-30)
- Command:
```bash
bash tools/integration/run_wifi_reliability_suite.sh
```
- Log directory: `/tmp/sg_wifi_reliability`
- Result:
  - wifi_node_e2e: PASS
  - wifi_ack_cases: PASS
  - wifi_reconnect: PASS
  - wifi_json_robustness: PASS
  - command_key_isolation: PASS
  - multi_transport_mock: PASS
  - wifi_weak_network_cache: PASS
  - wifi_stability_smoke: SKIPPED

Note: all results above are mock-based and not ESP32 physical hardware validation.
