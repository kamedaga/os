#!/usr/bin/env python3
import os
import socket
import sys
import time


def main() -> int:
    console = os.environ["PACGO_QEMU_CONSOLE"]
    timeout = float(os.environ.get("PACGO_QEMU_TIMEOUT_SECONDS", "60"))
    deadline = time.monotonic() + timeout
    output = bytearray()

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(0.2)
        sock.connect(console)
        sock.sendall(b"/bin/fastfetch\n")
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(4096)
            except TimeoutError:
                continue
            if not chunk:
                break
            output.extend(chunk)
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            if b"PachaOS" in output:
                return 0

    sys.stderr.write("missing expected console output: PachaOS\n")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
