#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SERVICE_SRC="$ROOT_DIR/systemd/serial-gateway.service"
SERVICE_DST="/etc/systemd/system/serial-gateway.service"
CFG_DIR="/etc/serial-gateway"
CFG_FILE="$CFG_DIR/gateway.yaml"
EXAMPLE_CFG="$ROOT_DIR/config/gateway.yaml"
ENV_FILE="$CFG_DIR/serial-gateway.env"
EXAMPLE_ENV="$ROOT_DIR/systemd/serial-gateway.env.example"
STATE_DIR="/var/lib/serial-gateway"

if [[ $EUID -ne 0 ]]; then
  echo "please run as root"
  exit 1
fi

mkdir -p "$CFG_DIR" "$STATE_DIR"
if [[ ! -f "$CFG_FILE" ]]; then
  cp "$EXAMPLE_CFG" "$CFG_FILE"
  echo "installed config template to $CFG_FILE"
else
  echo "config exists, keep current: $CFG_FILE"
fi

if [[ ! -f "$ENV_FILE" ]]; then
  cp "$EXAMPLE_ENV" "$ENV_FILE"
  echo "installed env template to $ENV_FILE"
else
  echo "env exists, keep current: $ENV_FILE"
fi

cp "$SERVICE_SRC" "$SERVICE_DST"
systemctl daemon-reload
systemctl enable serial-gateway.service

echo "systemd service installed."
echo "start service: systemctl start serial-gateway.service"
echo "view logs: journalctl -u serial-gateway.service -f"
echo "set resource limits: ./scripts/set_systemd_limits.sh --memory-max 300M --cpu-quota 50%"
