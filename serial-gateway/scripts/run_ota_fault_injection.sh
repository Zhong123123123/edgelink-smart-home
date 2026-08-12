#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

./scripts/run_ota_crc_fail.sh || true
./scripts/run_ota_disconnect.sh || true
