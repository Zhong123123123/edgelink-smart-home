#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -eq 0 ]]; then
  SUDO=""
else
  SUDO="sudo"
fi

echo "[INFO] Updating apt index..."
${SUDO} apt-get update

echo "[INFO] Installing ARM toolchain + flashing tools..."
${SUDO} apt-get install -y \
  build-essential \
  cmake \
  gdb-multiarch \
  gcc-arm-none-eabi \
  binutils-arm-none-eabi \
  libnewlib-arm-none-eabi \
  openocd \
  stlink-tools \
  minicom \
  python3-serial

echo "[INFO] Installation done."
echo "[INFO] If your user cannot access USB debugger, run:"
echo "       sudo usermod -aG dialout,plugdev \$USER"
echo "       then re-login."
