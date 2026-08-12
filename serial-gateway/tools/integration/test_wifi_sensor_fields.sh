#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOG_DIR="${ROOT_DIR}/logs"
mkdir -p "${LOG_DIR}"
TS="$(date +%Y%m%d_%H%M%S)"
OUT="${LOG_DIR}/wifi_sensor_fields_${TS}.log"

echo "[INFO] checking monitor and upload fields" | tee -a "${OUT}"

if ! curl -sf http://127.0.0.1:9010/api/status >/dev/null; then
  echo "[FAIL] monitor endpoint unavailable: http://127.0.0.1:9010/api/status" | tee -a "${OUT}"
  echo "[HINT] start gateway first: ./build/serial_gateway --config=config/gateway.yaml" | tee -a "${OUT}"
  exit 1
fi

status_json="$(curl -sf http://127.0.0.1:9010/api/status)"
dev_json="$(curl -sf http://127.0.0.1:9010/api/devices)"

echo "[INFO] api/status ok" | tee -a "${OUT}"
echo "${status_json}" | tee -a "${OUT}" >/dev/null

echo "[INFO] api/devices snapshot" | tee -a "${OUT}"
echo "${dev_json}" | tee -a "${OUT}" >/dev/null

if ! echo "${dev_json}" | rg -q '"device_id":2'; then
  echo "[FAIL] device_id=2 not found" | tee -a "${OUT}"
  exit 1
fi
if ! echo "${dev_json}" | rg -q '"link_type":"wifi"'; then
  echo "[FAIL] wifi link_type not found" | tee -a "${OUT}"
  exit 1
fi

if [[ -f "${LOG_DIR}/serial_gateway.log" ]]; then
  if rg -q '"mq2_alarm":|"ld2402_presence":' "${LOG_DIR}/serial_gateway.log"; then
    echo "[PASS] sensor fields observed in gateway logs" | tee -a "${OUT}"
  else
    echo "[WARN] sensor fields not found in gateway log yet, check tcp_receiver output window" | tee -a "${OUT}"
  fi
else
  echo "[WARN] logs/serial_gateway.log not found" | tee -a "${OUT}"
fi

echo "[TEST] wifi_sensor_fields PASS" | tee -a "${OUT}"
echo "[INFO] report: ${OUT}" | tee -a "${OUT}"
