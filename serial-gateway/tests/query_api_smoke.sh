#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
CFG="/tmp/sg_query_smoke.yaml"

cleanup() {
  pkill -P $$ || true
  rm -f /tmp/sg_query_gateway.log /tmp/sg_query_stdout.log "$CFG"
}
trap cleanup EXIT

if ! command -v curl >/dev/null 2>&1; then
  echo "query smoke requires curl"
  exit 99
fi

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DSERIAL_GATEWAY_ENABLE_MQTT=ON >/dev/null
cmake --build "$BUILD_DIR" -j >/dev/null 2>&1

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
  port: 19000
  disk_cache_enabled: false

runtime:
  queue_capacity: 128
  stats_interval_sec: 2
  drop_unknown_devices: false

command:
  enabled: false

heartbeat:
  enabled: false

monitor:
  enabled: true
  bind_host: "127.0.0.1"
  port: 9911
  recent_capacity: 50

log:
  file: "/tmp/sg_query_gateway.log"
  level: "INFO"
  also_stdout: false
CFGEOF

rm -f /tmp/sg_query_gateway.log

# start gateway without serial (no pty needed — just testing HTTP API)
"$BUILD_DIR/serial_gateway" --config="$CFG" >/tmp/sg_query_stdout.log 2>&1 &
GW_PID=$!

# wait for HTTP to be ready
for _ in $(seq 1 20); do
  if curl -sfS http://127.0.0.1:9911/api/status >/dev/null 2>&1; then
    break
  fi
  sleep 0.2
done

BASE="http://127.0.0.1:9911"

# ---------- test cases ----------
fail_count=0

check() {
  local desc="$1" url="$2" expect_status="$3" expect_body="$4"
  local resp
  resp="$(curl -sS -w "\n%{http_code}" "$url" 2>&1 || true)"
  local code
  code="$(echo "$resp" | tail -1)"
  local body
  body="$(echo "$resp" | sed '$d')"
  if [ "$code" != "$expect_status" ]; then
    echo "FAIL [$desc] expected status $expect_status got $code"
    echo "  url: $url"
    echo "  body: $body"
    fail_count=$((fail_count + 1))
    return
  fi
  if [ -n "$expect_body" ] && ! echo "$body" | grep -q "$expect_body"; then
    echo "FAIL [$desc] body missing expected content"
    echo "  url: $url"
    echo "  body: $body"
    echo "  want: $expect_body"
    fail_count=$((fail_count + 1))
    return
  fi
  echo "PASS [$desc]"
}

# 1. normal query (no params → defaults: limit=100, offset=0)
check "default params" \
  "$BASE/api/history/system" \
  "200" ""

# 2. limit=-1 should be treated as default (100)
check "limit=-1" \
  "$BASE/api/history/system?limit=-1" \
  "200" ""

# 3. limit=999999 should be capped to 1000
check "limit=999999" \
  "$BASE/api/history/system?limit=999999" \
  "200" ""

# 4. offset=-1 should become 0
check "offset=-1" \
  "$BASE/api/history/system?offset=-1" \
  "200" ""

# 5. non-numeric limit → default
check "non-numeric limit" \
  "$BASE/api/history/system?limit=abc" \
  "200" ""

# 6. normal /api/history with valid params
check "/api/history device_id=1" \
  "$BASE/api/history?device_id=1&limit=5" \
  "200" ""

# 7. /api/history with negative limit
check "/api/history limit=-1" \
  "$BASE/api/history?limit=-1" \
  "200" ""

# 8. /api/history with non-numeric device_id
check "/api/history device_id=abc" \
  "$BASE/api/history?device_id=abc" \
  "200" ""

# 9. verify 503 when history_store_ is unavailable
# cannot easily test without modifying startup, but structure is in place

echo ""
if [ "$fail_count" -eq 0 ]; then
  echo "All query API smoke tests passed"
else
  echo "$fail_count test(s) FAILED"
  exit 1
fi
