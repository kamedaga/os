#!/usr/bin/env python3
import os
import select
import socket
import sys
import time
import re


PROMPT = b"bash-5.3# "


class Console:
    def __init__(self) -> None:
        self.deadline = time.monotonic() + float(
            os.environ.get("PACGO_QEMU_TIMEOUT_SECONDS", "45")
        )
        self.output = bytearray()
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(os.environ["PACGO_QEMU_CONSOLE"])
        self.sock.setblocking(False)

    def close(self) -> None:
        self.sock.close()

    def wait_for(self, needle: bytes, timeout: float) -> None:
        end = min(self.deadline, time.monotonic() + timeout)
        while time.monotonic() < end:
            if needle in self.output:
                return
            readable, _, _ = select.select([self.sock], [], [], 0.1)
            if not readable:
                continue
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("virtio console closed")
            self.output.extend(chunk)
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
        raise AssertionError(f"missing {needle!r}; tail={bytes(self.output[-2000:])!r}")

    def clear(self) -> None:
        self.output.clear()

    def line(self, command: str) -> None:
        self.sock.sendall(command.encode() + b"\n")

    def ctrl_c(self) -> None:
        self.sock.sendall(b"\x03")

    def run_to_prompt(self, command: str, timeout: float = 3.0) -> None:
        self.clear()
        self.line(command)
        self.wait_for(PROMPT, timeout)

    def interrupt_status(self, label: str) -> tuple[float, int]:
        self.clear()
        self.line(os.environ.get("TTY_SIGNAL_COMMAND", "busybox sleep 3"))
        time.sleep(0.25)
        started = time.monotonic()
        self.ctrl_c()
        self.wait_for(PROMPT, 4.5)
        elapsed = time.monotonic() - started
        marker = f"TTY_SIGNAL_{label}_STATUS=".encode()
        self.clear()
        self.line(f"echo TTY_SIGNAL_{label}_STATUS=$?")
        self.wait_for(marker, 2.0)
        self.wait_for(PROMPT, 2.0)
        match = re.search(re.escape(marker) + rb"([0-9]+)", bytes(self.output))
        if match is not None:
            return elapsed, int(match.group(1))
        raise AssertionError(f"missing parsed status for {label}")


def main() -> int:
    console = Console()
    try:
        console.wait_for(PROMPT, 10.0)
        before_elapsed, before_status = console.interrupt_status("BEFORE")
        print(
            f"\nTTY_SIGNAL_BEFORE_RESULT elapsed={before_elapsed:.3f} status={before_status}",
            flush=True,
        )
        if before_elapsed >= 1.5:
            raise AssertionError(f"baseline Ctrl-C took {before_elapsed:.3f}s")

        for iteration in range(80):
            console.run_to_prompt("busybox true")
            if iteration in (15, 31, 47, 63, 79):
                print(f"TTY_SIGNAL_PID_CHURN={iteration + 1}", flush=True)

        after_elapsed, after_status = console.interrupt_status("AFTER")
        print(
            f"\nTTY_SIGNAL_AFTER_RESULT elapsed={after_elapsed:.3f} status={after_status}",
            flush=True,
        )
        if after_elapsed >= 1.5:
            raise AssertionError(f"post-churn Ctrl-C took {after_elapsed:.3f}s")
        if before_status != 130 or after_status != 130:
            raise AssertionError(
                f"Ctrl-C wait status is before={before_status} after={after_status}, expected 130"
            )
        print("TTY_SIGNAL_LIFETIME=OK", flush=True)
        return 0
    except Exception as exc:
        print(f"TTY_SIGNAL_LIFETIME=FAIL error={exc}", file=sys.stderr, flush=True)
        return 1
    finally:
        console.close()


if __name__ == "__main__":
    raise SystemExit(main())
