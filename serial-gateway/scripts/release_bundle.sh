#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${1:-$ROOT_DIR/dist}"
VERSION="${2:-v1.0.0}"
X86_BUILD_DIR="${X86_BUILD_DIR:-$ROOT_DIR/build}"
ARM_BUILD_DIR="${ARM_BUILD_DIR:-$ROOT_DIR/build-arm}"

mkdir -p "$OUT_DIR"
TMP_DIR="$(mktemp -d /tmp/sg_release.XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT

PROJECT_NAME="serial-gateway-${VERSION}"
SRC_STAGING="$TMP_DIR/${PROJECT_NAME}-src"
cp -a "$ROOT_DIR" "$SRC_STAGING"
rm -rf "$SRC_STAGING/build" "$SRC_STAGING/build-arm" "$SRC_STAGING/build-arm-qemu" "$SRC_STAGING/dist" "$SRC_STAGING/.git"

tar -C "$TMP_DIR" -czf "$OUT_DIR/${PROJECT_NAME}-src.tar.gz" "${PROJECT_NAME}-src"

bundle_binary() {
  local arch="$1"
  local build_dir="$2"
  local staging="$TMP_DIR/${PROJECT_NAME}-linux-${arch}"

  if [[ ! -x "$build_dir/serial_gateway" ]]; then
    echo "skip ${arch}: serial_gateway not found in $build_dir"
    return 0
  fi

  mkdir -p "$staging/bin" "$staging/config" "$staging/systemd" "$staging/scripts" "$staging/docs"

  cp "$build_dir/serial_gateway" "$staging/bin/"
  cp "$ROOT_DIR/config/gateway.yaml" "$staging/config/gateway.yaml.example"
  cp "$ROOT_DIR/systemd/serial-gateway.service" "$staging/systemd/"
  cp "$ROOT_DIR/systemd/serial-gateway.env.example" "$staging/systemd/"
  cp "$ROOT_DIR/scripts/install_systemd_service.sh" "$staging/scripts/"
  cp "$ROOT_DIR/scripts/uninstall_systemd_service.sh" "$staging/scripts/"
  cp "$ROOT_DIR/scripts/set_systemd_limits.sh" "$staging/scripts/"
  cp "$ROOT_DIR/docs/release_notes.md" "$staging/docs/"
  cp "$ROOT_DIR/docs/demo_commands.md" "$staging/docs/"

  tar -C "$TMP_DIR" -czf "$OUT_DIR/${PROJECT_NAME}-linux-${arch}.tar.gz" "${PROJECT_NAME}-linux-${arch}"
}

bundle_binary "x86_64" "$X86_BUILD_DIR"
bundle_binary "armhf" "$ARM_BUILD_DIR"

(
  cd "$OUT_DIR"
  sha256sum ./*.tar.gz > SHA256SUMS
)

echo "release bundles created in $OUT_DIR"
ls -lh "$OUT_DIR"
