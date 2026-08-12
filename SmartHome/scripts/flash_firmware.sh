#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
HEX_FILE="${ROOT_DIR}/Project/Objects/SmartHome.hex"
BIN_FILE="${ROOT_DIR}/Project/Objects/SmartHome.bin"
METHOD="auto"
MCU="f103"
OPENOCD_CFG=""
FLASH_BASE_ADDR="0x08000000"

set_default_openocd_cfg() {
  case "${MCU}" in
    f103) OPENOCD_CFG="board/stm32f1discovery.cfg" ;;
    f407) OPENOCD_CFG="board/stm32f4discovery.cfg" ;;
    *)
      echo "[ERROR] Unsupported MCU profile: ${MCU}"
      exit 2
      ;;
  esac
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --hex)
      HEX_FILE="$2"
      shift 2
      ;;
    --method)
      METHOD="$2"
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
    --base-addr)
      FLASH_BASE_ADDR="$2"
      shift 2
      ;;
    *)
      echo "[ERROR] Unknown arg: $1"
      echo "Usage: $0 [--hex <file>] [--method auto|openocd|stflash] [--mcu f103|f407] [--openocd-cfg <cfg>] [--base-addr <addr>]"
      exit 2
      ;;
  esac
done

if [[ -z "${OPENOCD_CFG}" ]]; then
  set_default_openocd_cfg
fi

if [[ ! -f "${HEX_FILE}" ]]; then
  echo "[ERROR] Firmware file not found: ${HEX_FILE}"
  exit 1
fi

to_bin_if_needed() {
  if [[ "${HEX_FILE}" == *.hex ]]; then
    if ! command -v arm-none-eabi-objcopy >/dev/null 2>&1; then
      echo "[ERROR] arm-none-eabi-objcopy not found; cannot convert hex to bin for st-flash."
      return 1
    fi
    arm-none-eabi-objcopy -I ihex -O binary "${HEX_FILE}" "${BIN_FILE}"
    echo "${BIN_FILE}"
    return 0
  fi
  echo "${HEX_FILE}"
  return 0
}

flash_openocd() {
  if ! command -v openocd >/dev/null 2>&1; then
    return 1
  fi
  echo "[INFO] Flashing via openocd (${OPENOCD_CFG})..."
  openocd -f "${OPENOCD_CFG}" \
    -c "init; reset halt; program ${HEX_FILE} verify reset exit"
}

flash_stflash() {
  if ! command -v st-flash >/dev/null 2>&1; then
    return 1
  fi
  local image
  image="$(to_bin_if_needed)" || return 1
  echo "[INFO] Flashing via st-flash..."
  st-flash --reset write "${image}" "${FLASH_BASE_ADDR}"
}

case "${METHOD}" in
  openocd)
    flash_openocd
    ;;
  stflash)
    flash_stflash
    ;;
  auto)
    if command -v openocd >/dev/null 2>&1; then
      flash_openocd
    elif command -v st-flash >/dev/null 2>&1; then
      flash_stflash
    else
      echo "[ERROR] Neither openocd nor st-flash is available."
      exit 1
    fi
    ;;
  *)
    echo "[ERROR] Invalid --method: ${METHOD}"
    exit 2
    ;;
esac

echo "[INFO] Flash done."
