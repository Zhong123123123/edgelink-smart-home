# STM32 WiFi Bridge Test Report

## Automated cases (pre-hardware)

- [x] `tcp_binary` full frame parsing.
- [x] half-packet reassembly.
- [x] sticky packets parse two frames.
- [x] bad CRC drop + resync to next good frame.
- [x] adapter heartbeat (`TYPE=0x7E`) accepted and session kept alive.
- [x] heartbeat stop triggers timeout close and active transport clear.
- [x] same `device_id` serial/tcp_binary conflict: lower priority ignored.
- [x] command routed to active `tcp_binary` session and mock bridge receives full AA55 command frame.
- [x] mock bridge sends command ACK and gateway returns tracked success.

## Script

Run:

```bash
cd serial-gateway
./tools/integration/test_stm32_wifi_bridge.sh
```

Artifacts:

- `/tmp/sg_tcp_binary_test/serial_gateway.log`
- `/tmp/sg_tcp_binary_test/mock.out`
- `/tmp/sg_tcp_binary_test/cmd.out`
- `/tmp/sg_tcp_binary_test/cmd.bin`

## 30-minute pre-hardware stability

Run:

```bash
cd serial-gateway
DURATION_SEC=1800 ./tools/integration/run_tcp_binary_stability_30m.sh
```

Quick smoke before full 30 min:

```bash
DURATION_SEC=120 ./tools/integration/run_tcp_binary_stability_30m.sh
```

Artifacts:

- `/tmp/sg_tcp_binary_30m/summary.txt`
- `/tmp/sg_tcp_binary_30m/status.json`
- `/tmp/sg_tcp_binary_30m/devices.json`
- `/tmp/sg_tcp_binary_30m/serial_gateway.log`
