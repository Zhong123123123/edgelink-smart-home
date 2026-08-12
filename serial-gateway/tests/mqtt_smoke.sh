#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
CFG="/tmp/sg_mqtt_smoke.yaml"
BROKER_PORT=18883
TOPIC="upstream/gw001/data"
HEARTBEAT_TOPIC="upstream/gw001/heartbeat"

cleanup() {
  pkill -P $$ || true
}
trap cleanup EXIT

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DSERIAL_GATEWAY_ENABLE_MQTT=ON
cmake --build "$BUILD_DIR" -j

if [[ ! -x "$BUILD_DIR/mqtt_receiver" ]]; then
  echo "mqtt smoke skipped: mqtt_receiver not built (libmosquitto may be missing)"
  exit 0
fi

if ! command -v mosquitto >/dev/null 2>&1; then
  echo "mqtt smoke skipped: mosquitto broker binary not found"
  exit 0
fi

cat > "$CFG" <<CFGEOF
serial:
  device: "/tmp/ttyV1"
  baudrate: 115200
  data_bits: 8
  parity: "N"
  stop_bits: 1
  read_chunk_size: 256

uploader:
  type: "mqtt"
  host: "127.0.0.1"
  port: $BROKER_PORT
  mqtt_topic: "$TOPIC"
  mqtt_heartbeat_topic: "$HEARTBEAT_TOPIC"
  mqtt_client_id: "serial-gateway-gw001-smoke"
  disk_cache_enabled: false

runtime:
  queue_capacity: 1024
  stats_interval_sec: 2
  drop_unknown_devices: false

devices:
  - id: 1
    name: "sensor-01"
    enabled: true

log:
  file: "/tmp/sg_mqtt_gateway.log"
  level: "INFO"
  also_stdout: false
CFGEOF

rm -f /tmp/sg_mqtt_receiver.log /tmp/sg_mqtt_gateway.log

"$BUILD_DIR/pty_bridge" /tmp/ttyV0 /tmp/ttyV1 >/tmp/sg_mqtt_bridge.log 2>&1 &
for _ in $(seq 1 30); do
  [[ -e /tmp/ttyV0 && -e /tmp/ttyV1 ]] && break
  sleep 0.1
done

mosquitto -p "$BROKER_PORT" -v >/tmp/sg_mqtt_broker.log 2>&1 &
sleep 0.5

"$BUILD_DIR/mqtt_receiver" 127.0.0.1 "$BROKER_PORT" "$TOPIC" >/tmp/sg_mqtt_receiver.log 2>&1 &
"$BUILD_DIR/serial_gateway" --config="$CFG" >/tmp/sg_mqtt_gateway_stdout.log 2>&1 &
"$BUILD_DIR/fake_sensor" /tmp/ttyV0 150 115200 >/tmp/sg_mqtt_sensor.log 2>&1 &

sleep 5

if ! grep -q '"temperature"' /tmp/sg_mqtt_receiver.log; then
  echo "mqtt smoke failed: no mqtt payload received"
  echo "--- receiver ---"
  cat /tmp/sg_mqtt_receiver.log || true
  echo "--- gateway ---"
  cat /tmp/sg_mqtt_gateway.log || true
  echo "--- broker ---"
  cat /tmp/sg_mqtt_broker.log || true
  exit 1
fi

echo "mqtt smoke passed"
