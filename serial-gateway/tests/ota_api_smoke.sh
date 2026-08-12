#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
CFG="/tmp/sg_ota_api_smoke.yaml"

cleanup() {
  pkill -P $$ || true
}
trap cleanup EXIT

if ! command -v curl >/dev/null 2>&1; then
  echo "ota api smoke requires curl"
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
  port: 9930
  recent_capacity: 50

log:
  file: "/tmp/sg_ota_api_smoke.log"
  level: "INFO"
  also_stdout: false
CFGEOF

rm -f /tmp/sg_ota_api_smoke.log

"$BUILD_DIR/pty_bridge" /tmp/ttyV0 /tmp/ttyV1 >/tmp/sg_ota_api_bridge.log 2>&1 &
for _ in $(seq 1 30); do
  [[ -e /tmp/ttyV0 && -e /tmp/ttyV1 ]] && break
  sleep 0.1
done

"$BUILD_DIR/serial_gateway" --config="$CFG" >/tmp/sg_ota_api_gateway_stdout.log 2>&1 &
$BUILD_DIR/fake_sensor /tmp/ttyV0 150 115200 >/tmp/sg_ota_api_sensor.log 2>&1 &
sleep 4

CREATE="$(
  curl -fsS -X POST "http://127.0.0.1:9930/api/ota/tasks" \
    -H 'Content-Type: application/json' \
    -d '{"device_id":1,"device_type":"stm32f407-smarthome","firmware_id":"stm32f407-smarthome-1.3.9-a1","transport":"serial","target":"127.0.0.1:19090"}'
)"
TASK_UUID="$(echo "$CREATE" | sed -n 's/.*"task_uuid":"\([^"]*\)".*/\1/p')"

if [[ -z "$TASK_UUID" ]]; then
  echo "ota api smoke failed: create response missing task_uuid"
  echo "$CREATE"
  exit 1
fi

sleep 1

LIST="$(curl -fsS "http://127.0.0.1:9930/api/ota/tasks")"
DETAIL="$(curl -fsS "http://127.0.0.1:9930/api/ota/tasks/$TASK_UUID")"
EVENTS="$(curl -fsS "http://127.0.0.1:9930/api/ota/tasks/$TASK_UUID/events")"
CANCEL="$(curl -fsS -X POST "http://127.0.0.1:9930/api/ota/tasks/$TASK_UUID/cancel")"
RETRY="$(curl -fsS -X POST "http://127.0.0.1:9930/api/ota/tasks/$TASK_UUID/retry")"

if [[ "$LIST" != *"$TASK_UUID"* ]]; then
  echo "ota api smoke failed: task not listed"
  echo "$LIST"
  exit 2
fi

if [[ "$DETAIL" != *"$TASK_UUID"* ]]; then
  echo "ota api smoke failed: task detail missing uuid"
  echo "$DETAIL"
  exit 3
fi

if [[ "$EVENTS" != *'"'* ]]; then
  echo "ota api smoke failed: events response malformed"
  echo "$EVENTS"
  exit 4
fi

if [[ "$CANCEL" != *'"ok":true'* ]]; then
  echo "ota api smoke failed: cancel not ok"
  echo "$CANCEL"
  exit 5
fi

if [[ "$RETRY" != *'"ok":true'* ]]; then
  echo "ota api smoke failed: retry not ok"
  echo "$RETRY"
  exit 6
fi

echo "ota api smoke passed"
