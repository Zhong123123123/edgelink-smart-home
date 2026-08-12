#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

start_base with_tcp
trap trap_cleanup EXIT

exec 3<>/dev/tcp/127.0.0.1/${WIFI_PORT}

# 1/2/3 valid events
printf '%s\n' '{"device_id":2,"device_name":"wifi-node-02","event_type":"sensor_data","timestamp":0,"temperature":27.1,"humidity":45.2,"voltage":3.3,"status":1,"link_type":"wifi","wifi_rssi":-55,"seq":9001}' >&3
printf '%s\n' '{"device_id":2,"device_name":"wifi-node-02","event_type":"heartbeat","timestamp":0,"link_type":"wifi","wifi_rssi":-55,"seq":9002}' >&3
printf '%s\n' '{"device_id":2,"device_name":"wifi-node-02","event_type":"command_ack","timestamp":0,"command_id":777,"command_result":0,"payload_summary":"cmd_id=777,result=0","link_type":"wifi","wifi_rssi":-55,"seq":9003}' >&3

# 5 missing device_id
printf '%s\n' '{"device_name":"wifi-node-02","event_type":"sensor_data","timestamp":0}' >&3
# 6 unknown device_id
printf '%s\n' '{"device_id":999,"device_name":"unknown","event_type":"sensor_data","timestamp":0}' >&3
# 7 missing event_type
printf '%s\n' '{"device_id":2,"device_name":"wifi-node-02","timestamp":0}' >&3
# 8 invalid json
printf '%s\n' 'NOT_A_JSON' >&3

# 9 half packet + multi-line sticky
printf '%s' '{"device_id":2,"device_name":"wifi-node-02","event_type":"sensor_data","timestamp":0,"temperature":28.2,' >&3
sleep 1
printf '%s\n' '"humidity":46.2,"voltage":3.3,"status":1,"link_type":"wifi","wifi_rssi":-50,"seq":9004}' >&3
printf '%s\n%s\n' '{"event_type":"heartbeat","device_name":"wifi-node-02","device_id":2,"timestamp":0,"link_type":"wifi","wifi_rssi":-54,"seq":9005}' '{"device_name":"wifi-node-02","device_id":2,"timestamp":0,"event_type":"sensor_data","humidity":44.2,"temperature":26.6,"status":1,"voltage":3.3,"link_type":"wifi","wifi_rssi":-53,"seq":9006}' >&3

sleep 3
curl -sf http://127.0.0.1:${MONITOR_PORT}/api/status > "${LOG_DIR}/json_status.json"
fetch_metrics > "${LOG_DIR}/json_metrics.txt"

# gateway alive + parse metrics present
grep -q '"uptime_sec"' "${LOG_DIR}/json_status.json"
grep -q 'sg_wifi_json_parse_ok{instance="primary"}' "${LOG_DIR}/json_metrics.txt"
grep -q 'sg_wifi_json_parse_fail{instance="primary"}' "${LOG_DIR}/json_metrics.txt"
grep -q 'sg_wifi_unknown_device{instance="primary"}' "${LOG_DIR}/json_metrics.txt"
grep -q 'sg_wifi_events_received{instance="primary"}' "${LOG_DIR}/json_metrics.txt"

# expected warnings
grep -q 'wifi json parse failed' "${LOG_DIR}/gateway.log"

# timestamp should be patched (no zero timestamp for uploaded events)
if grep -q '"timestamp":0' "${LOG_DIR}/tcp_receiver.log"; then
  echo "timestamp patch check failed"
  exit 1
fi

echo "[TEST] wifi_json_robustness PASS"
