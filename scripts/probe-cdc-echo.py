#!/usr/bin/env python3

import fcntl
import os
import select
import struct
import termios
import time


TTY = "/dev/ttyACM0"
PAYLOAD = b"ping\r\n"


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
        os.write(fd, PAYLOAD)

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

        print(data.decode("utf-8", errors="replace"))
        return 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    raise SystemExit(main())