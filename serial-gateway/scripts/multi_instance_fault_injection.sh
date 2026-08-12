#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
CFG="/tmp/sg_multi_fault.yaml"

MONITOR_BASE_PORT="${1:-9930}"
RUN_SEC="${2:-4}"
FAULT_SEC="${3:-5}"
MODE="${4:-tcp}" # tcp | mqtt
UPLINK_PORT="${5:-19040}"
RECOVERY_SEC="${6:-4}"

LOG_GATEWAY="/tmp/sg_multi_fault_gateway.log"
PIDS=()

cleanup() {
  for pid in "${PIDS[@]:-}"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

if ! command -v curl >/dev/null 2>&1; then
  echo "multi_instance_fault_injection requires curl"
  exit 99
fi
if ! [[ "$MONITOR_BASE_PORT" =~ ^[0-9]+$ && "$RUN_SEC" =~ ^[0-9]+$ && "$FAULT_SEC" =~ ^[0-9]+$ && "$UPLINK_PORT" =~ ^[0-9]+$ && "$RECOVERY_SEC" =~ ^[0-9]+$ ]]; then
  echo "monitor_base_port/run_sec/fault_sec/uplink_port/recovery_sec must be integers"
  exit 1
fi
if [[ "$MODE" != "tcp" && "$MODE" != "mqtt" ]]; then
  echo "mode must be tcp or mqtt"
  exit 1
fi

cmake -S "$ROOT_DIR" -B "$BUILD_DIR"
cmake --build "$BUILD_DIR" -j

MONITOR_B_PORT=$((MONITOR_BASE_PORT + 1))

if [[ "$MODE" == "mqtt" ]]; then
  if [[ ! -x "$BUILD_DIR/mqtt_receiver" ]]; then
    echo "mqtt mode skipped: mqtt_receiver not built"
    exit 0
  fi
  if ! command -v mosquitto >/dev/null 2>&1; then
    echo "mqtt mode skipped: mosquitto broker binary not found"
    exit 0
  fi
fi

cat > "$CFG" <<CFGEOF
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
  type: "$MODE"
  host: "127.0.0.1"
  port: $UPLINK_PORT
  mqtt_topic: "sensors/data"
  mqtt_client_id: "sg-multi-fault"
  disk_cache_enabled: true
  disk_cache_file: "/tmp/sg_multi_fault_cache.log"
  replay_batch_size: 2000
  replay_pause_ms: 0

runtime:
  queue_capacity: 1024
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
  port: $MONITOR_BASE_PORT
  recent_capacity: 100

devices:
  - id: 1
    name: "sensor-01"
    enabled: true

log:
  file: "$LOG_GATEWAY"
  level: "INFO"
  also_stdout: false
CFGEOF

rm -f /tmp/sg_multi_fault_*.log /tmp/sg_multi_fault_cache.log "$LOG_GATEWAY" /tmp/sg_multi_fault_mqtt_*.log

"$BUILD_DIR/pty_bridge" /tmp/ttyV0 /tmp/ttyV1 >/tmp/sg_multi_fault_bridge_a.log 2>&1 &
PIDS+=("$!")
"$BUILD_DIR/pty_bridge" /tmp/ttyV2 /tmp/ttyV3 >/tmp/sg_multi_fault_bridge_b.log 2>&1 &
BRIDGE_B_PID="$!"
PIDS+=("$BRIDGE_B_PID")

for _ in $(seq 1 40); do
  [[ -e /tmp/ttyV0 && -e /tmp/ttyV1 && -e /tmp/ttyV2 && -e /tmp/ttyV3 ]] && break
  sleep 0.1
done

if [[ "$MODE" == "tcp" ]]; then
  "$BUILD_DIR/tcp_receiver" "$UPLINK_PORT" >/tmp/sg_multi_fault_receiver.log 2>&1 &
  PIDS+=("$!")
else
  mosquitto -p "$UPLINK_PORT" -v >/tmp/sg_multi_fault_mqtt_broker.log 2>&1 &
  PIDS+=("$!")
  sleep 0.5
  "$BUILD_DIR/mqtt_receiver" 127.0.0.1 "$UPLINK_PORT" "sensors/data" >/tmp/sg_multi_fault_mqtt_receiver.log 2>&1 &
  PIDS+=("$!")
fi

"$BUILD_DIR/serial_gateway" --config="$CFG" >/tmp/sg_multi_fault_gateway_stdout.log 2>&1 &
GATEWAY_PID="$!"
PIDS+=("$GATEWAY_PID")
"$BUILD_DIR/fake_sensor" /tmp/ttyV0 50 115200 >/tmp/sg_multi_fault_sensor_a.log 2>&1 &
PIDS+=("$!")
"$BUILD_DIR/fake_sensor" /tmp/ttyV2 50 115200 >/tmp/sg_multi_fault_sensor_b.log 2>&1 &
SENSOR_B_PID="$!"
PIDS+=("$SENSOR_B_PID")

sleep "$RUN_SEC"

status_a_before="$(curl -fsS "http://127.0.0.1:${MONITOR_BASE_PORT}/api/status")"
status_b_before="$(curl -fsS "http://127.0.0.1:${MONITOR_B_PORT}/api/status")"

rx_a_before="$(echo "$status_a_before" | sed -n 's/.*"received_frames":\([0-9][0-9]*\).*/\1/p' | head -n1)"
rx_b_before="$(echo "$status_b_before" | sed -n 's/.*"received_frames":\([0-9][0-9]*\).*/\1/p' | head -n1)"

if [[ -z "$rx_a_before" || -z "$rx_b_before" ]]; then
  echo "fault test failed: unable to parse baseline frames"
  exit 2
fi

echo "[baseline][$MODE] line-a rx=$rx_a_before line-b rx=$rx_b_before"

echo "[fault][$MODE] stop line-b pty bridge for ${FAULT_SEC}s"
kill "$BRIDGE_B_PID" 2>/dev/null || true
wait "$BRIDGE_B_PID" 2>/dev/null || true
sleep "$FAULT_SEC"

status_a_after="$(curl -fsS "http://127.0.0.1:${MONITOR_BASE_PORT}/api/status")"
status_b_after="$(curl -fsS "http://127.0.0.1:${MONITOR_B_PORT}/api/status")"

rx_a_after="$(echo "$status_a_after" | sed -n 's/.*"received_frames":\([0-9][0-9]*\).*/\1/p' | head -n1)"
rx_b_after="$(echo "$status_b_after" | sed -n 's/.*"received_frames":\([0-9][0-9]*\).*/\1/p' | head -n1)"

if [[ -z "$rx_a_after" || -z "$rx_b_after" ]]; then
  echo "fault test failed: unable to parse post-fault frames"
  exit 3
fi

inc_a=$((rx_a_after - rx_a_before))
inc_b=$((rx_b_after - rx_b_before))

echo "[result][$MODE] line-a increment=$inc_a line-b increment=$inc_b"

if [[ "$inc_a" -le 0 ]]; then
  echo "fault test failed: line-a stopped unexpectedly"
  exit 4
fi
if [[ "$inc_b" -ge "$inc_a" ]]; then
  echo "fault test failed: line-b did not degrade as expected"
  exit 5
fi

echo "[recovery][$MODE] restart line-b pty bridge and wait ${RECOVERY_SEC}s"
"$BUILD_DIR/pty_bridge" /tmp/ttyV2 /tmp/ttyV3 >/tmp/sg_multi_fault_bridge_b_recovery.log 2>&1 &
BRIDGE_B_RECOVERY_PID="$!"
PIDS+=("$BRIDGE_B_RECOVERY_PID")

if [[ -n "${SENSOR_B_PID:-}" ]] && kill -0 "$SENSOR_B_PID" 2>/dev/null; then
  kill "$SENSOR_B_PID" 2>/dev/null || true
  wait "$SENSOR_B_PID" 2>/dev/null || true
fi
"$BUILD_DIR/fake_sensor" /tmp/ttyV2 50 115200 >/tmp/sg_multi_fault_sensor_b_recovery.log 2>&1 &
SENSOR_B_PID="$!"
PIDS+=("$SENSOR_B_PID")

if [[ -n "${GATEWAY_PID:-}" ]] && kill -0 "$GATEWAY_PID" 2>/dev/null; then
  kill "$GATEWAY_PID" 2>/dev/null || true
  wait "$GATEWAY_PID" 2>/dev/null || true
fi
"$BUILD_DIR/serial_gateway" --config="$CFG" >/tmp/sg_multi_fault_gateway_stdout_recovery.log 2>&1 &
GATEWAY_PID="$!"
PIDS+=("$GATEWAY_PID")

sleep "$RECOVERY_SEC"

status_a_recover="$(curl -fsS "http://127.0.0.1:${MONITOR_BASE_PORT}/api/status")"
status_b_recover="$(curl -fsS "http://127.0.0.1:${MONITOR_B_PORT}/api/status")"

rx_a_recover="$(echo "$status_a_recover" | sed -n 's/.*"received_frames":\([0-9][0-9]*\).*/\1/p' | head -n1)"
rx_b_recover="$(echo "$status_b_recover" | sed -n 's/.*"received_frames":\([0-9][0-9]*\).*/\1/p' | head -n1)"

if [[ -z "$rx_a_recover" || -z "$rx_b_recover" ]]; then
  echo "fault test failed: unable to parse recovery frames"
  exit 6
fi

echo "[recovery-result][$MODE] line-a rx=$rx_a_recover line-b rx=$rx_b_recover"

if [[ "$rx_a_recover" -le 0 ]]; then
  echo "fault test failed: line-a did not recover after gateway restart"
  exit 7
fi
if [[ "$rx_b_recover" -le 0 ]]; then
  echo "fault test failed: line-b did not recover"
  exit 8
fi

if [[ "$MODE" == "mqtt" ]]; then
  if ! grep -q '"temperature"' /tmp/sg_multi_fault_mqtt_receiver.log; then
    echo "fault test failed: mqtt receiver got no payload"
    exit 9
  fi
fi

echo "multi_instance_fault_injection passed mode=$MODE"
echo "monitor ports: line-a=${MONITOR_BASE_PORT}, line-b=${MONITOR_B_PORT}"
