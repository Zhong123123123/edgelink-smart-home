#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

start_base with_tcp
start_wifi_node --host=127.0.0.1 --port=${WIFI_PORT} --device-id=2 --period-ms=1000 --ack-mode=ok --disconnect-after-sec=5 --reconnect=true --reconnect-delay-ms=2000 --max-reconnects=1
trap trap_cleanup EXIT

sleep 3
fetch_metrics > "${LOG_DIR}/reconnect_metrics_1.txt"
grep -q 'sg_wifi_clients_connected{instance="primary"} 1' "${LOG_DIR}/reconnect_metrics_1.txt"
grep -q 'sg_wifi_devices_online{instance="primary"} 1' "${LOG_DIR}/reconnect_metrics_1.txt"

SEEN_DOWN=0
for _ in {1..8}; do
  fetch_metrics > "${LOG_DIR}/reconnect_metrics_2.txt"
  if grep -q 'sg_wifi_clients_connected{instance="primary"} 0' "${LOG_DIR}/reconnect_metrics_2.txt"; then
    SEEN_DOWN=1
    break
  fi
  sleep 1
done
if [[ "${SEEN_DOWN}" -ne 1 ]]; then
  echo "did not observe disconnect window"
  exit 1
fi
curl -sf http://127.0.0.1:${MONITOR_PORT}/api/devices > "${LOG_DIR}/reconnect_devices_2.json"
grep -q '"wifi_connected":false' "${LOG_DIR}/reconnect_devices_2.json"

SEEN_UP=0
for _ in {1..8}; do
  fetch_metrics > "${LOG_DIR}/reconnect_metrics_3.txt"
  if grep -q 'sg_wifi_clients_connected{instance="primary"} 1' "${LOG_DIR}/reconnect_metrics_3.txt"; then
    SEEN_UP=1
    break
  fi
  sleep 1
done
if [[ "${SEEN_UP}" -ne 1 ]]; then
  echo "did not observe reconnect window"
  exit 1
fi
grep -q 'sg_wifi_devices_online{instance="primary"} 1' "${LOG_DIR}/reconnect_metrics_3.txt"
curl -sf http://127.0.0.1:${MONITOR_PORT}/api/devices > "${LOG_DIR}/reconnect_devices_3.json"
grep -q '"online":true' "${LOG_DIR}/reconnect_devices_3.json"
grep -q '"wifi_reconnects":1' "${LOG_DIR}/reconnect_devices_3.json"

send_wifi_cmd 451 get_status '{}' 5000 > "${LOG_DIR}/reconnect_cmd.txt"
grep -q 'OK ack device_id=2 command_id=451 result=0' "${LOG_DIR}/reconnect_cmd.txt"

echo "[TEST] wifi_reconnect PASS"
