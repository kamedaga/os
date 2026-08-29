#!/usr/bin/env python3
"""Relay a pacgo virtio-console socket to this terminal in real time."""

from __future__ import annotations

import argparse
import os
import select
import socket
import sys
import termios
import tty


DETACH = b"\x1d"  # Ctrl-]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("socket", help="pacgo virtio-console Unix socket")
    parser.add_argument("--log", help="optional raw console log")
    args = parser.parse_args()

    console = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    console.connect(args.socket)
    console.setblocking(False)
    log = open(args.log, "ab", buffering=0) if args.log else None
    stdin_fd = sys.stdin.fileno()
    stdout_fd = sys.stdout.fileno()
    old_terminal = termios.tcgetattr(stdin_fd) if os.isatty(stdin_fd) else None

    try:
        if old_terminal is not None:
            tty.setraw(stdin_fd)
        while True:
            readable, _, _ = select.select([stdin_fd, console], [], [])
            if console in readable:
                chunk = console.recv(65536)
                if not chunk:
                    return 0
                os.write(stdout_fd, chunk)
                if log is not None:
                    log.write(chunk)
            if stdin_fd in readable:
                chunk = os.read(stdin_fd, 4096)
                if not chunk:
                    return 0
                if DETACH in chunk:
                    before, _, _ = chunk.partition(DETACH)
                    if before:
                        console.sendall(before)
                    return 0
                console.sendall(chunk)
    finally:
        if old_terminal is not None:
            termios.tcsetattr(stdin_fd, termios.TCSADRAIN, old_terminal)
        if log is not None:
            log.close()
        console.close()


if __name__ == "__main__":
    raise SystemExit(main())
