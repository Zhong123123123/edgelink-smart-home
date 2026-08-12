#!/usr/bin/env python3
import argparse
import struct
import time

import serial


HEAD = b"\xAA\x55"

TYPE_SENSOR_DATA = 0x01
TYPE_HEARTBEAT = 0x03
TYPE_COMMAND_ACK = 0x04
TYPE_DEVICE_STATUS = 0x05
TYPE_COMMAND_REQ = 0x10

CMD_SET_LED = 0x01
CMD_SET_BUZZER = 0x02
CMD_SET_MODE = 0x03
CMD_GET_STATUS = 0x04
CMD_SET_THRESHOLD = 0x05
CMD_SET_LOG_LEVEL = 0x06


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


def make_frame(frame_type: int, payload: bytes) -> bytes:
    wire_len = len(payload) + 1
    body = bytes([wire_len, frame_type]) + payload
    crc = crc16_modbus(body)
    return HEAD + body + struct.pack("<H", crc)


def command_payload(device_id: int, command_id: int, command_type: int, args: bytes = b"") -> bytes:
    return struct.pack("<BHB", device_id, command_id, command_type) + args


def describe_frame(frame_type: int, payload: bytes) -> str:
    if frame_type == TYPE_SENSOR_DATA and len(payload) >= 8:
        device_id = payload[0]
        temp = struct.unpack_from("<h", payload, 1)[0] / 10.0
        humi = struct.unpack_from("<H", payload, 3)[0] / 10.0
        voltage = struct.unpack_from("<H", payload, 5)[0]
        status = payload[7]
        flags = []
        flags.append("led=on" if status & 0x01 else "led=off")
        flags.append("alarm=on" if status & 0x02 else "alarm=off")
        flags.append("sensor=ok" if status & 0x04 else "sensor=bad")
        flags.append("mode=auto" if status & 0x08 else "mode=manual")
        return f"sensor dev={device_id} temp={temp:.1f}C humi={humi:.1f}% voltage={voltage}mV {' '.join(flags)}"
    if frame_type == TYPE_HEARTBEAT and len(payload) >= 1:
        return f"heartbeat dev={payload[0]}"
    if frame_type == TYPE_COMMAND_ACK and len(payload) >= 4:
        command_id = struct.unpack_from("<H", payload, 1)[0]
        return f"ack dev={payload[0]} cmd_id={command_id} result={payload[3]}"
    if frame_type == TYPE_DEVICE_STATUS and len(payload) >= 4:
        return f"status dev={payload[0]} wifi={payload[1]} mqtt={payload[2]} reconnect_fail={payload[3]}"
    return f"type=0x{frame_type:02X} payload={payload.hex(' ')}"


def read_frames(port: serial.Serial) -> None:
    buf = bytearray()
    while True:
        data = port.read(64)
        if not data:
            continue
        buf.extend(data)
        while len(buf) >= 5:
            if buf[0:2] != HEAD:
                del buf[0]
                continue
            wire_len = buf[2]
            total_len = 2 + 1 + wire_len + 2
            if len(buf) < total_len:
                break
            raw = bytes(buf[:total_len])
            del buf[:total_len]
            body = raw[2:-2]
            got_crc = struct.unpack_from("<H", raw, total_len - 2)[0]
            calc_crc = crc16_modbus(body)
            if got_crc != calc_crc:
                print(f"bad crc raw={raw.hex(' ')}")
                continue
            print(describe_frame(body[1], body[2:]))


def main() -> None:
    parser = argparse.ArgumentParser(description="SmartHome USART3 gateway protocol tester")
    parser.add_argument("port", help="Serial port, for example COM8")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--device", type=int, default=1)
    parser.add_argument("--cmd-id", type=int, default=1)
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("listen")
    sub.add_parser("status")
    led = sub.add_parser("led")
    led.add_argument("value", choices=["on", "off"])
    mode = sub.add_parser("mode")
    mode.add_argument("value", choices=["auto", "manual"])
    threshold = sub.add_parser("threshold")
    threshold.add_argument("temp", type=float)
    threshold.add_argument("humi", type=float)
    args = parser.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.2) as port:
        if args.cmd == "listen":
            read_frames(port)
            return

        if args.cmd == "status":
            payload = command_payload(args.device, args.cmd_id, CMD_GET_STATUS)
        elif args.cmd == "led":
            payload = command_payload(args.device, args.cmd_id, CMD_SET_LED, bytes([1 if args.value == "on" else 0]))
        elif args.cmd == "mode":
            payload = command_payload(args.device, args.cmd_id, CMD_SET_MODE, bytes([1 if args.value == "auto" else 0]))
        elif args.cmd == "threshold":
            temp = max(0, min(65535, int(args.temp * 10 + 0.5)))
            humi = max(0, min(65535, int(args.humi * 10 + 0.5)))
            payload = command_payload(args.device, args.cmd_id, CMD_SET_THRESHOLD, struct.pack("<HH", temp, humi))
        else:
            raise RuntimeError(args.cmd)

        frame = make_frame(TYPE_COMMAND_REQ, payload)
        port.write(frame)
        port.flush()
        print(f"tx {frame.hex(' ')}")
        time.sleep(0.1)
        end_time = time.time() + 2.0
        while time.time() < end_time:
            data = port.read(256)
            if data:
                print(f"rx raw {data.hex(' ')}")


if __name__ == "__main__":
    main()
