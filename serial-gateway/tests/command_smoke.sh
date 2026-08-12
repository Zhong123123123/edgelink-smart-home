#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
CFG="/tmp/sg_command_smoke.yaml"

cleanup() {
  pkill -P $$ || true
}
trap cleanup EXIT

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DSERIAL_GATEWAY_ENABLE_MQTT=ON
cmake --build "$BUILD_DIR" -j

cat > "$CFG" <<'CFGEOF'
serial:
  device: "/tmp/ttyV1"
  baudrate: 115200
  data_bits: 8
  parity: "N"
  stop_bits: 1
  read_chunk_size: 256

uploader:
  type: "tcp"
  host: "127.0.0.1"
  port: 9000
  disk_cache_enabled: false

runtime:
  queue_capacity: 128
  stats_interval_sec: 2
  drop_unknown_devices: false

command:
  enabled: true
  bind_host: "127.0.0.1"
  port: 9001
  client_timeout_ms: 100

log:
  file: "/tmp/sg_command_gateway.log"
  level: "INFO"
  also_stdout: false
CFGEOF

rm -f /tmp/sg_command_serial.log /tmp/sg_command_gateway.log

"$BUILD_DIR/pty_bridge" /tmp/ttyV0 /tmp/ttyV1 >/tmp/sg_command_bridge.log 2>&1 &
for _ in $(seq 1 30); do
  [[ -e /tmp/ttyV0 && -e /tmp/ttyV1 ]] && break
  sleep 0.1
done

stty -F /tmp/ttyV0 raw -echo || true

stdbuf -o0 cat /tmp/ttyV0 >/tmp/sg_command_serial.log 2>&1 &
"$BUILD_DIR/serial_gateway" --config="$CFG" >/tmp/sg_command_gateway_stdout.log 2>&1 &
sleep 1

RESP=$("$BUILD_DIR/command_sender" 127.0.0.1 9001 TEXT HELLO)
if [[ "$RESP" != *"OK wrote="* ]]; then
  echo "command smoke failed: sender response unexpected"
  echo "$RESP"
  exit 1
fi

sleep 1
if ! grep -q 'HELLO' /tmp/sg_command_serial.log; then
  echo "command smoke failed: serial peer did not receive HELLO"
  echo "--- serial log ---"
  cat /tmp/sg_command_serial.log || true
  exit 2
fi

echo "command smoke passed"
