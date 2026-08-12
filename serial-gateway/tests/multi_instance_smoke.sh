#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
CFG="/tmp/sg_multi_instance_smoke.yaml"

cleanup() {
  pkill -P $$ || true
}
trap cleanup EXIT

if ! command -v curl >/dev/null 2>&1; then
  echo "multi_instance_smoke requires curl"
  exit 99
fi

cmake -S "$ROOT_DIR" -B "$BUILD_DIR"
cmake --build "$BUILD_DIR" -j

cat > "$CFG" <<'CFGEOF'
serial:
  instance_name: "fallback"
  device: "/tmp/ttyV1"
  baudrate: 115200

serials:
  - instance_name: "line-a"
    device: "/tmp/ttyV1"
    baudrate: 115200
    data_bits: 8
    parity: "N"
    stop_bits: 1
    read_chunk_size: 256
  - instance_name: "line-b"
    device: "/tmp/ttyV3"
    baudrate: 115200
    data_bits: 8
    parity: "N"
    stop_bits: 1
    read_chunk_size: 256

uploader:
  type: "tcp"
  host: "127.0.0.1"
  port: 19020
  disk_cache_enabled: false

runtime:
  queue_capacity: 256
  stats_interval_sec: 1
  drop_unknown_devices: false

command:
  enabled: false

heartbeat:
  enabled: false

reload:
  enabled: false

monitor:
  enabled: true
  bind_host: "127.0.0.1"
  port: 9920
  recent_capacity: 50

devices:
  - id: 1
    name: "sensor-01"
    enabled: true

log:
  file: "/tmp/sg_multi_instance_smoke.log"
  level: "INFO"
  also_stdout: false
CFGEOF

rm -f /tmp/sg_multi_instance_smoke.log

"$BUILD_DIR/pty_bridge" /tmp/ttyV0 /tmp/ttyV1 >/tmp/sg_multi_bridge1.log 2>&1 &
"$BUILD_DIR/pty_bridge" /tmp/ttyV2 /tmp/ttyV3 >/tmp/sg_multi_bridge2.log 2>&1 &
for _ in $(seq 1 40); do
  [[ -e /tmp/ttyV0 && -e /tmp/ttyV1 && -e /tmp/ttyV2 && -e /tmp/ttyV3 ]] && break
  sleep 0.1
done

"$BUILD_DIR/tcp_receiver" 19020 >/tmp/sg_multi_receiver.log 2>&1 &
"$BUILD_DIR/serial_gateway" --config="$CFG" >/tmp/sg_multi_gateway_stdout.log 2>&1 &
"$BUILD_DIR/fake_sensor" /tmp/ttyV0 60 115200 >/tmp/sg_multi_sensor1.log 2>&1 &
"$BUILD_DIR/fake_sensor" /tmp/ttyV2 60 115200 >/tmp/sg_multi_sensor2.log 2>&1 &

sleep 5

STATUS0="$(curl -fsS http://127.0.0.1:9920/api/status)"
STATUS1="$(curl -fsS http://127.0.0.1:9921/api/status)"
METRICS0="$(curl -fsS http://127.0.0.1:9920/metrics)"
METRICS1="$(curl -fsS http://127.0.0.1:9921/metrics)"

if [[ "$STATUS0" != *'"serial_instance":"line-a"'* ]]; then
  echo "multi_instance_smoke failed: line-a status missing"
  echo "$STATUS0"
  exit 1
fi

if [[ "$STATUS1" != *'"serial_instance":"line-b"'* ]]; then
  echo "multi_instance_smoke failed: line-b status missing"
  echo "$STATUS1"
  exit 2
fi

if [[ "$METRICS0" != *'sg_device_online{'* || "$METRICS1" != *'sg_device_online{'* ]]; then
  echo "multi_instance_smoke failed: device metrics missing"
  exit 3
fi

if [[ "$METRICS0" != *'instance="line-a"'* || "$METRICS1" != *'instance="line-b"'* ]]; then
  echo "multi_instance_smoke failed: metrics instance label mismatch"
  exit 4
fi

echo "multi_instance_smoke passed"
