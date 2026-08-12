#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"

cleanup() {
  pkill -P $$ || true
}
trap cleanup EXIT

cmake -S "$ROOT_DIR" -B "$BUILD_DIR"
cmake --build "$BUILD_DIR" -j
ctest --test-dir "$BUILD_DIR" --output-on-failure

"$ROOT_DIR/scripts/create_virtual_serial.sh" /tmp/ttyV0 /tmp/ttyV1 > /tmp/sg_socat.log 2>&1 &
SOCAT_PID=$!

for _ in $(seq 1 30); do
  [[ -e /tmp/ttyV0 && -e /tmp/ttyV1 ]] && break
  sleep 0.1
done

"$BUILD_DIR/tcp_receiver" 9000 > /tmp/sg_receiver.log 2>&1 &
"$BUILD_DIR/serial_gateway" --config="$ROOT_DIR/config/gateway.yaml" > /tmp/sg_gateway.log 2>&1 &
"$BUILD_DIR/fake_sensor" /tmp/ttyV0 200 115200 > /tmp/sg_sensor.log 2>&1 &

sleep 5

if ! grep -q '"temperature"' /tmp/sg_receiver.log; then
  echo "smoke failed: no payload received"
  echo "--- receiver log ---"
  cat /tmp/sg_receiver.log
  exit 1
fi

echo "smoke passed"
