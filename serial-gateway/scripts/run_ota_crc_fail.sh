#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

./build/fake_bootloader --port 19090 --verify-fail > tmp_demo/fake_bl_crc_fail.log 2>&1 &
BL_PID=$!
trap 'kill $BL_PID >/dev/null 2>&1 || true' EXIT
sleep 0.2

./build/otactl ota start --device-id 1 --firmware-id stm32f407-smarthome-1.0.1 --target 127.0.0.1:19090
