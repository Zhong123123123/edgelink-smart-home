#!/usr/bin/env bash
set -euo pipefail

HOST="127.0.0.1"
BASE_PORT=9010
INSTANCES=2
OUT_DIR="/tmp/sg_multi_status"
WATCH=false
INTERVAL_SEC=2
COUNT=0
CSV_FILE=""

usage() {
  cat <<USAGE
Usage:
  collect_multi_instance_status.sh [host] [base_port] [instances] [out_dir]
  collect_multi_instance_status.sh --watch [--interval sec] [--count n] [--csv-file path] [host] [base_port] [instances] [out_dir]

Options:
  --watch             Continuously sample status.
  --interval <sec>    Interval seconds in watch mode (default: 2).
  --count <n>         Stop after n rounds in watch mode (0 means infinite).
  --csv-file <path>   Append summary rows into CSV file.
  -h, --help          Show this help.
USAGE
}

is_number() {
  [[ "$1" =~ ^[0-9]+$ ]]
}

POSITIONAL=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --watch)
      WATCH=true
      shift
      ;;
    --interval)
      [[ $# -ge 2 ]] || { echo "--interval requires value"; exit 1; }
      INTERVAL_SEC="$2"
      shift 2
      ;;
    --count)
      [[ $# -ge 2 ]] || { echo "--count requires value"; exit 1; }
      COUNT="$2"
      shift 2
      ;;
    --csv-file)
      [[ $# -ge 2 ]] || { echo "--csv-file requires value"; exit 1; }
      CSV_FILE="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      POSITIONAL+=("$1")
      shift
      ;;
  esac
done

if [[ ${#POSITIONAL[@]} -ge 1 ]]; then HOST="${POSITIONAL[0]}"; fi
if [[ ${#POSITIONAL[@]} -ge 2 ]]; then BASE_PORT="${POSITIONAL[1]}"; fi
if [[ ${#POSITIONAL[@]} -ge 3 ]]; then INSTANCES="${POSITIONAL[2]}"; fi
if [[ ${#POSITIONAL[@]} -ge 4 ]]; then OUT_DIR="${POSITIONAL[3]}"; fi

if ! command -v curl >/dev/null 2>&1; then
  echo "collect script requires curl"
  exit 99
fi

if ! is_number "$BASE_PORT" || ! is_number "$INSTANCES" || ! is_number "$INTERVAL_SEC" || ! is_number "$COUNT"; then
  echo "base_port/instances/interval/count must be integers"
  exit 1
fi
if [[ "$BASE_PORT" -le 0 || "$BASE_PORT" -gt 65535 ]]; then
  echo "invalid base_port"
  exit 1
fi
if [[ "$INSTANCES" -le 0 ]]; then
  echo "instances must be > 0"
  exit 1
fi
if [[ "$INTERVAL_SEC" -le 0 ]]; then
  echo "interval must be > 0"
  exit 1
fi

mkdir -p "$OUT_DIR"
if [[ -z "$CSV_FILE" ]]; then
  CSV_FILE="$OUT_DIR/summary.csv"
fi

if [[ ! -f "$CSV_FILE" ]]; then
  echo "timestamp,instance_index,port,serial_instance,devices_online,received_frames,upload_success,upload_failed,cache_backlog,status_file,devices_file,metrics_file" >"$CSV_FILE"
fi

collect_once() {
  local ts
  ts="$(date +%Y%m%d_%H%M%S)"
  echo "[$(date +%H:%M:%S)] collecting host=$HOST base_port=$BASE_PORT instances=$INSTANCES"

  for i in $(seq 0 $((INSTANCES - 1))); do
    local port=$((BASE_PORT + i))
    local status_file="$OUT_DIR/${ts}_instance_${i}_status.json"
    local devices_file="$OUT_DIR/${ts}_instance_${i}_devices.json"
    local metrics_file="$OUT_DIR/${ts}_instance_${i}_metrics.txt"

    if ! curl -fsS "http://$HOST:$port/api/status" >"$status_file"; then
      echo "instance[$i] port=$port status fetch failed"
      continue
    fi
    curl -fsS "http://$HOST:$port/api/devices" >"$devices_file" || true
    curl -fsS "http://$HOST:$port/metrics" >"$metrics_file" || true

    local serial_instance
    local devices_online
    local frames_rx
    local upload_success
    local upload_failed
    local cache_backlog
    serial_instance="$(sed -n 's/.*"serial_instance":"\([^"]*\)".*/\1/p' "$status_file" | head -n1)"
    devices_online="$(sed -n 's/.*"devices_online":\([0-9][0-9]*\).*/\1/p' "$status_file" | head -n1)"
    frames_rx="$(sed -n 's/.*"received_frames":\([0-9][0-9]*\).*/\1/p' "$status_file" | head -n1)"
    upload_success="$(sed -n 's/.*"upload_success":\([0-9][0-9]*\).*/\1/p' "$status_file" | head -n1)"
    upload_failed="$(sed -n 's/.*"upload_failed":\([0-9][0-9]*\).*/\1/p' "$status_file" | head -n1)"
    cache_backlog="$(sed -n 's/.*"cache_backlog":\([0-9][0-9]*\).*/\1/p' "$status_file" | head -n1)"

    serial_instance="${serial_instance:-unknown}"
    devices_online="${devices_online:-0}"
    frames_rx="${frames_rx:-0}"
    upload_success="${upload_success:-0}"
    upload_failed="${upload_failed:-0}"
    cache_backlog="${cache_backlog:-0}"

    echo "instance[$i] port=$port serial_instance=$serial_instance devices_online=$devices_online frames_rx=$frames_rx upload_success=$upload_success upload_failed=$upload_failed cache_backlog=$cache_backlog"
    echo "  status:  $status_file"
    echo "  devices: $devices_file"
    echo "  metrics: $metrics_file"

    echo "${ts},${i},${port},${serial_instance},${devices_online},${frames_rx},${upload_success},${upload_failed},${cache_backlog},${status_file},${devices_file},${metrics_file}" >>"$CSV_FILE"
  done

  echo "csv: $CSV_FILE"
}

if [[ "$WATCH" == true ]]; then
  round=0
  while true; do
    collect_once
    round=$((round + 1))
    if [[ "$COUNT" -gt 0 && "$round" -ge "$COUNT" ]]; then
      break
    fi
    sleep "$INTERVAL_SEC"
  done
else
  collect_once
fi
