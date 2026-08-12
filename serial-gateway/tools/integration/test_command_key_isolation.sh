#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

start_base with_tcp
start_serial_node
start_wifi_node --host=127.0.0.1 --port=${WIFI_PORT} --device-id=2 --period-ms=1000 --ack-mode=ok
trap trap_cleanup EXIT

sleep 3
"${BUILD_DIR}/command_sender" 127.0.0.1 ${COMMAND_PORT} DEVCMD 1 500 5000 get_status > "${LOG_DIR}/iso_serial.txt" &
PID1=$!
"${BUILD_DIR}/command_sender" 127.0.0.1 ${COMMAND_PORT} RAW '{"type":"command","device_id":2,"command_id":500,"command_type":"get_status","params":{},"timeout_ms":5000}' > "${LOG_DIR}/iso_wifi.txt" &
PID2=$!
wait ${PID1}
wait ${PID2}

grep -q 'OK ack device_id=1 command_id=500 result=0' "${LOG_DIR}/iso_serial.txt"
grep -q 'OK ack device_id=2 command_id=500 result=0' "${LOG_DIR}/iso_wifi.txt"

echo "[TEST] command_key_isolation PASS"
