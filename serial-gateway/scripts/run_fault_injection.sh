#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
CFG="/tmp/sg_fault_injection.yaml"

PORT="${1:-9000}"
FAULT_SEC="${2:-5}"
RECOVERY_SEC="${3:-5}"

LOG_GATEWAY="/tmp/sg_fault_gateway.log"
LOG_GATEWAY_STDOUT="/tmp/sg_fault_gateway_stdout.log"
LOG_RECEIVER="/tmp/sg_fault_receiver.log"
LOG_SENSOR="/tmp/sg_fault_sensor.log"
LOG_BRIDGE="/tmp/sg_fault_bridge.log"
CACHE_FILE="/tmp/sg_fault_pending.log"

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

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "missing required command: $1"
    exit 1
  fi
}

require_cmd cmake
require_cmd wc
require_cmd grep

cmake -S "$ROOT_DIR" -B "$BUILD_DIR"
cmake --build "$BUILD_DIR" -j

cat > "$CFG" <<CFGEOF
serial:
  instance_name: "fault-test"
  device: "/tmp/ttyV1"
  baudrate: 115200
  data_bits: 8
  parity: "N"
  stop_bits: 1
  read_chunk_size: 256

uploader:
  type: "tcp"
  host: "127.0.0.1"
  port: $PORT
  disk_cache_enabled: true
  disk_cache_file: "$CACHE_FILE"
  replay_batch_size: 2000
  replay_pause_ms: 0
  connect_timeout_ms: 500
  reconnect_initial_ms: 100
  reconnect_max_ms: 500

runtime:
  queue_capacity: 2048
  stats_interval_sec: 1
  drop_unknown_devices: false
  cache_backlog_warn_threshold: 20

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
  echo "fault injection failed: virtual tty not ready"
  exit 2
fi

"$BUILD_DIR/tcp_receiver" "$PORT" >"$LOG_RECEIVER" 2>&1 &
RECEIVER_PID="$!"

"$BUILD_DIR/serial_gateway" --config="$CFG" >"$LOG_GATEWAY_STDOUT" 2>&1 &
GATEWAY_PID="$!"

"$BUILD_DIR/fake_sensor" /tmp/ttyV0 30 115200 >"$LOG_SENSOR" 2>&1 &
SENSOR_PID="$!"

sleep 2

echo "[phase1] baseline running"

echo "[phase2] inject network fault: stop tcp_receiver for ${FAULT_SEC}s"
kill "$RECEIVER_PID" 2>/dev/null || true
wait "$RECEIVER_PID" 2>/dev/null || true
RECEIVER_PID=""
sleep "$FAULT_SEC"

CACHE_DURING_FAULT=0
if [[ -f "$CACHE_FILE" ]]; then
  CACHE_DURING_FAULT=$(wc -l < "$CACHE_FILE")
fi
if [[ "$CACHE_DURING_FAULT" -le 0 ]]; then
  echo "fault injection failed: cache did not grow during network fault"
  exit 3
fi

echo "[phase3] recovery: stop sensor, restart receiver, wait cache drain ${RECOVERY_SEC}s"
kill "$SENSOR_PID" 2>/dev/null || true
wait "$SENSOR_PID" 2>/dev/null || true
SENSOR_PID=""

"$BUILD_DIR/tcp_receiver" "$PORT" >"$LOG_RECEIVER" 2>&1 &
RECEIVER_PID="$!"
sleep "$RECOVERY_SEC"

CACHE_AFTER_RECOVERY=0
if [[ -f "$CACHE_FILE" ]]; then
  CACHE_AFTER_RECOVERY=$(wc -l < "$CACHE_FILE")
fi

if [[ "$CACHE_AFTER_RECOVERY" -ne 0 ]]; then
  echo "fault injection failed: cache not drained after recovery"
  echo "cache during fault=$CACHE_DURING_FAULT, cache after recovery=$CACHE_AFTER_RECOVERY"
  exit 4
fi

if ! grep -q 'metrics.reconnects=' "$LOG_GATEWAY"; then
  echo "fault injection failed: reconnect metrics not found in gateway log"
  exit 5
fi

echo "fault injection passed"
echo "cache during fault: $CACHE_DURING_FAULT"
echo "cache after recovery: $CACHE_AFTER_RECOVERY"
echo "logs:"
echo "  gateway:  $LOG_GATEWAY"
echo "  receiver: $LOG_RECEIVER"
echo "  sensor:   $LOG_SENSOR"
