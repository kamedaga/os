#!/usr/bin/env python3
"""Short live-console probe for Glycin after installing the current LPR."""

from __future__ import annotations

import os
import select
import socket
import sys
import time


PROMPT = b"bash-5.3# "


def wait_for(
    sock: socket.socket,
    output: bytearray,
    needle: bytes,
    timeout: float,
    console_log,
    start: int = 0,
) -> int:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        position = output.find(needle, start)
        if position >= 0:
            return position
        readable, _, _ = select.select([sock], [], [], 0.1)
        if not readable:
            continue
        chunk = sock.recv(4096)
        if not chunk:
            raise EOFError("virtio console closed")
        output.extend(chunk)
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        if console_log is not None:
            console_log.write(chunk)
    raise AssertionError(f"missing {needle!r}; tail={bytes(output[-4000:])!r}")


def run_command(
    sock: socket.socket,
    output: bytearray,
    command: bytes,
    marker: bytes,
    timeout: float,
    console_log,
) -> bytes:
    output.clear()
    sock.sendall(command + b"; printf '\n" + marker + b"=%s\n' \"$?\"\n")
    expected = marker + b"=0"
    marker_position = wait_for(sock, output, expected, timeout, console_log)
    wait_for(
        sock,
        output,
        PROMPT,
        timeout,
        console_log,
        marker_position + len(expected),
    )
    return bytes(output)


def main() -> int:
    output = bytearray()
    console = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    console_log_path = os.environ.get("PACGO_QEMU_CONSOLE_LOG")
    console_log = (
        open(console_log_path, "ab", buffering=0) if console_log_path else None
    )
    try:
        console.connect(os.environ["PACGO_QEMU_CONSOLE"])
        console.setblocking(False)
        console.sendall(b"\n")
        wait_for(console, output, PROMPT, 20.0, console_log)
        run_command(
            console,
            output,
            b"/bin/busybox wget -q "
            b"http://10.0.2.2:18080/.artifacts/lpr-linux-x86_64.so "
            b"-O /tmp/lpr-current.so && chmod 755 /tmp/lpr-current.so && "
            b"mv /tmp/lpr-current.so /lib/pacha/lpr-linux-x86_64.so && sync",
            b"LPR_INSTALL",
            15.0,
            console_log,
        )
        png_output = run_command(
            console,
            output,
            b"rm -f /tmp/glycin-wallpaper.png && "
            b"timeout 20 gdk-pixbuf-thumbnailer "
            b"/usr/share/backgrounds/sway/Sway_Wallpaper_Blue_1920x1080.png "
            b"/tmp/glycin-wallpaper.png && test -s /tmp/glycin-wallpaper.png",
            b"GLYCIN_PNG",
            25.0,
            console_log,
        )
        svg_output = run_command(
            console,
            output,
            b"rm -f /tmp/glycin-icon.png && "
            b"timeout 20 gdk-pixbuf-thumbnailer "
            b"/usr/share/icons/Adwaita/scalable/status/image-missing.svg "
            b"/tmp/glycin-icon.png && test -s /tmp/glycin-icon.png",
            b"GLYCIN_SVG",
            25.0,
            console_log,
        )
        combined = png_output + svg_output
        for fault in (
            b"GENERAL PROTECTION",
            b"PAGE FAULT",
            b"kobox rwsem: long wait",
        ):
            if fault in combined:
                raise AssertionError(f"fault in console output: {fault!r}")
        print("\nGLYCIN_RED_ZONE_QEMU=OK", flush=True)
        return 0
    except Exception as exc:
        print(f"GLYCIN_RED_ZONE_QEMU=FAIL error={exc}", file=sys.stderr, flush=True)
        return 1
    finally:
        console.close()
        if console_log is not None:
            console_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
