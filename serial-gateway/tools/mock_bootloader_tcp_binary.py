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


def read_u16_le(data: bytes, off: int = 0) -> int:
    return data[off] | (data[off + 1] << 8)


def sensor_frame(device_id: int) -> bytes:
    temp_c10 = 250
    humi10 = 500
    mv = 3300
    status = 1
    payload = bytes(
        [
            device_id & 0xFF,
            temp_c10 & 0xFF,
            (temp_c10 >> 8) & 0xFF,
            humi10 & 0xFF,
            (humi10 >> 8) & 0xFF,
            mv & 0xFF,
            (mv >> 8) & 0xFF,
            status & 0xFF,
        ]
    )
    return frame(0x01, payload)


def boot_hello_frame() -> bytes:
    # schema=2 active=0 pending=0xFF confirmed=0 rollback_count=0
    payload = bytes([2, 0, 0xFF, 0]) + struct.pack("<I", 0)
    return frame(0x20, payload)


def heartbeat_frame() -> bytes:
    return frame(0x7E, b"")


def send_ack(sock: socket.socket, seq: int, ok: bool, nack_code: int = 1) -> None:
    if ok:
        payload = struct.pack("<H", seq)
        sock.sendall(frame(0x22, payload))
    else:
        payload = struct.pack("<HH", seq, nack_code & 0xFFFF)
        sock.sendall(frame(0x23, payload))


def recv_frame(sock: socket.socket, timeout_sec: float):
    sock.settimeout(timeout_sec)
    head = sock.recv(4)
    if len(head) != 4:
        return None
    if head[0] != 0xAA or head[1] != 0x55:
        return None
    ln = head[2]
    ft = head[3]
    payload_len = ln - 1
    payload = b""
    while len(payload) < payload_len + 2:
        chunk = sock.recv(payload_len + 2 - len(payload))
        if not chunk:
            return None
        payload += chunk
    body = bytes([ln, ft]) + payload[:-2]
    wire_crc = payload[-2] | (payload[-1] << 8)
    calc_crc = crc16(body)
    if wire_crc != calc_crc:
        return None
    return ft, payload[:-2]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9101)
    ap.add_argument("--device-id", type=int, default=1)
    ap.add_argument("--nack-seq", type=int, default=-1)
    ap.add_argument("--verify-nack", action="store_true")
    ap.add_argument("--disconnect-at-seq", type=int, default=-1)
    ap.add_argument("--heartbeat-interval-ms", type=int, default=1000)
    args = ap.parse_args()

    sock = socket.create_connection((args.host, args.port), timeout=5)
    sock.settimeout(1.0)

    # Let gateway bind device->session first.
    sock.sendall(sensor_frame(args.device_id))
    time.sleep(0.1)
    sock.sendall(boot_hello_frame())

    last_hb = time.time()
    while True:
        now = time.time()
        if (now - last_hb) * 1000.0 >= args.heartbeat_interval_ms:
            try:
                sock.sendall(heartbeat_frame())
            except OSError:
                break
            last_hb = now

        try:
            got = recv_frame(sock, 0.2)
        except (socket.timeout, TimeoutError):
            continue
        except OSError:
            break
        if got is None:
            continue
        ft, payload = got

        # Adapter control is consumed by ESP32 in real path; ignore in mock if present.
        if ft == 0x7D:
            continue

        if ft not in (0x31, 0x32, 0x33, 0x34, 0x35, 0x37):
            continue

        seq = read_u16_le(payload, 0) if len(payload) >= 2 else 0
        if args.disconnect_at_seq >= 0 and seq == args.disconnect_at_seq:
            sock.close()
            return 0
        if args.nack_seq >= 0 and seq == args.nack_seq:
            send_ack(sock, seq, False, 5)
            args.nack_seq = -1
            continue
        if args.verify_nack and ft == 0x33:
            send_ack(sock, seq, False, 7)
            continue

        if ft == 0x37:
            # GET_VERSION -> VERSION_REPORT
            sock.sendall(frame(0x21, struct.pack("<II", 0x00010000, 0x00010001)))
            continue

        send_ack(sock, seq, True)
        if ft == 0x34:
            # COMMIT ack done; bootloader would reset.
            return 0


if __name__ == "__main__":
    raise SystemExit(main())

