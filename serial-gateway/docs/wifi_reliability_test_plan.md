# WiFi Reliability Test Plan (Mock Based)

## Scope
- This plan validates Linux Gateway WiFi-node integration using `tools/mock_wifi_node` and virtual serial.
- This is **not** ESP32 physical hardware validation.
- STM32 serial protocol is unchanged.

## Coverage
1. WiFi node E2E: `tools/integration/test_wifi_node.sh`
2. WiFi ACK cases (ok/fail/timeout/late): `tools/integration/test_wifi_ack_cases.sh`
3. WiFi reconnect behavior: `tools/integration/test_wifi_reconnect.sh`
4. WiFi JSON robustness: `tools/integration/test_wifi_json_robustness.sh`
5. Command key isolation (`device_id + command_id`): `tools/integration/test_command_key_isolation.sh`
6. Multi-transport mock (serial + wifi): `tools/integration/test_multi_transport_mock.sh`
7. Weak network + cache replay: `tools/integration/test_wifi_weak_network_cache.sh`
8. Stability smoke (default 300s): `tools/integration/test_wifi_stability_smoke.sh`

## Unified Entry
- Run all non-smoke cases:
```bash
tools/integration/run_wifi_reliability_suite.sh
```
- Include smoke case:
```bash
tools/integration/run_wifi_reliability_suite.sh --include-smoke
```

## Key Metrics
- Command tracking:
  - `sg_command_submit_count`
  - `sg_command_ack_count`
  - `sg_command_timeout_count`
  - `sg_command_fail_count`
  - `sg_command_late_ack_count`
  - `sg_command_success_rate`
- WiFi JSON/runtime:
  - `sg_wifi_json_parse_ok`
  - `sg_wifi_json_parse_fail`
  - `sg_wifi_unknown_device`
  - `sg_wifi_events_received`
  - `sg_wifi_clients_connected`
  - `sg_wifi_devices_online`

## Boundaries
- Mock tests verify gateway-side routing, tracking, caching, monitor, metrics, and error handling.
- ESP32 arrival will require real hardware verification for WiFi stack, RF behavior, and firmware integration.
