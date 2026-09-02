#!/usr/bin/env python3
import os
import select
import socket
import sys
import time


PROMPT = b"bash-5.3# "


def wait_for(sock: socket.socket, output: bytearray, needle: bytes, deadline: float) -> None:
    while time.monotonic() < deadline:
        if needle in output:
            return
        readable, _, _ = select.select([sock], [], [], 0.1)
        if not readable:
            continue
        chunk = sock.recv(4096)
        if not chunk:
            raise EOFError("virtio console closed")
        output.extend(chunk)
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
    raise AssertionError(f"missing {needle!r}; tail={bytes(output[-4000:])!r}")


def run_command(
    sock: socket.socket,
    output: bytearray,
    command: bytes,
    marker: bytes,
    deadline: float,
) -> bytes:
    output.clear()
    sock.sendall(command + b"; printf '\n" + marker + b"=%s\n' \"$?\"\n")
    wait_for(sock, output, PROMPT, deadline)
    result = bytes(output)
    if marker + b"=0" not in result:
        raise AssertionError(f"command failed marker={marker!r}; tail={result[-4000:]!r}")
    return result


def main() -> int:
    timeout = float(os.environ.get("PACGO_QEMU_TIMEOUT_SECONDS", "90"))
    deadline = time.monotonic() + timeout
    output = bytearray()
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.connect(os.environ["PACGO_QEMU_CONSOLE"])
        sock.setblocking(False)
        wait_for(sock, output, PROMPT, deadline)
        run_command(sock, output, b"apk update", b"APK_UPDATE_STATUS", deadline)
        install_output = run_command(
            sock,
            output,
            b"apk -v add gedit",
            b"APK_GEDIT_STATUS",
            deadline,
        )
        run_command(
            sock,
            output,
            b"apk info -e gedit && test -x /usr/bin/gedit",
            b"APK_GEDIT_INSTALLED_STATUS",
            deadline,
        )
        if b"execve: Exec format error" in install_output:
            raise AssertionError("gedit trigger returned an exec format error")
        print("\nAPK_GEDIT_QEMU=OK", flush=True)
        return 0
    except Exception as exc:
        print(f"APK_GEDIT_QEMU=FAIL error={exc}", file=sys.stderr, flush=True)
        return 1
    finally:
        sock.close()


if __name__ == "__main__":
    raise SystemExit(main())
