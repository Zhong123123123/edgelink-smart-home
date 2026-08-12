#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOG_DIR="${ROOT_DIR}/logs"
mkdir -p "${LOG_DIR}"

DURATION_SEC="${1:-1800}"      # default 30 min
SAMPLE_SEC="${2:-10}"           # metrics sample interval
CMD_EVERY_SEC="${3:-180}"       # send WiFi command every N sec
MONITOR_URL="${MONITOR_URL:-http://127.0.0.1:9010}"
CMD_HOST="${CMD_HOST:-127.0.0.1}"
CMD_PORT="${CMD_PORT:-9001}"
WIFI_DEVICE_ID="${WIFI_DEVICE_ID:-2}"

TS="$(date +%Y%m%d_%H%M%S)"
OUT_LOG="${LOG_DIR}/hw_stability_${TS}.log"
METRIC_LOG="${LOG_DIR}/hw_stability_metrics_${TS}.log"
CMD_LOG="${LOG_DIR}/hw_stability_cmd_${TS}.log"
SUMMARY_LOG="${LOG_DIR}/hw_stability_summary_${TS}.txt"

{
  echo "[INFO] start=$(date '+%F %T') duration_sec=${DURATION_SEC} sample_sec=${SAMPLE_SEC} cmd_every_sec=${CMD_EVERY_SEC}"
  echo "[INFO] monitor_url=${MONITOR_URL} cmd=${CMD_HOST}:${CMD_PORT} wifi_device_id=${WIFI_DEVICE_ID}"
  echo "[INFO] logs:"
  echo "  - ${OUT_LOG}"
  echo "  - ${METRIC_LOG}"
  echo "  - ${CMD_LOG}"
  echo "  - ${SUMMARY_LOG}"
} | tee -a "${OUT_LOG}"

if ! curl -sf "${MONITOR_URL}/metrics" >/dev/null; then
  echo "[ERROR] monitor not reachable: ${MONITOR_URL}" | tee -a "${OUT_LOG}"
  exit 1
fi

start_ts=$(date +%s)
end_ts=$((start_ts + DURATION_SEC))
next_cmd_ts=$((start_ts + CMD_EVERY_SEC))
cmd_id=10000

sample_metrics() {
  local now_human
  now_human="$(date '+%F %T')"
  local m
  if ! m="$(curl -sf "${MONITOR_URL}/metrics")"; then
    echo "${now_human} [WARN] metrics fetch failed" | tee -a "${OUT_LOG}"
    return 0
  fi
  {
    echo "===== ${now_human} ====="
    echo "${m}" | rg 'sg_devices_online|sg_serial_devices_online|sg_wifi_devices_online|sg_wifi_clients_connected|sg_upload_fail_total|sg_command_submit_count|sg_command_ack_count|sg_command_timeout_count|sg_command_fail_count|sg_command_late_ack_count|sg_wifi_events_received' || true
  } | tee -a "${METRIC_LOG}" "${OUT_LOG}"
}

send_wifi_cmd() {
  local payload
  payload='{"type":"command","device_id":'"${WIFI_DEVICE_ID}"',"command_id":'"${cmd_id}"',"command_type":"get_status","params":{},"timeout_ms":5000}'
  local resp
  if resp="$(${ROOT_DIR}/build/command_sender "${CMD_HOST}" "${CMD_PORT}" RAW "${payload}" 2>&1)"; then
    echo "$(date '+%F %T') [CMD] id=${cmd_id} ${resp}" | tee -a "${CMD_LOG}" "${OUT_LOG}"
  else
    echo "$(date '+%F %T') [CMD] id=${cmd_id} FAIL ${resp}" | tee -a "${CMD_LOG}" "${OUT_LOG}"
  fi
  cmd_id=$((cmd_id + 1))
}

# initial sample
sample_metrics

while [[ $(date +%s) -lt ${end_ts} ]]; do
  now=$(date +%s)
  if [[ ${now} -ge ${next_cmd_ts} ]]; then
    send_wifi_cmd
    next_cmd_ts=$((now + CMD_EVERY_SEC))
  fi
  sample_metrics
  sleep "${SAMPLE_SEC}"
done

# final sample + summary
sample_metrics

last_metrics="$(tail -n 200 "${METRIC_LOG}" | rg 'sg_devices_online|sg_serial_devices_online|sg_wifi_devices_online|sg_wifi_clients_connected|sg_upload_fail_total|sg_command_submit_count|sg_command_ack_count|sg_command_timeout_count|sg_command_fail_count|sg_command_late_ack_count|sg_wifi_events_received' | tail -n 20)"
{
  echo "[SUMMARY] end=$(date '+%F %T')"
  echo "[SUMMARY] duration_sec=${DURATION_SEC}"
  echo "[SUMMARY] last_metrics:"
  echo "${last_metrics}"
} | tee -a "${SUMMARY_LOG}" "${OUT_LOG}"

echo "[DONE] stability capture finished" | tee -a "${OUT_LOG}"
