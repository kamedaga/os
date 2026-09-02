#!/usr/bin/env python3
"""Run the futex ping-pong benchmark while streaming the guest console."""

from __future__ import annotations

import os
import select
import socket
import sys
import time
from pathlib import Path


PROMPTS = (b"bash-5.2# ", b"bash-5.3# ")
DONE = b"LPR_FUTEX_PINGPONG_DONE"
FAULT_MARKERS = (
    b"GENERAL PROTECTION",
    b"PAGE FAULT",
    b"TableFull",
    b"kobox rwsem: long wait",
)


def main() -> int:
    timeout = float(os.environ.get("PACGO_QEMU_TIMEOUT_SECONDS", "30"))
    deadline = time.monotonic() + timeout
    console = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    output = bytearray()
    console_log = open(os.environ["PACGO_QEMU_CONSOLE_LOG"], "ab", buffering=0)
    try:
        console.connect(os.environ["PACGO_QEMU_CONSOLE"])
        console.setblocking(False)
        console.sendall(b"\n")
        sent = False
        while time.monotonic() < deadline:
            readable, _, _ = select.select([console], [], [], 0.1)
            if not readable:
                continue
            chunk = console.recv(65536)
            if not chunk:
                raise EOFError("virtio console closed")
            output.extend(chunk)
            console_log.write(chunk)
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            if not sent and any(prompt in output for prompt in PROMPTS):
                console.sendall(b"bash /cmd/lpr_futex_pingpong_bench.sh 64 3\n")
                sent = True
                output.clear()
            if sent and DONE in output:
                break
        else:
            raise TimeoutError("futex benchmark timed out")

        serial = Path(os.environ["PACGO_QEMU_SERIAL_LOG"]).read_bytes()
        for marker in FAULT_MARKERS:
            if marker in serial:
                raise RuntimeError(f"serial fault marker: {marker!r}")
        print("\nLPR_FUTEX_PINGPONG_BENCH=OK", flush=True)
        return 0
    except Exception as exc:
        print(f"LPR_FUTEX_PINGPONG_BENCH=FAIL error={exc}", file=sys.stderr)
        return 1
    finally:
        console.close()
        console_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
