#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
RUN_ONE="${ROOT_DIR}/tools/integration/test_ota_over_tcp_binary.sh"
OUT_DIR="${OUT_DIR:-/tmp/sg_ota_over_tcp_binary_fault_matrix}"
mkdir -p "${OUT_DIR}"

run_case() {
  local name="$1"
  local expect="$2"
  local mock_args="$3"
  local idx="$4"
  local case_log="${OUT_DIR}/${name}.log"
  local base=$((19400 + idx * 20))
  local up_port=$((base))
  local cmd_port=$((base + 1))
  local mon_port=$((base + 10))
  local tcpb_port=$((base + 11))
  echo "[CASE] ${name} expect=${expect} mock_args='${mock_args}'"
  local ok=0
  for attempt in 1 2; do
    if LOG_DIR="/tmp/sg_ota_over_tcp_binary_${name}" \
      UPLOAD_PORT="${up_port}" \
      COMMAND_PORT="${cmd_port}" \
      MONITOR_PORT="${mon_port}" \
      TCP_BIN_PORT="${tcpb_port}" \
      EXPECT_RESULT="${expect}" MOCK_ARGS="${mock_args}" \
      "${RUN_ONE}" >>"${case_log}" 2>&1; then
      ok=1
      break
    fi
    sleep 0.5
  done
  if [[ "${ok}" -eq 1 ]]; then
    echo "PASS" > "${OUT_DIR}/${name}.result"
  else
    echo "FAIL" > "${OUT_DIR}/${name}.result"
  fi
}

run_case "success_baseline" "SUCCESS" "" 0
run_case "fault_nack_seq3" "SUCCESS" "--nack-seq 3" 1
run_case "fault_verify_nack" "FAILED_OR_CANCELED" "--verify-nack" 2
run_case "fault_disconnect_seq3" "FAILED_OR_CANCELED" "--disconnect-at-seq 3" 3

{
  echo "# OTA over tcp_binary Fault Matrix"
  echo
  echo "| case | result |"
  echo "|---|---|"
  for c in success_baseline fault_nack_seq3 fault_verify_nack fault_disconnect_seq3; do
    r="$(cat "${OUT_DIR}/${c}.result" 2>/dev/null || echo FAIL)"
    echo "| ${c} | ${r} |"
  done
} > "${OUT_DIR}/report.md"

cat "${OUT_DIR}/report.md"
