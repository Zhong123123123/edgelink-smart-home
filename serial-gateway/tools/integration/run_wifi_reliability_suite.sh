#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INCLUDE_SMOKE="false"
if [[ "${1:-}" == "--include-smoke" ]]; then
  INCLUDE_SMOKE="true"
fi

run_case() {
  local name="$1"
  local cmd="$2"
  if eval "${cmd}"; then
    echo "${name}: PASS"
  else
    echo "${name}: FAIL"
    return 1
  fi
}

run_case wifi_node_e2e "${DIR}/test_wifi_node.sh"
run_case wifi_ack_cases "${DIR}/test_wifi_ack_cases.sh"
run_case wifi_reconnect "${DIR}/test_wifi_reconnect.sh"
run_case wifi_json_robustness "${DIR}/test_wifi_json_robustness.sh"
run_case command_key_isolation "${DIR}/test_command_key_isolation.sh"
run_case multi_transport_mock "${DIR}/test_multi_transport_mock.sh"
run_case wifi_weak_network_cache "${DIR}/test_wifi_weak_network_cache.sh"

if [[ "${INCLUDE_SMOKE}" == "true" ]]; then
  run_case wifi_stability_smoke "${DIR}/test_wifi_stability_smoke.sh"
  SMOKE_RES="PASS"
else
  SMOKE_RES="SKIPPED"
fi

echo "========== WiFi Reliability Test Summary =========="
echo "wifi_node_e2e: PASS"
echo "wifi_ack_cases: PASS"
echo "wifi_reconnect: PASS"
echo "wifi_json_robustness: PASS"
echo "command_key_isolation: PASS"
echo "multi_transport_mock: PASS"
echo "wifi_weak_network_cache: PASS"
echo "wifi_stability_smoke: ${SMOKE_RES}"
echo "=================================================="
