#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROJECT_FILE="${ROOT_DIR}/Project/SmartHome.uvprojx"
TARGET_NAME="${TARGET_NAME:-SmartHome_SingleTask}"
HEX_OUT="${ROOT_DIR}/Project/Objects/SmartHome.hex"

USE_EXISTING=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --use-existing)
      USE_EXISTING=1
      shift
      ;;
    --target)
      TARGET_NAME="$2"
      shift 2
      ;;
    *)
      echo "[ERROR] Unknown arg: $1"
      echo "Usage: $0 [--use-existing] [--target <keil_target_name>]"
      exit 2
      ;;
  esac
done

if [[ ${USE_EXISTING} -eq 1 ]]; then
  if [[ -f "${HEX_OUT}" ]]; then
    echo "[INFO] Using existing firmware: ${HEX_OUT}"
    exit 0
  fi
  echo "[ERROR] --use-existing was set but ${HEX_OUT} does not exist."
  exit 1
fi

if command -v UV4 >/dev/null 2>&1; then
  echo "[INFO] Building with Keil CLI: UV4"
  UV4 -b "${PROJECT_FILE}" -t "${TARGET_NAME}"
  if [[ ! -f "${HEX_OUT}" ]]; then
    echo "[ERROR] Build completed but firmware not found: ${HEX_OUT}"
    exit 1
  fi
  echo "[INFO] Build done: ${HEX_OUT}"
  exit 0
fi

if command -v uvision >/dev/null 2>&1; then
  echo "[INFO] Building with Keil CLI: uvision"
  uvision -b "${PROJECT_FILE}" -t "${TARGET_NAME}"
  if [[ ! -f "${HEX_OUT}" ]]; then
    echo "[ERROR] Build completed but firmware not found: ${HEX_OUT}"
    exit 1
  fi
  echo "[INFO] Build done: ${HEX_OUT}"
  exit 0
fi

echo "[ERROR] Keil command line tool not found (UV4/uvision)."
echo "[ERROR] On Linux, use one of these options:"
echo "  1) Build in Keil on Windows and copy SmartHome.hex back."
echo "  2) Install Keil CLI via Wine and ensure UV4 is in PATH."
echo "  3) Run this script with --use-existing if hex already exists."
exit 1
