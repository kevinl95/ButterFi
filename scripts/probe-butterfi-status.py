#!/usr/bin/env python3

import fcntl
import os
import select
import struct
import termios
import time


TTY = "/dev/ttyACM0"
STATUS_FRAME = bytes([0x42, 0x46, 0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0x04])

FRAME_TYPE_LABELS = {
    0x81: "device_status",
    0x82: "uplink_accepted",
    0x83: "response_chunk",
    0x84: "transfer_complete",
    0x85: "transfer_error",
    0x86: "pong",
    0x87: "debug_text",
}

DEVICE_STATE_LABELS = {
    0: "Idle",
    1: "Serial ready",
    2: "Sidewalk starting",
    3: "Sidewalk ready",
    4: "Busy",
    5: "Error",
    6: "Sidewalk not registered",
}

LINK_STATE_LABELS = {
    0: "Unknown",
    1: "BLE",
    2: "FSK",
    3: "LoRa",
}


def checksum(frame: bytes) -> int:
    value = 0

    for byte in frame[2:-1]:
        value ^= byte

    return value


def iter_frames(data: bytes):
    offset = 0

    while offset + 9 <= len(data):
        if data[offset] != 0x42 or data[offset + 1] != 0x46:
            break

        payload_len = data[offset + 6] | (data[offset + 7] << 8)
        frame_len = 8 + payload_len + 1
        if offset + frame_len > len(data):
            break

        frame = data[offset : offset + frame_len]
        yield offset, frame
        offset += frame_len

    return offset


def describe_frame(index: int, offset: int, frame: bytes) -> str:
    frame_type = frame[3]
    request_id = frame[4]
    flags = frame[5]
    payload = frame[8:-1]
    expected_checksum = checksum(frame)
    actual_checksum = frame[-1]
    frame_label = FRAME_TYPE_LABELS.get(frame_type, f"0x{frame_type:02x}")
    summary = (
        f"frame {index}: offset={offset} type={frame_label} req={request_id} "
        f"flags=0x{flags:02x} payload={payload.hex()} checksum=0x{actual_checksum:02x}"
    )

    if expected_checksum != actual_checksum:
        return summary + f" INVALID(expected=0x{expected_checksum:02x})"

    if frame_type == 0x81 and len(payload) == 3:
        state, link, active_request = payload
        return (
            summary
            + f" state={state}({DEVICE_STATE_LABELS.get(state, 'unknown')})"
            + f" link={link}({LINK_STATE_LABELS.get(link, 'unknown')})"
            + f" active_request={active_request}"
        )

    if frame_type == 0x85 and payload:
        error_code = payload[0]
        try:
            error_text = payload[1:].decode("utf-8", errors="replace")
        except Exception:
            error_text = payload[1:].hex()
        return summary + f" error_code={error_code} error_text={error_text}"

    if frame_type == 0x87:
        try:
            text = payload.decode("utf-8", errors="replace")
        except Exception:
            text = payload.hex()
        return summary + f" text={text}"

    return summary


def main() -> int:
    fd = os.open(TTY, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)

    try:
        attrs = termios.tcgetattr(fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0
        attrs[4] = termios.B115200
        attrs[5] = termios.B115200
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)

        TIOCM_DTR = 0x002
        TIOCM_RTS = 0x004
        TIOCMBIS = 0x5416
        fcntl.ioctl(fd, TIOCMBIS, struct.pack("I", TIOCM_DTR | TIOCM_RTS))

        time.sleep(0.3)
        os.write(fd, STATUS_FRAME)

        deadline = time.time() + 2.0
        data = b""

        while time.time() < deadline:
            readable, _, _ = select.select([fd], [], [], 0.2)
            if not readable:
                continue

            try:
                chunk = os.read(fd, 256)
            except BlockingIOError:
                continue

            if chunk:
                data += chunk

        print(data.hex())

        consumed = 0
        frame_count = 0
        for frame_count, (offset, frame) in enumerate(iter_frames(data), start=1):
            consumed = offset + len(frame)
            print(describe_frame(frame_count, offset, frame))

        if consumed < len(data):
            print(f"trailing_bytes: {data[consumed:].hex()}")

        return 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    raise SystemExit(main())