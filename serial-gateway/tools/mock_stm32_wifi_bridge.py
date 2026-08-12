#!/usr/bin/env python3
import argparse
import socket
import struct
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


def sensor_frame(device_id: int, temp_c10: int = 250, humi10: int = 500, mv: int = 3300, status: int = 1) -> bytes:
    payload = bytes([
        device_id,
        temp_c10 & 0xFF,
        (temp_c10 >> 8) & 0xFF,
        humi10 & 0xFF,
        (humi10 >> 8) & 0xFF,
        mv & 0xFF,
        (mv >> 8) & 0xFF,
        status & 0xFF,
    ])
    return frame(0x01, payload)


def command_ack(device_id: int, cmd_id: int, result: int = 0) -> bytes:
    payload = bytes([device_id, cmd_id & 0xFF, (cmd_id >> 8) & 0xFF, result & 0xFF])
    return frame(0x04, payload)


def send_with_split(sock: socket.socket, data: bytes, split_at: int) -> None:
    sock.sendall(data[:split_at])
    time.sleep(0.05)
    sock.sendall(data[split_at:])


def recv_and_ack(sock: socket.socket, device_id: int, timeout_sec: float, dump_path: str) -> bool:
    end = time.time() + timeout_sec
    buf = b""
    got = False
    with open(dump_path, "wb") as f:
        while time.time() < end:
            sock.settimeout(0.2)
            try:
                chunk = sock.recv(2048)
            except (socket.timeout, TimeoutError):
                continue
            if not chunk:
                break
            f.write(chunk)
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
                wire = pkt[-2] | (pkt[-1] << 8)
                calc = crc16(pkt[2:-2])
                if wire != calc:
                    continue
                if pkt[3] == 0x10 and len(pkt) >= 10:
                    cmd_id = pkt[5] | (pkt[6] << 8)
                    sock.sendall(command_ack(device_id, cmd_id, 0))
                    got = True
    return got


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9101)
    ap.add_argument("--device-id", type=int, default=1)
    ap.add_argument("--hb-count", type=int, default=3)
    ap.add_argument("--hb-interval", type=float, default=0.8)
    ap.add_argument("--listen-command-sec", type=float, default=0.0)
    ap.add_argument("--dump", default="/tmp/sg_tcp_binary_cmd.bin")
    args = ap.parse_args()

    sock = socket.create_connection((args.host, args.port), timeout=3)

    # 1) full frame
    sock.sendall(sensor_frame(args.device_id, temp_c10=251, humi10=503))
    # 2) half frame
    sf = sensor_frame(args.device_id, temp_c10=252, humi10=504)
    send_with_split(sock, sf, 5)
    # 3) sticky frames
    sock.sendall(sensor_frame(args.device_id, temp_c10=253) + sensor_frame(args.device_id, temp_c10=254))
    # 4) bad crc then good frame
    bad = bytearray(sensor_frame(args.device_id, temp_c10=255))
    bad[-1] ^= 0xFF
    sock.sendall(bytes(bad) + sensor_frame(args.device_id, temp_c10=256))

    # adapter heartbeat 0x7E
    for _ in range(args.hb_count):
        sock.sendall(frame(0x7E, b""))
        time.sleep(args.hb_interval)

    if args.listen_command_sec > 0:
        ok = recv_and_ack(sock, args.device_id, args.listen_command_sec, args.dump)
        print("CMD_RX=1" if ok else "CMD_RX=0")

    sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
