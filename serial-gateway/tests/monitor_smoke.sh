#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
CFG="/tmp/sg_monitor_smoke.yaml"

cleanup() {
  pkill -P $$ || true
}
trap cleanup EXIT

if ! command -v curl >/dev/null 2>&1; then
  echo "monitor smoke requires curl"
  exit 99
fi

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
  port: 19000
  disk_cache_enabled: false

runtime:
  queue_capacity: 128
  stats_interval_sec: 2
  drop_unknown_devices: false

command:
  enabled: false

heartbeat:
  enabled: false

monitor:
  enabled: true
  bind_host: "127.0.0.1"
  port: 9910
  recent_capacity: 50

log:
  file: "/tmp/sg_monitor_gateway.log"
  level: "INFO"
  also_stdout: false
CFGEOF

rm -f /tmp/sg_monitor_gateway.log

"$BUILD_DIR/pty_bridge" /tmp/ttyV0 /tmp/ttyV1 >/tmp/sg_monitor_bridge.log 2>&1 &
for _ in $(seq 1 30); do
  [[ -e /tmp/ttyV0 && -e /tmp/ttyV1 ]] && break
  sleep 0.1
done

"$BUILD_DIR/serial_gateway" --config="$CFG" >/tmp/sg_monitor_stdout.log 2>&1 &
"$BUILD_DIR/fake_sensor" /tmp/ttyV0 120 115200 >/tmp/sg_monitor_sensor.log 2>&1 &

sleep 4

STATUS_JSON="$(curl -fsS http://127.0.0.1:9910/api/status)"
RECENT_JSON="$(curl -fsS http://127.0.0.1:9910/api/recent)"
HOME_HTML="$(curl -fsS http://127.0.0.1:9910/)"

if [[ "$STATUS_JSON" != *'"uptime_sec"'* || "$STATUS_JSON" != *'"recent_count"'* ]]; then
  echo "monitor smoke failed: /api/status fields missing"
  echo "$STATUS_JSON"
  exit 1
fi

if [[ "$RECENT_JSON" != *'"temperature"'* ]]; then
  echo "monitor smoke failed: /api/recent has no sample"
  echo "$RECENT_JSON"
  exit 2
fi

if [[ "$HOME_HTML" != *"Serial Gateway Monitor"* ]]; then
  echo "monitor smoke failed: monitor home page not rendered"
  exit 3
fi

echo "monitor smoke passed"
