#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

start_base without_tcp
start_wifi_node --host=127.0.0.1 --port=${WIFI_PORT} --device-id=2 --period-ms=300 --ack-mode=ok
trap trap_cleanup EXIT

sleep 5
fetch_metrics > "${LOG_DIR}/weak_metrics_during.txt"
UPLOAD_FAIL=$(metric_value "$(cat "${LOG_DIR}/weak_metrics_during.txt")" 'sg_upload_fail_total{instance="primary"}')
CACHE_ENQ=$(metric_value "$(cat "${LOG_DIR}/weak_metrics_during.txt")" 'sg_cache_enqueue_total{instance="primary"}')
CACHE_BACKLOG=$(metric_value "$(cat "${LOG_DIR}/weak_metrics_during.txt")" 'sg_cache_backlog{instance="primary"}')

if [[ "${UPLOAD_FAIL:-0}" -le 0 || "${CACHE_ENQ:-0}" -le 0 || "${CACHE_BACKLOG:-0}" -le 0 ]]; then
  echo "weak network during metrics check failed"
  cat "${LOG_DIR}/weak_metrics_during.txt"
  exit 1
fi

"${BUILD_DIR}/tcp_receiver" "${UPLOAD_PORT}" > "${LOG_DIR}/tcp_receiver.log" 2>&1 &
TCP_PID=$!

for _ in {1..20}; do
  sleep 1
  fetch_metrics > "${LOG_DIR}/weak_metrics_after.txt"
  BACKLOG=$(metric_value "$(cat "${LOG_DIR}/weak_metrics_after.txt")" 'sg_cache_backlog{instance="primary"}')
  if [[ "${BACKLOG:-1}" -eq 0 ]]; then
    break
  fi
done

CACHE_REPLAY_OK=$(metric_value "$(cat "${LOG_DIR}/weak_metrics_after.txt")" 'sg_cache_replay_ok_total{instance="primary"}')
BACKLOG=$(metric_value "$(cat "${LOG_DIR}/weak_metrics_after.txt")" 'sg_cache_backlog{instance="primary"}')
if [[ "${CACHE_REPLAY_OK:-0}" -le 0 || "${BACKLOG:-1}" -ne 0 ]]; then
  echo "cache replay check failed"
  cat "${LOG_DIR}/weak_metrics_after.txt"
  exit 1
fi

grep -q 'wifi-node-02' "${LOG_DIR}/tcp_receiver.log"

echo "[TEST] wifi_weak_network_cache PASS"
echo "during: upload_fail=${UPLOAD_FAIL} cache_enqueue=${CACHE_ENQ} cache_backlog=${CACHE_BACKLOG}"
echo "after: cache_replay_ok=${CACHE_REPLAY_OK} cache_backlog=${BACKLOG}"
