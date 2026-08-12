#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SMARTHOME_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_DIR="$(cd "${SMARTHOME_DIR}/.." && pwd)"
PKG_TOOL="${REPO_DIR}/serial-gateway/tools/package_firmware.py"

ENTRY_ADDR="${ENTRY_ADDR:-0x08020000}"
SLOT_SIZE="${SLOT_SIZE:-393216}"
DEVICE_TYPE="${DEVICE_TYPE:-stm32f407-smarthome}"
VERSION=""
BIN_PATH=""
MAP_PATH=""
HEX_PATH=""
OUT_DIR=""

usage() {
  cat <<USAGE
Usage:
  $0 --version <x.y.z> --bin <app.bin> --map <app.map> [--hex <app.hex>] [--out <dir>] [--device-type <type>] [--entry <addr>] [--slot-size <bytes>]

Default:
  --device-type ${DEVICE_TYPE}
  --entry ${ENTRY_ADDR}
  --slot-size ${SLOT_SIZE}
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) VERSION="$2"; shift 2;;
    --bin) BIN_PATH="$2"; shift 2;;
    --map) MAP_PATH="$2"; shift 2;;
    --hex) HEX_PATH="$2"; shift 2;;
    --out) OUT_DIR="$2"; shift 2;;
    --device-type) DEVICE_TYPE="$2"; shift 2;;
    --entry) ENTRY_ADDR="$2"; shift 2;;
    --slot-size) SLOT_SIZE="$2"; shift 2;;
    -h|--help) usage; exit 0;;
    *) echo "[ERROR] unknown arg: $1"; usage; exit 2;;
  esac
done

if [[ -z "${VERSION}" || -z "${BIN_PATH}" || -z "${MAP_PATH}" ]]; then
  echo "[ERROR] --version/--bin/--map are required"
  usage
  exit 2
fi

if [[ ! -f "${BIN_PATH}" ]]; then
  echo "[ERROR] bin not found: ${BIN_PATH}"; exit 1
fi
if [[ ! -f "${MAP_PATH}" ]]; then
  echo "[ERROR] map not found: ${MAP_PATH}"; exit 1
fi
if [[ ! -f "${PKG_TOOL}" ]]; then
  echo "[ERROR] package tool not found: ${PKG_TOOL}"; exit 1
fi

BIN_SIZE=$(wc -c < "${BIN_PATH}")
if (( BIN_SIZE > SLOT_SIZE )); then
  echo "[ERROR] app.bin size ${BIN_SIZE} exceeds slot-size ${SLOT_SIZE}"
  exit 1
fi

echo "[INFO] app.bin size=${BIN_SIZE} bytes, slot-size=${SLOT_SIZE} bytes"

if rg -n "Load Region|Execution Region|ER_IROM1|LR_IROM1|${ENTRY_ADDR}" "${MAP_PATH}" >/dev/null 2>&1; then
  echo "[INFO] map file indicates APP link region includes ${ENTRY_ADDR}"
else
  echo "[WARN] map file did not match expected ${ENTRY_ADDR}; check Keil scatter/linker settings"
fi

if [[ -z "${OUT_DIR}" ]]; then
  OUT_DIR="${REPO_DIR}/serial-gateway/dist/${DEVICE_TYPE}-${VERSION}"
fi

CMD=(python3 "${PKG_TOOL}" --device-type "${DEVICE_TYPE}" --version "${VERSION}" --bin "${BIN_PATH}" --entry "${ENTRY_ADDR}" --slot-size "${SLOT_SIZE}" --out "${OUT_DIR}")
if [[ -n "${HEX_PATH}" ]]; then
  if [[ ! -f "${HEX_PATH}" ]]; then
    echo "[ERROR] hex not found: ${HEX_PATH}"; exit 1
  fi
  CMD+=(--hex "${HEX_PATH}")
fi
CMD+=(--map "${MAP_PATH}")

"${CMD[@]}"

echo "[INFO] OTA package generated at: ${OUT_DIR}"
cat <<EOF2
[NEXT]
1) Flash bootloader image to 0x08000000 region.
2) Flash APP image (or keep current APP) with link address ${ENTRY_ADDR}.
3) Register package on gateway:
   ./build/otactl firmware add --manifest ${OUT_DIR}/manifest.json --image ${OUT_DIR}/app.bin
4) Run OTA simulation first, then hardware validation.
EOF2
