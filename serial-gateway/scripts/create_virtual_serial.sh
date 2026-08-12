#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
LINK_A="${1:-/tmp/ttyV0}"
LINK_B="${2:-/tmp/ttyV1}"

if [[ ! -x "$BUILD_DIR/pty_bridge" ]]; then
  echo "pty_bridge not found, build project first: cmake -S . -B build && cmake --build build"
  exit 1
fi

exec "$BUILD_DIR/pty_bridge" "$LINK_A" "$LINK_B"
