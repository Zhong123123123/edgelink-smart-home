#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
LOG_DIR="${LOG_DIR:-/tmp/sg_tcp_binary_test}"
CFG_FILE="${LOG_DIR}/gateway_tcp_binary.yaml"
UPLOAD_PORT="${UPLOAD_PORT:-19300}"
COMMAND_PORT="${COMMAND_PORT:-19301}"
MONITOR_PORT="${MONITOR_PORT:-19310}"
TCP_BIN_PORT="${TCP_BIN_PORT:-19311}"
SER_A="/tmp/ttyTB0"
SER_B="/tmp/ttyTB1"

mkdir -p "${LOG_DIR}"

cleanup() {
  pkill -f "serial_gateway" || true
  pkill -f "tcp_receiver" || true
  pkill -f "pty_bridge" || true
}
trap cleanup EXIT
cleanup

cat > "${CFG_FILE}" <<EOF_CFG
serial:
  instance_name: "primary"
  device: "${SER_A}"
  baudrate: 115200
  data_bits: 8
  parity: "N"
  stop_bits: 1
  read_chunk_size: 256
uploader:
  type: "tcp"
  host: "127.0.0.1"
  port: ${UPLOAD_PORT}
runtime:
  queue_capacity: 1024
  stats_interval_sec: 3
  drop_unknown_devices: false
  device_offline_timeout_sec: 3
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
  heartbeat_timeout_ms: 2500
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

python3 "${ROOT_DIR}/tools/mock_stm32_wifi_bridge.py" \
  --host 127.0.0.1 --port "${TCP_BIN_PORT}" --device-id 1 \
  --listen-command-sec 6 --dump "${LOG_DIR}/cmd.bin" >"${LOG_DIR}/mock.out" &
MOCK_PID=$!
sleep 1
"${BUILD_DIR}/command_sender" 127.0.0.1 "${COMMAND_PORT}" DEVCMD 1 701 5000 get_status >"${LOG_DIR}/cmd.out" || true
wait "${MOCK_PID}"
sleep 1

curl --noproxy '*' -sf "http://127.0.0.1:${MONITOR_PORT}/api/devices" > "${LOG_DIR}/devices.json"
grep -q '"device_id":1' "${LOG_DIR}/devices.json"
grep -q '"link_type":"tcp_binary"' "${LOG_DIR}/devices.json"

grep -q 'CMD_RX=1' "${LOG_DIR}/mock.out"
grep -q 'OK ack device_id=1 command_id=701 result=0' "${LOG_DIR}/cmd.out"
grep -q 'tcp_binary crc error' "${LOG_DIR}/serial_gateway.log"
grep -q 'duplicate transport ignored' "${LOG_DIR}/serial_gateway.log" || true

sleep 5

grep -q 'reason=tcp_timeout' "${LOG_DIR}/serial_gateway.log"

echo "[TEST] stm32_wifi_bridge PASS"
