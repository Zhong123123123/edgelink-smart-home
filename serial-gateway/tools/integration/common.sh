#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
LOG_DIR="${LOG_DIR:-/tmp/sg_wifi_reliability}"
SER_A="/tmp/ttyV0"
SER_B="/tmp/ttyV1"
CFG_FILE="${LOG_DIR}/gateway_wifi_mock.yaml"
UPLOAD_PORT="${UPLOAD_PORT:-19000}"
COMMAND_PORT="${COMMAND_PORT:-19001}"
MONITOR_PORT="${MONITOR_PORT:-19010}"
WIFI_PORT="${WIFI_PORT:-19100}"

mkdir -p "${LOG_DIR}"

cleanup_all() {
  pkill -f "serial_gateway" || true
  pkill -f "mock_wifi_node" || true
  pkill -f "mock_serial_node" || true
  pkill -f "tcp_receiver" || true
  pkill -f "pty_bridge" || true
  sleep 1
}

wait_ports_free() {
  for _ in {1..20}; do
    if ss -ltn | awk '{print $4}' | grep -Eq ":(${UPLOAD_PORT}|${COMMAND_PORT}|${MONITOR_PORT}|${WIFI_PORT})$"; then
      sleep 0.5
      continue
    fi
    return 0
  done
  return 1
}

write_gateway_config() {
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
  disk_cache_enabled: true
  disk_cache_file: "${LOG_DIR}/pending_records.log"
  disk_cache_max_lines: 10000
  replay_batch_size: 200
  replay_pause_ms: 0
runtime:
  queue_capacity: 1024
  stats_interval_sec: 5
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
  enabled: true
  listen_host: "0.0.0.0"
  listen_port: ${WIFI_PORT}
devices:
  - device_id: 1
    name: "sensor-01"
    enabled: true
    link_type: "serial"
    topic_suffix: "line-a/sensor-01"
  - device_id: 2
    name: "wifi-node-02"
    enabled: true
    link_type: "wifi"
    topic_suffix: "line-a/wifi-node-02"
log:
  file: "${LOG_DIR}/serial_gateway.log"
  level: "INFO"
  also_stdout: true
EOF_CFG
}

start_base() {
  cleanup_all
  wait_ports_free || true
  write_gateway_config
  "${BUILD_DIR}/pty_bridge" "${SER_A}" "${SER_B}" > "${LOG_DIR}/pty_bridge.log" 2>&1 &
  PTY_PID=$!
  if [[ "${1:-with_tcp}" == "with_tcp" ]]; then
    "${BUILD_DIR}/tcp_receiver" "${UPLOAD_PORT}" > "${LOG_DIR}/tcp_receiver.log" 2>&1 &
    TCP_PID=$!
  else
    TCP_PID=""
  fi
  "${BUILD_DIR}/serial_gateway" --config="${CFG_FILE}" > "${LOG_DIR}/gateway.log" 2>&1 &
  GW_PID=$!
  sleep 2
}

stop_base() {
  if [[ -n "${GW_PID:-}" ]]; then kill "${GW_PID}" 2>/dev/null || true; fi
  if [[ -n "${TCP_PID:-}" ]]; then kill "${TCP_PID}" 2>/dev/null || true; fi
  if [[ -n "${PTY_PID:-}" ]]; then kill "${PTY_PID}" 2>/dev/null || true; fi
}

start_wifi_node() {
  "${BUILD_DIR}/mock_wifi_node" "$@" > "${LOG_DIR}/mock_wifi.log" 2>&1 &
  WIFI_PID=$!
  sleep 1
}

stop_wifi_node() {
  if [[ -n "${WIFI_PID:-}" ]]; then kill "${WIFI_PID}" 2>/dev/null || true; fi
  WIFI_PID=""
}

start_serial_node() {
  "${BUILD_DIR}/mock_serial_node" "${SER_B}" 1 500 115200 > "${LOG_DIR}/mock_serial.log" 2>&1 &
  SERIAL_NODE_PID=$!
  sleep 1
}

stop_serial_node() {
  if [[ -n "${SERIAL_NODE_PID:-}" ]]; then kill "${SERIAL_NODE_PID}" 2>/dev/null || true; fi
  SERIAL_NODE_PID=""
}

send_wifi_cmd() {
  local cmd_id="$1"
  local cmd_type="$2"
  local params_json="$3"
  local timeout_ms="${4:-5000}"
  local payload="{\"type\":\"command\",\"device_id\":2,\"command_id\":${cmd_id},\"command_type\":\"${cmd_type}\",\"params\":${params_json},\"timeout_ms\":${timeout_ms}}"
  local out
  for _ in {1..10}; do
    if out="$("${BUILD_DIR}/command_sender" 127.0.0.1 "${COMMAND_PORT}" RAW "${payload}" 2>&1)"; then
      echo "${out}"
      return 0
    fi
    sleep 1
  done
  echo "${out}"
  return 1
}

fetch_metrics() {
  curl -sf "http://127.0.0.1:${MONITOR_PORT}/metrics"
}

metric_value() {
  local text="$1"
  local name="$2"
  echo "$text" | awk -v n="$name" '$1 ~ n {print $2}' | tail -n1
}

trap_cleanup() {
  stop_wifi_node || true
  stop_serial_node || true
  stop_base || true
}
