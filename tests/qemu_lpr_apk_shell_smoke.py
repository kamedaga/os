#!/usr/bin/env python3
import os
import select
import socket
import sys
import time


PROMPT = b"bash-5.3# "
MARKERS = (
    b"APK_SHELL_VERSION=OK",
    b"APK_SHELL_UPDATE=OK",
    b"APK_SHELL_NANO_CYCLES=OK iterations=3",
    b"APK_SHELL_ADD_GREP=OK",
    b"APK_SHELL_ADD_WGET=OK",
    b"APK_SHELL_ADD_FASTFETCH=OK",
    b"APK_SHELL_FINAL_NANO_ABSENT=OK",
    b"APK_SHELL_SYNC=OK",
)


def wait_for(
    sock: socket.socket,
    output: bytearray,
    needle: bytes,
    deadline: float,
    fail_on_prompt: bool = False,
) -> None:
    while time.monotonic() < deadline:
        if needle in output:
            return
        if fail_on_prompt and PROMPT in output:
            raise AssertionError(
                f"shell returned before {needle!r}; tail={bytes(output[-4000:])!r}"
            )
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


def main() -> int:
    timeout = float(os.environ.get("PACGO_QEMU_TIMEOUT_SECONDS", "240"))
    deadline = time.monotonic() + timeout
    output = bytearray()
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.connect(os.environ["PACGO_QEMU_CONSOLE"])
        sock.setblocking(False)
        wait_for(sock, output, PROMPT, deadline)
        output.clear()
        sock.sendall(b"bash /cmd/apk_shell_smoke.sh\n")
        for marker in MARKERS:
            wait_for(sock, output, marker, deadline, fail_on_prompt=True)
        wait_for(sock, output, PROMPT, deadline)
        if output.count(b"APK_SHELL_NANO_ADD iteration=") != 3:
            raise AssertionError("actual apk nano add cycle count was not 3")
        if output.count(b"APK_SHELL_NANO_DEL iteration=") != 3:
            raise AssertionError("actual apk nano del cycle count was not 3")
        print("\nAPK_SHELL_QEMU=OK", flush=True)
        return 0
    except Exception as exc:
        print(f"APK_SHELL_QEMU=FAIL error={exc}", file=sys.stderr, flush=True)
        return 1
    finally:
        sock.close()


if __name__ == "__main__":
    raise SystemExit(main())
