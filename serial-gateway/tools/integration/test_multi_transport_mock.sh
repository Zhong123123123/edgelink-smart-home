#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

start_base with_tcp
start_serial_node
start_wifi_node --host=127.0.0.1 --port=${WIFI_PORT} --device-id=2 --period-ms=1000 --ack-mode=ok
trap trap_cleanup EXIT

sleep 6
curl -sf http://127.0.0.1:${MONITOR_PORT}/api/devices > "${LOG_DIR}/multi_devices.json"
fetch_metrics > "${LOG_DIR}/multi_metrics.txt"

grep -q '"device_id":1' "${LOG_DIR}/multi_devices.json"
grep -q '"device_id":2' "${LOG_DIR}/multi_devices.json"
grep -q '"link_type":"serial"' "${LOG_DIR}/multi_devices.json"
grep -q '"link_type":"wifi"' "${LOG_DIR}/multi_devices.json"
grep -q '"device_id":1,"device_name":"sensor-01","link_type":"serial"' "${LOG_DIR}/tcp_receiver.log"
grep -q '"device_id":2,"device_name":"wifi-node-02","link_type":"wifi"' "${LOG_DIR}/tcp_receiver.log"
grep -q 'sg_devices_online{instance="primary"} 2' "${LOG_DIR}/multi_metrics.txt"
grep -q 'sg_serial_devices_online{instance="primary"} 1' "${LOG_DIR}/multi_metrics.txt"
grep -q 'sg_wifi_devices_online{instance="primary"} 1' "${LOG_DIR}/multi_metrics.txt"
grep -q 'sg_wifi_clients_connected{instance="primary"} 1' "${LOG_DIR}/multi_metrics.txt"

"${BUILD_DIR}/command_sender" 127.0.0.1 ${COMMAND_PORT} DEVCMD 1 601 5000 get_status > "${LOG_DIR}/multi_cmd_serial.txt"
send_wifi_cmd 602 get_status '{}' 5000 > "${LOG_DIR}/multi_cmd_wifi.txt"
grep -q 'OK ack device_id=1 command_id=601 result=0' "${LOG_DIR}/multi_cmd_serial.txt"
grep -q 'OK ack device_id=2 command_id=602 result=0' "${LOG_DIR}/multi_cmd_wifi.txt"

echo "[TEST] multi_transport_mock PASS"
