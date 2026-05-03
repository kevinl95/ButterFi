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
DEFAULT_PAYLOAD = "ping"

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
        description="Write a test line to the ButterFi USB CDC port and print the response."
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
    parser.add_argument(
        "--payload",
        default=os.environ.get("BUTTERFI_PAYLOAD", DEFAULT_PAYLOAD),
        help="Text payload to send. A trailing CRLF is added automatically.",
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


def main() -> int:
    args = parse_args()

    try:
        fd = open_serial_device(args.tty, args.baud)
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return 1

    try:
        time.sleep(args.wake_delay)
        payload = args.payload.encode("utf-8") + b"\r\n"
        os.write(fd, payload)
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

    response = data.decode("utf-8", errors="replace")
    print(response, end="" if response.endswith("\n") else "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())