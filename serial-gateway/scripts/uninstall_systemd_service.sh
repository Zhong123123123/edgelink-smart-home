#!/usr/bin/env bash
set -euo pipefail

SERVICE_DST="/etc/systemd/system/serial-gateway.service"
DROPIN_DIR="/etc/systemd/system/serial-gateway.service.d"

if [[ $EUID -ne 0 ]]; then
  echo "please run as root"
  exit 1
fi

systemctl disable --now serial-gateway.service || true
rm -f "$SERVICE_DST"
rm -rf "$DROPIN_DIR"
systemctl daemon-reload

echo "systemd service removed. config files under /etc/serial-gateway are kept."
