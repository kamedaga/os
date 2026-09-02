#!/usr/bin/env python3
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
    deadline: float,
    console_log,
    start: int = 0,
) -> int:
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
    overall_deadline: float,
    console_log,
    expected_status: int | None = 0,
) -> bytes:
    output.clear()
    sock.sendall(command + b"; printf '\n" + marker + b"=%s\n' \"$?\"\n")
    deadline = min(time.monotonic() + timeout, overall_deadline)
    expected = marker + b"="
    if expected_status is not None:
        expected += str(expected_status).encode()
    marker_position = wait_for(
        sock,
        output,
        expected,
        deadline,
        console_log,
    )
    wait_for(
        sock,
        output,
        PROMPT,
        deadline,
        console_log,
        marker_position + len(expected),
    )
    result = bytes(output)
    if expected_status is not None:
        if expected not in result:
            raise AssertionError(
                f"command failed marker={marker!r}; tail={result[-4000:]!r}"
            )
    return result


def main() -> int:
    overall_timeout = float(
        os.environ.get("APK_GEDIT_GUEST_TIMEOUT_SECONDS", "120")
    )
    overall_deadline = time.monotonic() + overall_timeout
    output = bytearray()
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    console_log_path = os.environ.get("PACGO_QEMU_CONSOLE_LOG")
    console_log = (
        open(console_log_path, "ab", buffering=0) if console_log_path else None
    )
    try:
        sock.connect(os.environ["PACGO_QEMU_CONSOLE"])
        sock.setblocking(False)
        sock.sendall(b"\n")
        wait_for(
            sock,
            output,
            PROMPT,
            min(time.monotonic() + 30.0, overall_deadline),
            console_log,
        )
        pthread_output = run_command(
            sock,
            output,
            b"bash /cmd/lpr_pthread_smoke.sh",
            b"PTHREAD_SMOKE_STATUS",
            30.0,
            overall_deadline,
            console_log,
        )
        if pthread_output.count(b"LPR_PTHREAD_FUTEX_WAIT_BITSET=OK") != 2:
            raise AssertionError("static/dynamic WAIT_BITSET regression missing")
        if pthread_output.count(b"LPR_PTHREAD_FUTEX_TIMEOUT_STALE=OK") != 2:
            raise AssertionError("static/dynamic stale-futex regression missing")
        run_command(
            sock,
            output,
            b"apk update",
            b"APK_UPDATE_STATUS",
            30.0,
            overall_deadline,
            console_log,
        )
        run_command(
            sock,
            output,
            b"apk -v add gedit",
            b"APK_GEDIT_STATUS",
            90.0,
            overall_deadline,
            console_log,
        )
        bwrap_output = run_command(
            sock,
            output,
            b"bwrap --unshare-all --die-with-parent --chdir / --ro-bind /usr /usr --dev /dev -- /usr/bin/true",
            b"BWRAP_STATUS",
            10.0,
            overall_deadline,
            console_log,
            expected_status=None,
        )
        if b"Creating new namespace failed" not in bwrap_output:
            raise AssertionError(
                f"bwrap did not reach namespace probe; tail={bwrap_output[-4000:]!r}"
            )
        for unexpected in (
            b"Socket type not supported",
            b"capget failed",
            b"capget (for uid == 0) failed",
        ):
            if unexpected in bwrap_output:
                raise AssertionError(
                    f"bwrap failed before namespace probe: {unexpected!r}"
                )
        run_command(
            sock,
            output,
            b"rm -f /tmp/sway-wallpaper.png; gdk-pixbuf-thumbnailer /usr/share/backgrounds/sway/Sway_Wallpaper_Blue_1920x1080.png /tmp/sway-wallpaper.png && test -s /tmp/sway-wallpaper.png",
            b"GLYCIN_PNG_STATUS",
            25.0,
            overall_deadline,
            console_log,
        )
        run_command(
            sock,
            output,
            b"rm -f /tmp/image-missing.png; gdk-pixbuf-thumbnailer /usr/share/icons/Adwaita/scalable/status/image-missing.svg /tmp/image-missing.png && test -s /tmp/image-missing.png",
            b"GLYCIN_SVG_STATUS",
            25.0,
            overall_deadline,
            console_log,
        )
        print("\nAPK_GEDIT_GLYCIN=OK", flush=True)
        return 0
    except Exception as exc:
        print(f"APK_GEDIT_GLYCIN=FAIL error={exc}", file=sys.stderr, flush=True)
        return 1
    finally:
        sock.close()
        if console_log is not None:
            console_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
