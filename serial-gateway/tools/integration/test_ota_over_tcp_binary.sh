#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
LOG_DIR="${LOG_DIR:-/tmp/sg_ota_over_tcp_binary}"
CFG_FILE="${LOG_DIR}/gateway_ota_over_tcp_binary.yaml"
UPLOAD_PORT="${UPLOAD_PORT:-19400}"
COMMAND_PORT="${COMMAND_PORT:-19401}"
MONITOR_PORT="${MONITOR_PORT:-19410}"
TCP_BIN_PORT="${TCP_BIN_PORT:-19411}"
SER_A="/tmp/ttyTO0"
SER_B="/tmp/ttyTO1"
FIRMWARE_ID="${FIRMWARE_ID:-stm32f407-smarthome-1.1.0}"
EXPECT_RESULT="${EXPECT_RESULT:-SUCCESS}"
MOCK_ARGS="${MOCK_ARGS:-}"

mkdir -p "${LOG_DIR}"
cd "${ROOT_DIR}"
rm -f "${ROOT_DIR}/data/ota_tasks.json" "${ROOT_DIR}/data/ota_tasks.db"

cleanup() {
  pkill -f "serial_gateway" || true
  pkill -f "tcp_receiver" || true
  pkill -f "pty_bridge" || true
  pkill -f "mock_bootloader_tcp_binary.py" || true
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
  heartbeat_timeout_ms: 5000
devices:
  - device_id: 1
    name: "stm32-main"
    enabled: true
    link_type: "serial"
    topic_suffix: "line-a/stm32-main"
log:
  file: "${LOG_DIR}/serial_gateway.log"
  level: "INFO"
  also_stdout: true
EOF_CFG

"${BUILD_DIR}/pty_bridge" "${SER_A}" "${SER_B}" >"${LOG_DIR}/pty_bridge.log" 2>&1 &
"${BUILD_DIR}/tcp_receiver" "${UPLOAD_PORT}" >"${LOG_DIR}/tcp_receiver.log" 2>&1 &
"${BUILD_DIR}/serial_gateway" --config="${CFG_FILE}" >"${LOG_DIR}/gateway.stdout.log" 2>&1 &
for _ in {1..40}; do
  if curl --noproxy '*' -sf "http://127.0.0.1:${MONITOR_PORT}/api/devices" >/dev/null 2>&1; then
    break
  fi
  sleep 0.25
done

python3 "${ROOT_DIR}/tools/mock_bootloader_tcp_binary.py" \
  --host 127.0.0.1 \
  --port "${TCP_BIN_PORT}" \
  --device-id 1 ${MOCK_ARGS} >"${LOG_DIR}/mock_bl.out" 2>&1 &
MOCK_PID=$!
sleep 1

CREATE_HTTP="$(curl --noproxy '*' -sS -o "${LOG_DIR}/create_resp.json" -w "%{http_code}" -X POST "http://127.0.0.1:${MONITOR_PORT}/api/ota/tasks" \
  -H "Content-Type: application/json" \
  -d "{\"device_id\":1,\"device_type\":\"stm32f407-smarthome\",\"firmware_id\":\"${FIRMWARE_ID}\",\"transport\":\"tcp_binary\"}")"
CREATE_JSON="$(cat "${LOG_DIR}/create_resp.json")"
if [[ "${CREATE_HTTP}" != "200" ]]; then
  echo "[TEST] create ota task failed http=${CREATE_HTTP}"
  echo "${CREATE_JSON}"
  exit 1
fi
TASK_UUID="$(echo "${CREATE_JSON}" | sed -n 's/.*"task_uuid":"\([^"]*\)".*/\1/p')"
if [[ -z "${TASK_UUID}" ]]; then
  echo "[TEST] create ota task failed"
  echo "${CREATE_JSON}"
  exit 1
fi

STATUS=""
for _ in {1..80}; do
  HTTP_CODE="$(curl --noproxy '*' -sS -o "${LOG_DIR}/task_resp.json" -w "%{http_code}" "http://127.0.0.1:${MONITOR_PORT}/api/ota/tasks/${TASK_UUID}" || true)"
  if [[ "${HTTP_CODE}" != "200" ]]; then
    sleep 0.5
    continue
  fi
  TASK_JSON="$(cat "${LOG_DIR}/task_resp.json")"
  STATUS="$(echo "${TASK_JSON}" | sed -n 's/.*"state":"\([^"]*\)".*/\1/p')"
  if [[ "${STATUS}" == "SUCCESS" || "${STATUS}" == "FAILED" || "${STATUS}" == "CANCELED" ]]; then
    break
  fi
  sleep 0.5
done

for _ in {1..20}; do
  if ! kill -0 "${MOCK_PID}" 2>/dev/null; then
    break
  fi
  sleep 0.1
done
if kill -0 "${MOCK_PID}" 2>/dev/null; then
  kill "${MOCK_PID}" 2>/dev/null || true
fi
wait "${MOCK_PID}" 2>/dev/null || true

if [[ "${STATUS}" != "SUCCESS" ]]; then
  if [[ "${EXPECT_RESULT}" == "FAILED_OR_CANCELED" ]] && [[ "${STATUS}" == "FAILED" || "${STATUS}" == "CANCELED" ]]; then
    :
  else
    echo "[TEST] ota_over_tcp_binary FAIL status=${STATUS} expect=${EXPECT_RESULT}"
    cat "${LOG_DIR}/serial_gateway.log" || true
    exit 1
  fi
fi

grep -q "\\[OTA\\] device=1 transport=tcp_binary" "${LOG_DIR}/serial_gateway.log"

echo "[TEST] ota_over_tcp_binary PASS task=${TASK_UUID} status=${STATUS} expect=${EXPECT_RESULT}"
