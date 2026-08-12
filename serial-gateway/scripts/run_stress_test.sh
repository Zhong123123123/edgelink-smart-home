#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
CFG="/tmp/sg_stress.yaml"

DURATION_SEC="${1:-20}"
INTERVAL_MS="${2:-5}"
PORT="${3:-9000}"

LOG_GATEWAY="/tmp/sg_stress_gateway.log"
LOG_GATEWAY_STDOUT="/tmp/sg_stress_gateway_stdout.log"
LOG_RECEIVER="/tmp/sg_stress_receiver.log"
LOG_SENSOR="/tmp/sg_stress_sensor.log"
LOG_BRIDGE="/tmp/sg_stress_bridge.log"
CACHE_FILE="/tmp/sg_stress_pending.log"

BRIDGE_PID=""
GATEWAY_PID=""
RECEIVER_PID=""
SENSOR_PID=""

cleanup() {
  for pid in "$SENSOR_PID" "$RECEIVER_PID" "$GATEWAY_PID" "$BRIDGE_PID"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

if [[ "$DURATION_SEC" -lt 5 ]]; then
  echo "duration must be >= 5 sec"
  exit 1
fi
if [[ "$INTERVAL_MS" -lt 1 ]]; then
  echo "interval_ms must be >= 1"
  exit 1
fi

cmake -S "$ROOT_DIR" -B "$BUILD_DIR"
cmake --build "$BUILD_DIR" -j

cat > "$CFG" <<CFGEOF
serial:
  instance_name: "stress"
  device: "/tmp/ttyV1"
  baudrate: 115200
  data_bits: 8
  parity: "N"
  stop_bits: 1
  read_chunk_size: 512

uploader:
  type: "tcp"
  host: "127.0.0.1"
  port: $PORT
  disk_cache_enabled: true
  disk_cache_file: "$CACHE_FILE"
  replay_batch_size: 2000
  replay_pause_ms: 0
  reconnect_initial_ms: 100
  reconnect_max_ms: 500

runtime:
  queue_capacity: 8192
  stats_interval_sec: 1
  drop_unknown_devices: false

command:
  enabled: false

heartbeat:
  enabled: false

reload:
  enabled: false

monitor:
  enabled: false

devices:
  - id: 1
    name: "sensor-01"
    enabled: true

log:
  file: "$LOG_GATEWAY"
  level: "INFO"
  also_stdout: false
CFGEOF

rm -f "$LOG_GATEWAY" "$LOG_GATEWAY_STDOUT" "$LOG_RECEIVER" "$LOG_SENSOR" "$LOG_BRIDGE" "$CACHE_FILE"

"$BUILD_DIR/pty_bridge" /tmp/ttyV0 /tmp/ttyV1 >"$LOG_BRIDGE" 2>&1 &
BRIDGE_PID="$!"

for _ in $(seq 1 50); do
  [[ -e /tmp/ttyV0 && -e /tmp/ttyV1 ]] && break
  sleep 0.1
done
if [[ ! -e /tmp/ttyV0 || ! -e /tmp/ttyV1 ]]; then
  echo "stress test failed: virtual tty not ready"
  exit 2
fi

"$BUILD_DIR/tcp_receiver" "$PORT" >"$LOG_RECEIVER" 2>&1 &
RECEIVER_PID="$!"

"$BUILD_DIR/serial_gateway" --config="$CFG" >"$LOG_GATEWAY_STDOUT" 2>&1 &
GATEWAY_PID="$!"

"$BUILD_DIR/fake_sensor" /tmp/ttyV0 "$INTERVAL_MS" 115200 >"$LOG_SENSOR" 2>&1 &
SENSOR_PID="$!"

echo "stress test running: duration=${DURATION_SEC}s interval=${INTERVAL_MS}ms"
sleep "$DURATION_SEC"

kill "$SENSOR_PID" 2>/dev/null || true
wait "$SENSOR_PID" 2>/dev/null || true
SENSOR_PID=""

sleep 1

LAST_METRICS_LINE="$(grep 'metrics.frames_rx=' "$LOG_GATEWAY" | tail -n 1 || true)"
if [[ -z "$LAST_METRICS_LINE" ]]; then
  echo "stress test failed: no metrics line found"
  exit 3
fi

FRAMES_RX="$(echo "$LAST_METRICS_LINE" | sed -n 's/.*metrics.frames_rx=\([0-9][0-9]*\).*/\1/p')"
UPLOAD_OK="$(echo "$LAST_METRICS_LINE" | sed -n 's/.*metrics.upload_ok=\([0-9][0-9]*\).*/\1/p')"
CACHE_BACKLOG="$(echo "$LAST_METRICS_LINE" | sed -n 's/.*metrics.cache_backlog=\([0-9][0-9]*\).*/\1/p')"

if [[ -z "$FRAMES_RX" || -z "$UPLOAD_OK" || -z "$CACHE_BACKLOG" ]]; then
  echo "stress test failed: metrics parse error"
  echo "line: $LAST_METRICS_LINE"
  exit 4
fi

if [[ "$FRAMES_RX" -le 20 ]]; then
  echo "stress test failed: frames_rx too low ($FRAMES_RX)"
  exit 5
fi

echo "stress test passed"
echo "frames_rx=$FRAMES_RX upload_ok=$UPLOAD_OK cache_backlog=$CACHE_BACKLOG"
echo "last metrics: $LAST_METRICS_LINE"
echo "logs:"
echo "  gateway:  $LOG_GATEWAY"
echo "  receiver: $LOG_RECEIVER"
echo "  sensor:   $LOG_SENSOR"
