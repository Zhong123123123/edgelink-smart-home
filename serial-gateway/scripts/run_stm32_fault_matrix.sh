#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

OUT_DIR="${1:-$ROOT_DIR/tmp_fault_matrix}"
FIRMWARE_ID="${FIRMWARE_ID:-stm32f407-smarthome-1.1.0-a1}"
DEVICE_ID="${DEVICE_ID:-1}"
DEVICE_TYPE="${DEVICE_TYPE:-stm32f407-smarthome}"
TARGET_HOST="${TARGET_HOST:-127.0.0.1}"
TARGET_PORT="${TARGET_PORT:-19090}"

mkdir -p "$OUT_DIR"

if [[ ! -x ./build/otactl || ! -x ./build/fake_bootloader ]]; then
  echo "missing ./build/otactl or ./build/fake_bootloader, run cmake build first"
  exit 1
fi

run_case() {
  local name="$1"
  local bl_arg="$2"
  local expect="$3"
  local case_dir="$OUT_DIR/$name"
  local task_id=""
  local status_line=""
  local before_ids=""
  local after_ids=""
  local new_ids=""
  local wait_i=0

  mkdir -p "$case_dir"
  echo "[CASE] $name"

  ./build/fake_bootloader --port "$TARGET_PORT" $bl_arg >"$case_dir/fake_bootloader.log" 2>&1 &
  local bl_pid=$!

  sleep 0.2
  local start_out
  before_ids=$(./build/otactl ota list 2>/dev/null | awk '{print $1}' | sort -u || true)

  start_out=$(./build/otactl ota start \
    --device-id "$DEVICE_ID" \
    --device-type "$DEVICE_TYPE" \
    --firmware-id "$FIRMWARE_ID" \
    --target "$TARGET_HOST:$TARGET_PORT" 2>&1 || true)
  echo "$start_out" > "$case_dir/start.out"

  task_id=$(echo "$start_out" | sed -n 's/.*task=\([^ ]*\).*/\1/p' | head -n1)
  if [[ -z "$task_id" ]]; then
    after_ids=$(./build/otactl ota list 2>/dev/null | awk -v d="$DEVICE_ID" -v fw="$FIRMWARE_ID" '$2=="device="d && $3=="fw="fw {print $1}' | sort -u || true)
    new_ids=$(comm -13 <(echo "$before_ids") <(echo "$after_ids") || true)
    task_id=$(echo "$new_ids" | tail -n1)
    if [[ -z "$task_id" ]]; then
      echo "task id parse failed and no new task found" > "$case_dir/error.txt"
      kill "$bl_pid" 2>/dev/null || true
      wait "$bl_pid" 2>/dev/null || true
      echo "TASK_PARSE_FAILED" > "$case_dir/result.txt"
      return 0
    fi
  fi

  for wait_i in $(seq 1 80); do
    ./build/otactl ota status --task "$task_id" > "$case_dir/status.txt" 2>&1 || true
    status_line=$(cat "$case_dir/status.txt")
    if echo "$status_line" | grep -Eq "state=(SUCCESS|FAILED|CANCELED)"; then
      break
    fi
    sleep 0.1
  done

  status_line=$(cat "$case_dir/status.txt")
  if ! echo "$status_line" | grep -Eq "state=(SUCCESS|FAILED|CANCELED)"; then
    ./build/otactl ota cancel --task "$task_id" > "$case_dir/cancel.txt" 2>&1 || true
    sleep 0.2
    ./build/otactl ota status --task "$task_id" > "$case_dir/status.txt" 2>&1 || true
  fi

  ./build/otactl ota events --task "$task_id" > "$case_dir/events.txt" 2>&1 || true

  status_line=$(cat "$case_dir/status.txt")
  if [[ "$expect" == "FAILED_OR_CANCELED" ]]; then
    if echo "$status_line" | grep -Eq "state=(FAILED|CANCELED)"; then
      echo "PASS" > "$case_dir/result.txt"
    else
      echo "FAIL" > "$case_dir/result.txt"
    fi
  else
    if echo "$status_line" | grep -q "state=$expect"; then
      echo "PASS" > "$case_dir/result.txt"
    else
      echo "FAIL" > "$case_dir/result.txt"
    fi
  fi

  kill "$bl_pid" 2>/dev/null || true
  wait "$bl_pid" 2>/dev/null || true
}

run_case "success_baseline" "" "SUCCESS"
run_case "fault_verify_fail" "--verify-fail" "FAILED"
run_case "fault_disconnect_seq3" "--disconnect-at-seq 3" "FAILED_OR_CANCELED"
run_case "retry_nack_seq2" "--nack-seq 2" "SUCCESS"

python3 ./scripts/gen_fault_report.py --input "$OUT_DIR" --output "$OUT_DIR/report.md"
echo "done: $OUT_DIR/report.md"
