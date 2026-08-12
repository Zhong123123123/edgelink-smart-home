#!/usr/bin/env python3
import argparse
import socket
import time


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc >> 1) ^ 0xA001) if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF


def frame(ft: int, payload: bytes) -> bytes:
    body = bytes([1 + len(payload), ft]) + payload
    c = crc16(body)
    return b"\xAA\x55" + body + bytes([c & 0xFF, (c >> 8) & 0xFF])


def sensor_frame(device_id: int, seq: int) -> bytes:
    temp = 240 + (seq % 20)
    hum = 500 + (seq % 50)
    mv = 3300
    st = seq & 0x01
    payload = bytes([
        device_id,
        temp & 0xFF, (temp >> 8) & 0xFF,
        hum & 0xFF, (hum >> 8) & 0xFF,
        mv & 0xFF, (mv >> 8) & 0xFF,
        st,
    ])
    return frame(0x01, payload)


def command_ack(device_id: int, cmd_id: int, result: int = 0) -> bytes:
    return frame(0x04, bytes([device_id, cmd_id & 0xFF, (cmd_id >> 8) & 0xFF, result & 0xFF]))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9101)
    ap.add_argument("--device-id", type=int, default=1)
    ap.add_argument("--duration-sec", type=int, default=1800)
    ap.add_argument("--send-period-ms", type=int, default=500)
    ap.add_argument("--hb-ms", type=int, default=1000)
    ap.add_argument("--out", default="/tmp/sg_tcp_binary_daemon.log")
    args = ap.parse_args()

    s = socket.create_connection((args.host, args.port), timeout=3)
    s.settimeout(0.05)
    start = time.time()
    seq = 0
    last_send = 0.0
    last_hb = 0.0
    buf = b""

    with open(args.out, "w", encoding="utf-8") as f:
        while time.time() - start < args.duration_sec:
            now = time.time()
            if now - last_send >= args.send_period_ms / 1000.0:
                s.sendall(sensor_frame(args.device_id, seq))
                seq += 1
                last_send = now
            if now - last_hb >= args.hb_ms / 1000.0:
                s.sendall(frame(0x7E, b""))
                last_hb = now

            try:
                chunk = s.recv(2048)
            except (socket.timeout, TimeoutError):
                chunk = b""
            if chunk:
                buf += chunk
                while len(buf) >= 6:
                    if not (buf[0] == 0xAA and buf[1] == 0x55):
                        buf = buf[1:]
                        continue
                    ln = buf[2]
                    total = 2 + 1 + ln + 2
                    if len(buf) < total:
                        break
                    pkt = buf[:total]
                    buf = buf[total:]
                    if (pkt[-2] | (pkt[-1] << 8)) != crc16(pkt[2:-2]):
                        continue
                    if pkt[3] == 0x10 and len(pkt) >= 10:
                        cmd_id = pkt[5] | (pkt[6] << 8)
                        s.sendall(command_ack(args.device_id, cmd_id, 0))
                        f.write(f"ACK cmd_id={cmd_id}\n")
                        f.flush()

            time.sleep(0.01)

    s.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
