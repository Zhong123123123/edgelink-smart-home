#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

start_base with_tcp
start_wifi_node --host=127.0.0.1 --port=${WIFI_PORT} --device-id=2 --period-ms=1000 --ack-mode=ok
trap trap_cleanup EXIT

sleep 4
send_wifi_cmd 301 get_status '{}' 5000 > "${LOG_DIR}/cmd_get_status.txt"
send_wifi_cmd 302 set_led '{"value":1}' 5000 > "${LOG_DIR}/cmd_set_led.txt"

curl -sf "http://127.0.0.1:${MONITOR_PORT}/api/devices" > "${LOG_DIR}/devices.json"
fetch_metrics > "${LOG_DIR}/metrics.txt"

grep -q 'OK ack device_id=2 command_id=301 result=0' "${LOG_DIR}/cmd_get_status.txt"
grep -q 'OK ack device_id=2 command_id=302 result=0' "${LOG_DIR}/cmd_set_led.txt"
grep -q '"device_id":2' "${LOG_DIR}/devices.json"
grep -q '"link_type":"wifi"' "${LOG_DIR}/devices.json"
grep -q '"wifi_connected":true' "${LOG_DIR}/devices.json"
grep -q 'wifi-node-02' "${LOG_DIR}/tcp_receiver.log"
grep -q '"link_type":"wifi"' "${LOG_DIR}/tcp_receiver.log"
grep -q 'sg_wifi_clients_connected{instance="primary"} 1' "${LOG_DIR}/metrics.txt"
grep -q 'sg_wifi_devices_online{instance="primary"} 1' "${LOG_DIR}/metrics.txt"

echo "[TEST] wifi_node_e2e PASS"
echo "logs: ${LOG_DIR}"
