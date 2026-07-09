#!/usr/bin/env python3
import os
import select
import socket
import sys
import time


PROMPT = b"bash-5.3# "


class Console:
    def __init__(self, socket_path: str, log_path: str | None, timeout: float) -> None:
        self.deadline = time.monotonic() + timeout
        self.output = bytearray()
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(socket_path)
        self.sock.setblocking(False)
        self.log = open(log_path, "ab", buffering=0) if log_path else None

    def close(self) -> None:
        if self.log is not None:
            self.log.close()
        self.sock.close()

    def _remaining(self, per_call: float | None = None) -> float:
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("qemu coreutils mini smoke timed out")
        return remaining if per_call is None else min(remaining, per_call)

    def _record(self, chunk: bytes) -> None:
        self.output.extend(chunk)
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        if self.log is not None:
            self.log.write(chunk)

    def read_available(self, duration: float = 0.12) -> None:
        end = time.monotonic() + duration
        while time.monotonic() < end:
            readable, _, _ = select.select([self.sock], [], [], min(0.05, max(0.0, end - time.monotonic())))
            if not readable:
                continue
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("virtio console closed")
            self._record(chunk)

    def wait_for(self, needle: bytes, label: str, per_call_timeout: float = 8.0) -> None:
        end = time.monotonic() + self._remaining(per_call_timeout)
        while time.monotonic() < end:
            if needle in self.output:
                return
            readable, _, _ = select.select([self.sock], [], [], min(0.1, max(0.0, end - time.monotonic())))
            if not readable:
                continue
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("virtio console closed")
            self._record(chunk)
        raise AssertionError(f"missing expected output for {label}: {needle!r}\n{self.tail()}")

    def tail(self, size: int = 3000) -> str:
        return bytes(self.output[-size:]).decode("utf-8", errors="replace")

    def run(self, name: str, line: str, expects: list[bytes]) -> None:
        print(f"\n[qemu-lpr-coreutils-mini-smoke] run: {name}", flush=True)
        self.read_available()
        self.output.clear()
        self.sock.sendall(line.encode("utf-8") + b"\n")
        for expect in expects:
            self.wait_for(expect, name)
        self.wait_for(PROMPT, name)


def main() -> int:
    console = Console(
        os.environ["PACGO_QEMU_CONSOLE"],
        os.environ.get("PACGO_QEMU_CONSOLE_LOG"),
        float(os.environ.get("PACGO_QEMU_TIMEOUT_SECONDS", "30")),
    )
    try:
        console.wait_for(PROMPT, "initial prompt", per_call_timeout=10.0)
        console.run("coreutils list", "/bin/coreutils", [b"coreutils-mini commands:\r\n", b"cat coreutils echo", b" sleep sync tail"])
        console.run("coreutils version", "/bin/coreutils --version", [b"\r\ncoreutils-mini (PachaOS)\r\n"])
        console.run("echo cat pipe", "/bin/echo mini | /bin/cat", [b"\r\nmini\r\n"])
        console.run("printf wc pipe", "/bin/printf abc | /bin/wc -c", [b"\r\n3\r\n"])
        console.run("head", "/bin/printf 'a\\nb\\nc\\n' | /bin/head -n 2", [b"\r\na\r\nb\r\n"])
        console.run("tail", "/bin/printf 'a\\nb\\nc\\n' | /bin/tail -n 2", [b"\r\nb\r\nc\r\n"])
        console.run("grep", "/bin/printf 'aa\\nbb\\n' | /bin/grep bb", [b"\r\nbb\r\n"])
        console.run("yes head wc", "/bin/yes y | /bin/head -n 3 | /bin/wc -l", [b"\r\n3\r\n"])
        console.run("redirection cat", "/bin/printf redir >/tmp/coremini-f; /bin/cat </tmp/coremini-f", [b"\r\nredir"])
        console.run("ls sees file", "/bin/ls /tmp | /bin/grep coremini-f", [b"\r\ncoremini-f\r\n"])
        console.run("test true", "/bin/test -e /tmp/coremini-f && /bin/true && /bin/echo true-ok", [b"\r\ntrue-ok\r\n"])
        console.run("false", "/bin/false || /bin/echo false-ok", [b"\r\nfalse-ok\r\n"])
        console.run("mkdir", "/bin/mkdir /tmp/coremini-d && /bin/echo mkdir-ok", [b"\r\nmkdir-ok\r\n"])
        console.run("touch", "/bin/touch /tmp/coremini-d/x && /bin/test -f /tmp/coremini-d/x && /bin/echo touch-ok", [b"\r\ntouch-ok\r\n"])
        console.run("sync", "/bin/sync && /bin/echo sync-ok", [b"\r\nsync-ok\r\n"])
        console.run("rm file", "/bin/rm /tmp/coremini-d/x && /bin/echo rm-file-ok", [b"\r\nrm-file-ok\r\n"])
        console.run("rm dir", "/bin/rm /tmp/coremini-d && /bin/echo rm-dir-ok", [b"\r\nrm-dir-ok\r\n"])
        print("\n[qemu-lpr-coreutils-mini-smoke] passed", flush=True)
        return 0
    except Exception as exc:
        sys.stderr.write(f"\n[qemu-lpr-coreutils-mini-smoke] failed: {exc}\n")
        return 1
    finally:
        console.close()


if __name__ == "__main__":
    raise SystemExit(main())
