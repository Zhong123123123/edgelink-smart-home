#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

echo "[1/3] monitor smoke"
"$ROOT_DIR/tests/monitor_smoke.sh"

echo "[2/3] control smoke"
"$ROOT_DIR/tests/control_smoke.sh"

echo "[3/3] ota api smoke"
"$ROOT_DIR/tests/ota_api_smoke.sh"

echo "full upgrade smoke passed"
