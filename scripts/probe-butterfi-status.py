#!/usr/bin/env python3

import argparse
import fcntl
import os
import select
import struct
import sys
import termios
import time


DEFAULT_TTY = "/dev/ttyACM0"
DEFAULT_BAUD = 115200
DEFAULT_TIMEOUT = 2.0
DEFAULT_WAKE_DELAY = 0.3
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

BAUD_RATES = {
    9600: termios.B9600,
    19200: termios.B19200,
    38400: termios.B38400,
    57600: termios.B57600,
    115200: termios.B115200,
}

if hasattr(termios, "B230400"):
    BAUD_RATES[230400] = termios.B230400


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Send a ButterFi status request over USB CDC and decode the response."
    )
    parser.add_argument(
        "--tty",
        default=os.environ.get("BUTTERFI_TTY", DEFAULT_TTY),
        help="Serial device path. Defaults to BUTTERFI_TTY or /dev/ttyACM0.",
    )
    parser.add_argument(
        "--baud",
        type=int,
        choices=sorted(BAUD_RATES),
        default=DEFAULT_BAUD,
        help="Serial baud rate.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT,
        help="Seconds to wait for a response.",
    )
    parser.add_argument(
        "--wake-delay",
        type=float,
        default=DEFAULT_WAKE_DELAY,
        help="Seconds to wait after asserting DTR/RTS before writing.",
    )
    args = parser.parse_args()

    if args.timeout <= 0:
        parser.error("--timeout must be greater than 0")

    if args.wake_delay < 0:
        parser.error("--wake-delay must be 0 or greater")

    return args


def configure_serial(fd: int, baud: int) -> None:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = BAUD_RATES[baud]
    attrs[5] = BAUD_RATES[baud]
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def set_ready_lines(fd: int) -> None:
    tiocmbis = 0x5416
    tiocm_dtr = 0x002
    tiocm_rts = 0x004
    fcntl.ioctl(fd, tiocmbis, struct.pack("I", tiocm_dtr | tiocm_rts))


def open_serial_device(tty: str, baud: int) -> int:
    try:
        fd = os.open(tty, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError as exc:
        raise RuntimeError(f"Unable to open {tty}: {exc.strerror or exc}") from exc

    try:
        configure_serial(fd, baud)
        set_ready_lines(fd)
        return fd
    except Exception:
        os.close(fd)
        raise


def read_response(fd: int, timeout: float) -> bytes:
    deadline = time.time() + timeout
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

    return data


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
    args = parse_args()

    try:
        fd = open_serial_device(args.tty, args.baud)
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return 1

    try:
        time.sleep(args.wake_delay)
        os.write(fd, STATUS_FRAME)
        data = read_response(fd, args.timeout)
    except OSError as exc:
        print(f"I/O failure on {args.tty}: {exc.strerror or exc}", file=sys.stderr)
        return 1
    finally:
        os.close(fd)

    if not data:
        print(
            f"No response received from {args.tty} within {args.timeout:.1f}s.",
            file=sys.stderr,
        )
        return 1

    print(f"Raw bytes: {data.hex()}")

    consumed = 0
    frame_count = 0
    for frame_count, (offset, frame) in enumerate(iter_frames(data), start=1):
        consumed = offset + len(frame)
        print(describe_frame(frame_count, offset, frame))

    if frame_count == 0:
        print("No complete ButterFi frames decoded.")

    if consumed < len(data):
        print(f"trailing_bytes: {data[consumed:].hex()}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())