#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

mkdir -p data tmp_demo

cmake -S . -B build >/dev/null
cmake --build build -j >/dev/null

printf '\x01\x02\x03\x04\x05\x06\x07\x08' > tmp_demo/app.bin
python3 - <<'PY'
import zlib, json
b=open('tmp_demo/app.bin','rb').read()
m={"firmware_id":"stm32f407-smarthome-1.0.1","device_type":"stm32f407-smarthome","version":"1.0.1","image":"app.bin","image_size":len(b),"crc32":f"0x{zlib.crc32(b)&0xffffffff:08X}"}
open('tmp_demo/manifest.json','w').write(json.dumps(m))
PY

./build/otactl firmware add --manifest tmp_demo/manifest.json --image tmp_demo/app.bin
./build/fake_bootloader --port 19090 > tmp_demo/fake_bl.log 2>&1 &
BL_PID=$!
trap 'kill $BL_PID >/dev/null 2>&1 || true' EXIT
sleep 0.2

./build/otactl ota start --device-id 1 --firmware-id stm32f407-smarthome-1.0.1 --target 127.0.0.1:19090
./build/otactl ota list
