#!/usr/bin/env bash
set -euo pipefail

SERVICE_NAME="serial-gateway.service"
DROPIN_DIR="/etc/systemd/system/${SERVICE_NAME}.d"
DROPIN_FILE="${DROPIN_DIR}/limits.conf"

if [[ $EUID -ne 0 ]]; then
  echo "please run as root"
  exit 1
fi

MEMORY_MAX=""
CPU_QUOTA=""
TASKS_MAX=""
RESET=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --memory-max)
      MEMORY_MAX="${2:-}"
      shift 2
      ;;
    --cpu-quota)
      CPU_QUOTA="${2:-}"
      shift 2
      ;;
    --tasks-max)
      TASKS_MAX="${2:-}"
      shift 2
      ;;
    --reset)
      RESET=true
      shift
      ;;
    *)
      echo "unknown argument: $1"
      echo "usage: $0 [--memory-max 300M] [--cpu-quota 50%] [--tasks-max 256] [--reset]"
      exit 2
      ;;
  esac
done

if [[ "$RESET" == true ]]; then
  rm -f "$DROPIN_FILE"
  systemctl daemon-reload
  systemctl restart "$SERVICE_NAME" || true
  echo "resource limits reset for ${SERVICE_NAME}"
  exit 0
fi

if [[ -z "$MEMORY_MAX" && -z "$CPU_QUOTA" && -z "$TASKS_MAX" ]]; then
  echo "no limits provided"
  echo "usage: $0 [--memory-max 300M] [--cpu-quota 50%] [--tasks-max 256]"
  exit 3
fi

mkdir -p "$DROPIN_DIR"
{
  echo "[Service]"
  if [[ -n "$MEMORY_MAX" ]]; then
    echo "MemoryMax=$MEMORY_MAX"
  fi
  if [[ -n "$CPU_QUOTA" ]]; then
    echo "CPUQuota=$CPU_QUOTA"
  fi
  if [[ -n "$TASKS_MAX" ]]; then
    echo "TasksMax=$TASKS_MAX"
  fi
} > "$DROPIN_FILE"

systemctl daemon-reload
systemctl restart "$SERVICE_NAME" || true

echo "resource limits updated for ${SERVICE_NAME}:"
[[ -n "$MEMORY_MAX" ]] && echo "  MemoryMax=$MEMORY_MAX"
[[ -n "$CPU_QUOTA" ]] && echo "  CPUQuota=$CPU_QUOTA"
[[ -n "$TASKS_MAX" ]] && echo "  TasksMax=$TASKS_MAX"
