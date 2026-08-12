#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

DURATION_SEC="${1:-300}"
start_base with_tcp
start_wifi_node --host=127.0.0.1 --port=${WIFI_PORT} --device-id=2 --period-ms=500 --ack-mode=ok
trap trap_cleanup EXIT

START_TS=$(date +%s)
END_TS=$((START_TS + DURATION_SEC))
ITER=0

curl -sf http://127.0.0.1:${MONITOR_PORT}/api/status > "${LOG_DIR}/smoke_status_start.json"
RSS_START=$(grep -o '"rss_kb":[0-9]*' "${LOG_DIR}/smoke_status_start.json" | cut -d: -f2)
THR_START=$(grep -o '"threads":[0-9]*' "${LOG_DIR}/smoke_status_start.json" | cut -d: -f2)

while [[ $(date +%s) -lt ${END_TS} ]]; do
  fetch_metrics > "${LOG_DIR}/smoke_metrics_${ITER}.txt"
  send_wifi_cmd $((800 + ITER)) get_status '{}' 3000 > "${LOG_DIR}/smoke_cmd_${ITER}.txt" || true
  sleep 10
  ITER=$((ITER + 1))
done

curl -sf http://127.0.0.1:${MONITOR_PORT}/api/status > "${LOG_DIR}/smoke_status_end.json"
RSS_END=$(grep -o '"rss_kb":[0-9]*' "${LOG_DIR}/smoke_status_end.json" | cut -d: -f2)
THR_END=$(grep -o '"threads":[0-9]*' "${LOG_DIR}/smoke_status_end.json" | cut -d: -f2)

fetch_metrics > "${LOG_DIR}/smoke_metrics_final.txt"
TIMEOUTS=$(metric_value "$(cat "${LOG_DIR}/smoke_metrics_final.txt")" 'sg_command_timeout_count{instance="primary"}')
UPLOAD_FAIL=$(metric_value "$(cat "${LOG_DIR}/smoke_metrics_final.txt")" 'sg_upload_fail_total{instance="primary"}')
EVENTS=$(metric_value "$(cat "${LOG_DIR}/smoke_metrics_final.txt")" 'sg_wifi_events_received{instance="primary"}')
SUBMITS=$(metric_value "$(cat "${LOG_DIR}/smoke_metrics_final.txt")" 'sg_command_submit_count{instance="primary"}')
ACKS=$(metric_value "$(cat "${LOG_DIR}/smoke_metrics_final.txt")" 'sg_command_ack_count{instance="primary"}')

if [[ "${TIMEOUTS:-0}" -ne 0 ]]; then
  echo "smoke timeout check failed"
  exit 1
fi
if [[ "${UPLOAD_FAIL:-0}" -ne 0 ]]; then
  echo "smoke upload fail check failed"
  exit 1
fi
if [[ "${THR_END:-0}" -gt $((THR_START + 5)) ]]; then
  echo "smoke thread growth abnormal"
  exit 1
fi

echo "[TEST] wifi_stability_smoke PASS duration=${DURATION_SEC}s"
echo "wifi_events_received=${EVENTS} command_submit_count=${SUBMITS} command_ack_count=${ACKS} command_timeout_count=${TIMEOUTS} upload_fail=${UPLOAD_FAIL} rss_kb_start=${RSS_START} rss_kb_end=${RSS_END} threads_start=${THR_START} threads_end=${THR_END}"
