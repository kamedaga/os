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
            raise TimeoutError("qemu tty smoke timed out")
        if per_call is None:
            return remaining
        return min(remaining, per_call)

    def _record(self, chunk: bytes) -> None:
        self.output.extend(chunk)
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        if self.log is not None:
            self.log.write(chunk)

    def read_available(self, duration: float = 0.15) -> None:
        end = time.monotonic() + duration
        while time.monotonic() < end:
            timeout = min(0.05, end - time.monotonic())
            readable, _, _ = select.select([self.sock], [], [], max(timeout, 0.0))
            if not readable:
                continue
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("virtio console closed")
            self._record(chunk)

    def wait_for(self, needle: bytes, label: str, per_call_timeout: float | None = None) -> None:
        end = time.monotonic() + self._remaining(per_call_timeout)
        while time.monotonic() < end:
            if needle in self.output:
                return
            readable, _, _ = select.select([self.sock], [], [], min(0.1, end - time.monotonic()))
            if not readable:
                continue
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("virtio console closed")
            self._record(chunk)
        raise AssertionError(f"missing expected console output for {label}: {needle!r}\n{self.tail()}")

    def tail(self, size: int = 3000) -> str:
        return bytes(self.output[-size:]).decode("utf-8", errors="replace")

    def send_line(self, line: str, *, char_delay: float = 0.0, enter_delay: float = 0.0) -> None:
        data = line.encode("utf-8")
        if char_delay > 0:
            for byte in data:
                self.sock.sendall(bytes([byte]))
                self.read_available(0.005)
                time.sleep(char_delay)
            if enter_delay > 0:
                self.read_available(enter_delay)
        else:
            self.sock.sendall(data)
            if enter_delay > 0:
                self.read_available(enter_delay)
        self.sock.sendall(b"\n")

    def run(self, name: str, line: str, expects: list[bytes], *, slow: bool = False) -> None:
        print(f"\n[qemu-lpr-fd-pipe-smoke] run: {name}", flush=True)
        self.read_available(0.12)
        self.output.clear()
        if slow:
            self.send_line(line, char_delay=0.006, enter_delay=0.35)
        else:
            self.send_line(line)
        for expect in expects:
            self.wait_for(expect, name, per_call_timeout=8.0)
        self.wait_for(PROMPT, name, per_call_timeout=8.0)


def main() -> int:
    console_path = os.environ["PACGO_QEMU_CONSOLE"]
    console_log = os.environ.get("PACGO_QEMU_CONSOLE_LOG")
    timeout = float(os.environ.get("PACGO_QEMU_TIMEOUT_SECONDS", "30"))

    console = Console(console_path, console_log, timeout)
    try:
        console.wait_for(PROMPT, "initial prompt", per_call_timeout=10.0)
        console.run("pipe cat", "echo hi|busybox cat", [b"\r\nhi\r\n"])
        console.run("pipe wc", "echo hi|busybox wc -c", [b"\r\n3\r\n"])
        console.run("printf pipe wc", "printf zz|busybox wc -c", [b"\r\n2\r\n"])
        console.run(
            "dup2 stderr to stdout through pipe",
            "busybox sh -c 'echo DUP2_ERR >&2' 2>&1 | busybox cat",
            [b"\r\nDUP2_ERR\r\n"],
        )
        console.run(
            "yes head wc pipe chain",
            "busybox yes y 2>/dev/null | busybox head -n 3 | busybox wc -l",
            [b"\r\n3\r\n"],
        )
        console.run(
            "read -n redirection keeps fd offset",
            "busybox sh -c 'printf abcd >/tmp/f; exec 8</tmp/f; read -r -n 2 a <&8; read -r -n 2 b <&8; echo ${a}${b}'",
            [b"\r\nabcd\r\n"],
        )
        console.run(
            "exec fd inheritance through cat",
            "busybox sh -c 'printf inherit >/tmp/inherit_fd; exec 8</tmp/inherit_fd; busybox cat <&8'",
            [b"\r\ninherit"],
        )
        console.run(
            "slow long tty command",
            "busybox sh -c 'printf \"slowtty\\n\" >/tmp/slowtty_input_file; busybox cat /tmp/slowtty_input_file; echo LONG_TTY_OK'",
            [b"\r\nslowtty\r\n", b"\r\nLONG_TTY_OK\r\n"],
            slow=True,
        )
        print("\n[qemu-lpr-fd-pipe-smoke] passed", flush=True)
        return 0
    except Exception as exc:
        sys.stderr.write(f"\n[qemu-lpr-fd-pipe-smoke] failed: {exc}\n")
        return 1
    finally:
        console.close()


if __name__ == "__main__":
    raise SystemExit(main())
