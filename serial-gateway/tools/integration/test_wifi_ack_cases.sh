#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

start_base with_tcp
trap trap_cleanup EXIT

start_wifi_node --host=127.0.0.1 --port=${WIFI_PORT} --device-id=2 --period-ms=1000 --ack-mode=ok
sleep 2
send_wifi_cmd 401 get_status '{}' 5000 > "${LOG_DIR}/ack_ok.txt"
grep -q 'OK ack device_id=2 command_id=401 result=0' "${LOG_DIR}/ack_ok.txt"
stop_wifi_node

start_wifi_node --host=127.0.0.1 --port=${WIFI_PORT} --device-id=2 --period-ms=1000 --ack-mode=fail --ack-result=1
sleep 2
send_wifi_cmd 402 get_status '{}' 5000 > "${LOG_DIR}/ack_fail.txt"
grep -q 'OK ack device_id=2 command_id=402 result=1' "${LOG_DIR}/ack_fail.txt"
stop_wifi_node

start_wifi_node --host=127.0.0.1 --port=${WIFI_PORT} --device-id=2 --period-ms=1000 --ack-mode=timeout
sleep 2
send_wifi_cmd 403 get_status '{}' 3000 > "${LOG_DIR}/ack_timeout.txt" || true
grep -q 'ERR ack timeout device_id=2 command_id=403' "${LOG_DIR}/ack_timeout.txt"
stop_wifi_node

start_wifi_node --host=127.0.0.1 --port=${WIFI_PORT} --device-id=2 --period-ms=1000 --ack-mode=late --ack-delay-ms=7000
sleep 2
send_wifi_cmd 404 get_status '{}' 3000 > "${LOG_DIR}/ack_late.txt" || true
grep -q 'ERR ack timeout device_id=2 command_id=404' "${LOG_DIR}/ack_late.txt"
sleep 8

fetch_metrics > "${LOG_DIR}/metrics_ack_cases.txt"
grep -q 'sg_command_submit_count{instance="primary"} 4' "${LOG_DIR}/metrics_ack_cases.txt"
grep -q 'sg_command_timeout_count{instance="primary"} 2' "${LOG_DIR}/metrics_ack_cases.txt"
grep -q 'sg_command_fail_count{instance="primary"} 1' "${LOG_DIR}/metrics_ack_cases.txt"
grep -q 'sg_command_late_ack_count{instance="primary"} 1' "${LOG_DIR}/metrics_ack_cases.txt"
grep -q '\[CMD\] late ack device_id=2 command_id=404 result=0' "${LOG_DIR}/gateway.log"

echo "[TEST] wifi_ack_cases PASS"
