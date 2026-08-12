#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-/dev/ttyUSB0}"
BAUD="${2:-115200}"
OUT_FILE="${3:-./uart_capture.log}"

if [[ ! -e "${PORT}" ]]; then
  echo "[ERROR] Serial port not found: ${PORT}"
  exit 1
fi

echo "[INFO] Capturing ${PORT} @ ${BAUD} to ${OUT_FILE}"
echo "[INFO] Press Ctrl+C to stop."

stty -F "${PORT}" "${BAUD}" raw -echo -echoe -echok -echoctl -echoke

exec stdbuf -oL cat "${PORT}" | awk '{ print strftime("[%Y-%m-%d %H:%M:%S]"), $0; fflush(); }' | tee "${OUT_FILE}"
