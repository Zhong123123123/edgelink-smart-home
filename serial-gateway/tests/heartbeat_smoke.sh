#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
CFG="/tmp/sg_heartbeat_smoke.yaml"

cleanup() {
  pkill -P $$ || true
}
trap cleanup EXIT

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DSERIAL_GATEWAY_ENABLE_MQTT=ON
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
  disk_cache_enabled: false

runtime:
  queue_capacity: 128
  stats_interval_sec: 2
  drop_unknown_devices: false

command:
  enabled: false

heartbeat:
  enabled: true
  interval_sec: 1
  gateway_id: "gw-heartbeat-smoke"

log:
  file: "/tmp/sg_heartbeat_gateway.log"
  level: "INFO"
  also_stdout: false
CFGEOF

rm -f /tmp/sg_heartbeat_receiver.log /tmp/sg_heartbeat_gateway.log

"$BUILD_DIR/pty_bridge" /tmp/ttyV0 /tmp/ttyV1 >/tmp/sg_heartbeat_bridge.log 2>&1 &
for _ in $(seq 1 30); do
  [[ -e /tmp/ttyV0 && -e /tmp/ttyV1 ]] && break
  sleep 0.1
done

"$BUILD_DIR/tcp_receiver" 9000 >/tmp/sg_heartbeat_receiver.log 2>&1 &
"$BUILD_DIR/serial_gateway" --config="$CFG" >/tmp/sg_heartbeat_gateway_stdout.log 2>&1 &

sleep 4

if ! grep -q '"type":"heartbeat"' /tmp/sg_heartbeat_receiver.log; then
  echo "heartbeat smoke failed: no heartbeat payload"
  echo "--- receiver ---"
  cat /tmp/sg_heartbeat_receiver.log || true
  echo "--- gateway ---"
  cat /tmp/sg_heartbeat_gateway.log || true
  exit 1
fi

if ! grep -q '"rss_kb":' /tmp/sg_heartbeat_receiver.log; then
  echo "heartbeat smoke failed: resource usage fields missing"
  echo "--- receiver ---"
  cat /tmp/sg_heartbeat_receiver.log || true
  exit 2
fi

echo "heartbeat smoke passed"
