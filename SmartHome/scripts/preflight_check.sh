#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

fail() {
  echo "[FAIL] $1" >&2
  exit 1
}

pass() {
  echo "[PASS] $1"
}

require_file() {
  local path="$1"
  [[ -f "${path}" ]] || fail "missing file: ${path}"
}

require_file "${ROOT_DIR}/bootloader/bootloader_config.h"
require_file "${ROOT_DIR}/bootloader/bootloader_main.c"
require_file "${ROOT_DIR}/1_App/main.c"
require_file "${ROOT_DIR}/1_App/app_sensor.c"
require_file "${ROOT_DIR}/1_App/app_key.c"
require_file "${ROOT_DIR}/1_App/app_led.c"

if rg -n '^#define BL_DEV_BYPASS_CRC[[:space:]]+0U$' "${ROOT_DIR}/bootloader/bootloader_config.h" >/dev/null; then
  pass "bootloader CRC bypass disabled"
else
  fail "BL_DEV_BYPASS_CRC must be 0U for delivery"
fi

if rg -n '^#define GW_APP_CONFIRM_DELAY_MS[[:space:]]+30000U$' "${ROOT_DIR}/1_App/app_gateway.c" >/dev/null; then
  pass "ota confirm delay extended to 30s"
else
  fail "GW_APP_CONFIRM_DELAY_MS must be 30000U"
fi

if rg -n 'gw_can_confirm_slot' "${ROOT_DIR}/1_App/app_gateway.c" >/dev/null \
  && rg -n 'Watchdog_IsTaskFresh' "${ROOT_DIR}/1_App/app_gateway.c" >/dev/null; then
  pass "ota confirm guarded by subsystem health"
else
  fail "ota confirm health gate missing"
fi

if rg -n '^#define BL_DEBUG_FORCE_BOOT_SLOT[[:space:]]+0xFFU$' "${ROOT_DIR}/bootloader/bootloader_config.h" >/dev/null; then
  pass "bootloader debug force slot disabled"
else
  fail "BL_DEBUG_FORCE_BOOT_SLOT must remain 0xFFU"
fi

if rg -n '#if BL_DEV_BYPASS_CRC' "${ROOT_DIR}/bootloader/bootloader_main.c" >/dev/null; then
  pass "bootloader retains explicit prod/dev branch"
else
  fail "bootloader main missing BL_DEV_BYPASS_CRC branch"
fi

storage_line="$(rg -n 'vStartStorageTasks\(384, 2\);' "${ROOT_DIR}/1_App/main.c" | head -n1 | cut -d: -f1)"
fault_line="$(rg -n 'FaultDiag_InitAtBoot\(\);' "${ROOT_DIR}/1_App/main.c" | head -n1 | cut -d: -f1)"
[[ -n "${storage_line}" && -n "${fault_line}" ]] || fail "cannot locate startup order in main.c"
if (( storage_line < fault_line )); then
  pass "storage starts before fault replay"
else
  fail "storage must start before FaultDiag_InitAtBoot"
fi

if rg -n 'reported_valid = 0U;' "${ROOT_DIR}/1_App/app_sensor.c" >/dev/null \
  && rg -n 'SmartHomeState_SetSensor\(' "${ROOT_DIR}/1_App/app_sensor.c" >/dev/null; then
  pass "sensor failure path propagates invalid state"
else
  fail "sensor failure path does not propagate invalid state"
fi

if rg -n 'if\(keyDev == NULL\)' "${ROOT_DIR}/1_App/app_key.c" >/dev/null; then
  pass "key task guards null device"
else
  fail "key task missing null-device guard"
fi

if rg -n 'if \(ledDev == NULL\)' "${ROOT_DIR}/1_App/app_led.c" >/dev/null; then
  pass "led task guards null device"
else
  fail "led task missing null-device guard"
fi

if rg -n 'SmartHomeState_SetManualLed' "${ROOT_DIR}/1_App/app_gateway.c" >/dev/null \
  && rg -n 'SmartHomeState_SetManualBuzzer' "${ROOT_DIR}/1_App/app_gateway.c" >/dev/null \
  && rg -n 'SmartHomeState_SetManualLed' "${ROOT_DIR}/1_App/app_mqtt.c" >/dev/null \
  && rg -n 'SmartHomeState_SetManualBuzzer' "${ROOT_DIR}/1_App/app_mqtt.c" >/dev/null \
  && rg -n 'Driver_Beep_WriteStatus' "${ROOT_DIR}/1_App/app_alarm.c" >/dev/null; then
  pass "manual requests are separated from actuator execution"
else
  fail "manual request / actuator separation missing"
fi

echo "[INFO] preflight checks completed"
