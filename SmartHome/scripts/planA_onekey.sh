#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

USE_EXISTING=0
SKIP_FLASH=0
SKIP_CAPTURE=0
PORT="/dev/ttyUSB0"
BAUD=115200
LOG_FILE="${ROOT_DIR}/uart_capture.log"
FLASH_METHOD="auto"
MCU="f103"
OPENOCD_CFG=""
TARGET_NAME="SmartHome_SingleTask"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --use-existing)
      USE_EXISTING=1
      shift
      ;;
    --skip-flash)
      SKIP_FLASH=1
      shift
      ;;
    --skip-capture)
      SKIP_CAPTURE=1
      shift
      ;;
    --port)
      PORT="$2"
      shift 2
      ;;
    --baud)
      BAUD="$2"
      shift 2
      ;;
    --log-file)
      LOG_FILE="$2"
      shift 2
      ;;
    --flash-method)
      FLASH_METHOD="$2"
      shift 2
      ;;
    --mcu)
      MCU="$2"
      shift 2
      ;;
    --openocd-cfg)
      OPENOCD_CFG="$2"
      shift 2
      ;;
    --target)
      TARGET_NAME="$2"
      shift 2
      ;;
    *)
      echo "[ERROR] Unknown arg: $1"
      echo "Usage: $0 [--use-existing] [--skip-flash] [--skip-capture] [--port /dev/ttyUSB0] [--baud 115200] [--log-file <path>] [--flash-method auto|openocd|stflash] [--mcu f103|f407] [--openocd-cfg <cfg>] [--target <keil_target_name>]"
      exit 2
      ;;
  esac
done

echo "[STEP] Build firmware"
if [[ ${USE_EXISTING} -eq 1 ]]; then
  "${SCRIPT_DIR}/build_firmware.sh" --use-existing --target "${TARGET_NAME}"
else
  "${SCRIPT_DIR}/build_firmware.sh" --target "${TARGET_NAME}"
fi

if [[ ${SKIP_FLASH} -eq 0 ]]; then
  echo "[STEP] Flash firmware"
  FLASH_ARGS=(--method "${FLASH_METHOD}" --mcu "${MCU}")
  if [[ -n "${OPENOCD_CFG}" ]]; then
    FLASH_ARGS+=(--openocd-cfg "${OPENOCD_CFG}")
  fi
  "${SCRIPT_DIR}/flash_firmware.sh" "${FLASH_ARGS[@]}"
else
  echo "[INFO] Flash skipped."
fi

if [[ ${SKIP_CAPTURE} -eq 0 ]]; then
  echo "[STEP] Start UART capture"
  "${SCRIPT_DIR}/capture_uart_log.sh" "${PORT}" "${BAUD}" "${LOG_FILE}"
else
  echo "[INFO] UART capture skipped."
fi
