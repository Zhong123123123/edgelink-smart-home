#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import re
import shutil
import zlib

def parse_version_code(version: str) -> int:
    m = re.match(r"^(\\d+)\\.(\\d+)\\.(\\d+)", version)
    if not m:
        return 0
    major = int(m.group(1))
    minor = int(m.group(2))
    patch = int(m.group(3))
    if major > 255 or minor > 255 or patch > 255:
        return 0
    return (major << 16) | (minor << 8) | patch


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--device-type", required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--bin", required=True)
    p.add_argument("--hex")
    p.add_argument("--map")
    p.add_argument("--entry", required=True)
    p.add_argument("--slot-size", type=int, required=True)
    p.add_argument("--out", required=True)
    args = p.parse_args()

    os.makedirs(args.out, exist_ok=True)
    app_bin_out = os.path.join(args.out, "app.bin")
    shutil.copyfile(args.bin, app_bin_out)

    with open(args.bin, "rb") as f:
        data = f.read()
    crc32 = f"0x{zlib.crc32(data) & 0xFFFFFFFF:08X}"
    sha256 = hashlib.sha256(data).hexdigest()

    firmware_id = f"{args.device_type}-{args.version}"
    image_version = parse_version_code(args.version)
    manifest = {
        "firmware_id": firmware_id,
        "device_type": args.device_type,
        "version": args.version,
        "image": "app.bin",
        "image_size": len(data),
        "crc32": crc32,
        "sha256": sha256,
        "entry_addr": args.entry,
        "image_version": image_version,
        "slot_size": args.slot_size,
        "min_bootloader_version": "1.0.0",
        "features": ["ota_confirm"],
    }

    with open(os.path.join(args.out, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)

    if args.hex:
        shutil.copyfile(args.hex, os.path.join(args.out, "app.hex"))
    if args.map:
        shutil.copyfile(args.map, os.path.join(args.out, "app.map"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
