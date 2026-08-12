#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
CFG="/tmp/sg_recovery.yaml"

cleanup() {
  pkill -P $$ || true
}
trap cleanup EXIT

cmake -S "$ROOT_DIR" -B "$BUILD_DIR"
cmake --build "$BUILD_DIR" -j

cat > "$CFG" <<'CFGEOF'
serial:
  device: "/tmp/ttyV1"
  baudrate: 115200
  data_bits: 8
  parity: "N"
  stop_bits: 1
  read_chunk_size: 256

uploader:
  type: "tcp"
  host: "127.0.0.1"
  port: 9000
  disk_cache_enabled: true
  disk_cache_file: "/tmp/sg_recovery_pending.log"
  connect_timeout_ms: 1000
  reconnect_initial_ms: 100
  reconnect_max_ms: 400

runtime:
  queue_capacity: 1024
  stats_interval_sec: 1
  drop_unknown_devices: false

devices:
  - id: 1
    name: "sensor-01"
    enabled: true

log:
  file: "/tmp/sg_recovery_gateway.log"
  level: "INFO"
  also_stdout: false
CFGEOF

rm -f /tmp/sg_recovery_pending.log /tmp/sg_recovery_gateway.log /tmp/sg_recovery_receiver.log
pkill -f "$BUILD_DIR/tcp_receiver 9000" || true

"$BUILD_DIR/pty_bridge" /tmp/ttyV0 /tmp/ttyV1 >/tmp/sg_recovery_bridge.log 2>&1 &
for _ in $(seq 1 30); do
  [[ -e /tmp/ttyV0 && -e /tmp/ttyV1 ]] && break
  sleep 0.1
done

# Phase 1: receiver down, expect cache grows.
"$BUILD_DIR/serial_gateway" --config="$CFG" >/tmp/sg_recovery_gateway_stdout1.log 2>&1 &
"$BUILD_DIR/fake_sensor" /tmp/ttyV0 120 115200 >/tmp/sg_recovery_sensor1.log 2>&1 &
sleep 3

CACHE1=0
if [[ -f /tmp/sg_recovery_pending.log ]]; then
  CACHE1=$(wc -l < /tmp/sg_recovery_pending.log)
fi
if [[ "$CACHE1" -le 0 ]]; then
  echo "recovery test failed: cache did not grow while receiver down"
  exit 1
fi

pkill -f "$BUILD_DIR/serial_gateway --config=$CFG" || true
pkill -f "$BUILD_DIR/fake_sensor /tmp/ttyV0 120 115200" || true
sleep 0.5

# Phase 2: receiver up, expect replay and cache drains.
"$BUILD_DIR/tcp_receiver" 9000 >/tmp/sg_recovery_receiver.log 2>&1 &
"$BUILD_DIR/serial_gateway" --config="$CFG" >/tmp/sg_recovery_gateway_stdout2.log 2>&1 &
"$BUILD_DIR/fake_sensor" /tmp/ttyV0 120 115200 >/tmp/sg_recovery_sensor2.log 2>&1 &
sleep 4

CACHE2=0
if [[ -f /tmp/sg_recovery_pending.log ]]; then
  CACHE2=$(wc -l < /tmp/sg_recovery_pending.log)
fi
if [[ "$CACHE2" -ne 0 ]]; then
  echo "recovery test failed: cache not drained after receiver recovery"
  exit 2
fi

if ! grep -q '"temperature"' /tmp/sg_recovery_receiver.log; then
  echo "recovery test failed: receiver got no JSON payload"
  exit 3
fi

if ! grep -q 'reconnects=' /tmp/sg_recovery_gateway.log; then
  echo "recovery test failed: reconnect stats missing"
  exit 4
fi

echo "recovery test passed"
