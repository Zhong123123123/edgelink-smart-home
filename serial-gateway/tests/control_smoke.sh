#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
CFG="/tmp/sg_control_smoke.yaml"

cleanup() {
  pkill -P $$ || true
}
trap cleanup EXIT

if ! command -v curl >/dev/null 2>&1; then
  echo "control smoke requires curl"
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
  port: 9920
  recent_capacity: 50

log:
  file: "/tmp/sg_control_smoke.log"
  level: "INFO"
  also_stdout: false
CFGEOF

rm -f /tmp/sg_control_smoke.log

"$BUILD_DIR/pty_bridge" /tmp/ttyV0 /tmp/ttyV1 >/tmp/sg_control_bridge.log 2>&1 &
for _ in $(seq 1 30); do
  [[ -e /tmp/ttyV0 && -e /tmp/ttyV1 ]] && break
  sleep 0.1
done

"$BUILD_DIR/serial_gateway" --config="$CFG" >/tmp/sg_control_gateway_stdout.log 2>&1 &
"$BUILD_DIR/fake_sensor" /tmp/ttyV0 200 115200 >/tmp/sg_control_sensor.log 2>&1 &

sleep 4

RESPONSE="$(
  curl -fsS -X POST "http://127.0.0.1:9920/api/command" \
    -H 'Content-Type: application/json' \
    -d '{"device_id":1,"command_type":"set_led","args":{"on":true},"timeout_ms":500}'
)"
SEMANTIC_LED="$(
  curl -fsS -X POST "http://127.0.0.1:9920/api/device/1/led" \
    -H 'Content-Type: application/json' \
    -d '{"on":false,"timeout_ms":500}'
)"
SEMANTIC_QUERY="$(
  curl -fsS -X POST "http://127.0.0.1:9920/api/device/1/status/query" \
    -H 'Content-Type: application/json' \
    -d '{"timeout_ms":500}'
)"
RECENT="$(curl -fsS "http://127.0.0.1:9920/api/commands/recent")"
STATUS="$(curl -fsS "http://127.0.0.1:9920/api/status")"

if [[ "$RESPONSE" != *'"command_type":"set_led"'* || "$RESPONSE" != *'"status":"timeout"'* ]]; then
  echo "control smoke failed: /api/command response unexpected"
  echo "$RESPONSE"
  exit 1
fi

if [[ "$SEMANTIC_LED" != *'"command_type":"set_led"'* || "$SEMANTIC_LED" != *'"status":"timeout"'* ]]; then
  echo "control smoke failed: semantic led response unexpected"
  echo "$SEMANTIC_LED"
  exit 2
fi

if [[ "$SEMANTIC_QUERY" != *'"command_type":"get_status"'* || "$SEMANTIC_QUERY" != *'"status":"timeout"'* ]]; then
  echo "control smoke failed: semantic query response unexpected"
  echo "$SEMANTIC_QUERY"
  exit 3
fi

if [[ "$RECENT" != *'"commands"'* || "$RECENT" != *'"command_type":"set_led"'* ]]; then
  echo "control smoke failed: /api/commands/recent missing command"
  echo "$RECENT"
  exit 4
fi

if [[ "$STATUS" != *'"uptime_sec"'* || "$STATUS" != *'"recent_count"'* ]]; then
  echo "control smoke failed: /api/status fields missing"
  echo "$STATUS"
  exit 5
fi

echo "control smoke passed"
