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
    status_marker: bytes,
    deadline: float,
) -> bytes:
    output.clear()
    sock.sendall(command + b"; printf '\\n" + status_marker + b"=%s\\n' \"$?\"\n")
    wait_for(sock, output, PROMPT, deadline)
    result = bytes(output)
    if status_marker + b"=0" not in result:
        raise AssertionError(f"command failed marker={status_marker!r}; tail={result[-4000:]!r}")
    if b"execve: Exec format error" in result:
        raise AssertionError(f"script exec returned ENOEXEC marker={status_marker!r}")
    return result


def main() -> int:
    timeout = float(os.environ.get("PACGO_QEMU_TIMEOUT_SECONDS", "1200"))
    deadline = time.monotonic() + timeout
    output = bytearray()
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.connect(os.environ["PACGO_QEMU_CONSOLE"])
        sock.setblocking(False)
        wait_for(sock, output, PROMPT, deadline)
        run_command(
            sock,
            output,
            (
                b"rm -f /var/cache/apk/lpr-symlink-smoke; "
                b"busybox ln -s symlink-target /var/cache/apk/lpr-symlink-smoke; "
                b"test \"$(readlink /var/cache/apk/lpr-symlink-smoke)\" = symlink-target"
            ),
            b"SYMLINK_STATUS",
            deadline,
        )
        run_command(
            sock,
            output,
            (
                b"update_status=1; "
                b"for attempt in 1 2 3 4 5 6 7 8 9 10; do "
                b"if apk update; then update_status=0; break; fi; sleep 2; done; "
                b"(exit \"$update_status\")"
            ),
            b"APK_UPDATE_STATUS",
            deadline,
        )
        gedit_output = run_command(
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
        zsh_output = run_command(
            sock,
            output,
            b"apk -v add zsh",
            b"APK_ZSH_STATUS",
            deadline,
        )
        run_command(
            sock,
            output,
            b"apk info -e zsh && test -x /bin/zsh",
            b"APK_ZSH_INSTALLED_STATUS",
            deadline,
        )
        print(
            "\nAPK_GEDIT_ZSH_QEMU=OK "
            f"gedit_exec_errors={gedit_output.count(b'execve: Exec format error')} "
            f"zsh_exec_errors={zsh_output.count(b'execve: Exec format error')}",
            flush=True,
        )
        return 0
    except Exception as exc:
        print(f"APK_GEDIT_ZSH_QEMU=FAIL error={exc}", file=sys.stderr, flush=True)
        return 1
    finally:
        sock.close()


if __name__ == "__main__":
    raise SystemExit(main())
