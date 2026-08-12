#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
LOG_DIR="${LOG_DIR:-/tmp/sg_tcp_binary_30m}"
CFG_FILE="${LOG_DIR}/gateway_tcp_binary_30m.yaml"
DURATION_SEC="${DURATION_SEC:-1800}"
UPLOAD_PORT="${UPLOAD_PORT:-19400}"
COMMAND_PORT="${COMMAND_PORT:-19401}"
MONITOR_PORT="${MONITOR_PORT:-19410}"
TCP_BIN_PORT="${TCP_BIN_PORT:-19411}"
SER_A="/tmp/ttyTB30A"
SER_B="/tmp/ttyTB30B"

mkdir -p "${LOG_DIR}"

cleanup() {
  pkill -f "serial_gateway" || true
  pkill -f "tcp_receiver" || true
  pkill -f "pty_bridge" || true
  pkill -f "mock_stm32_wifi_bridge_daemon.py" || true
}
trap cleanup EXIT
cleanup

cat > "${CFG_FILE}" <<EOF_CFG
serial:
  instance_name: "primary"
  device: "${SER_A}"
  baudrate: 115200
uploader:
  type: "tcp"
  host: "127.0.0.1"
  port: ${UPLOAD_PORT}
runtime:
  queue_capacity: 1024
  stats_interval_sec: 5
  drop_unknown_devices: false
  device_offline_timeout_sec: 6
  cache_backlog_warn_threshold: 2000
command:
  enabled: true
  bind_host: "127.0.0.1"
  port: ${COMMAND_PORT}
  client_timeout_ms: 200
heartbeat:
  enabled: true
  interval_sec: 10
  gateway_id: "serial-gateway"
reload:
  enabled: false
  check_interval_sec: 5
monitor:
  enabled: true
  bind_host: "127.0.0.1"
  port: ${MONITOR_PORT}
  recent_capacity: 100
wifi_device_server:
  enabled: false
  listen_host: "0.0.0.0"
  listen_port: 19100
tcp_binary:
  enabled: true
  listen_host: "0.0.0.0"
  port: ${TCP_BIN_PORT}
  name: "stm32_wifi_bridge"
  priority: 100
  heartbeat_timeout_ms: 4000
devices:
  - device_id: 1
    name: "sensor-01"
    enabled: true
    link_type: "serial"
    topic_suffix: "line-a/sensor-01"
log:
  file: "${LOG_DIR}/serial_gateway.log"
  level: "INFO"
  also_stdout: true
EOF_CFG

"${BUILD_DIR}/pty_bridge" "${SER_A}" "${SER_B}" >"${LOG_DIR}/pty_bridge.log" 2>&1 &
"${BUILD_DIR}/tcp_receiver" "${UPLOAD_PORT}" >"${LOG_DIR}/tcp_receiver.log" 2>&1 &
"${BUILD_DIR}/serial_gateway" --config="${CFG_FILE}" >"${LOG_DIR}/gateway.stdout.log" 2>&1 &
sleep 2

python3 "${ROOT_DIR}/tools/mock_stm32_wifi_bridge_daemon.py" \
  --host 127.0.0.1 --port "${TCP_BIN_PORT}" --device-id 1 \
  --duration-sec "${DURATION_SEC}" --out "${LOG_DIR}/daemon.log" &
DAEMON_PID=$!

# warmup: wait device active on tcp_binary (max 20s)
for _ in {1..20}; do
  if curl --noproxy '*' -sf "http://127.0.0.1:${MONITOR_PORT}/api/devices" | grep -q '"link_type":"tcp_binary"'; then
    break
  fi
  sleep 1
done

end_ts=$(( $(date +%s) + DURATION_SEC ))
cmd_id=1000
ok=0
fail=0

while [[ $(date +%s) -lt ${end_ts} ]]; do
  out="$(${BUILD_DIR}/command_sender 127.0.0.1 ${COMMAND_PORT} DEVCMD 1 ${cmd_id} 4000 get_status || true)"
  if echo "$out" | grep -q "OK ack device_id=1 command_id=${cmd_id} result=0"; then
    ok=$((ok+1))
  else
    fail=$((fail+1))
  fi
  cmd_id=$((cmd_id+1))
  sleep 5
done

wait "${DAEMON_PID}"

curl --noproxy '*' -sf "http://127.0.0.1:${MONITOR_PORT}/api/status" > "${LOG_DIR}/status.json"
curl --noproxy '*' -sf "http://127.0.0.1:${MONITOR_PORT}/api/devices" > "${LOG_DIR}/devices.json"

{
  echo "duration_sec=${DURATION_SEC}"
  echo "command_ok=${ok}"
  echo "command_fail=${fail}"
} > "${LOG_DIR}/summary.txt"

if [[ ${fail} -gt 0 ]]; then
  echo "[TEST] tcp_binary_30m FAIL (command_fail=${fail})"
  exit 1
fi

echo "[TEST] tcp_binary_30m PASS (command_ok=${ok}, fail=${fail})"
