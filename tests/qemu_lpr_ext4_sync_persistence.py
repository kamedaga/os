#!/usr/bin/env python3
import os
import select
import socket
import sys
import time


PROMPT = b"bash-5.3# "
PERSIST_PATH = "/p"
PERSIST_TEXT = "p"
PERSIST_DIR = "/kame"


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
            raise TimeoutError("qemu ext4 sync persistence smoke timed out")
        return remaining if per_call is None else min(remaining, per_call)

    def _record(self, chunk: bytes) -> None:
        self.output.extend(chunk)
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        if self.log is not None:
            self.log.write(chunk)

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
        print(f"\n[qemu-lpr-ext4-sync-persistence] run: {name}", flush=True)
        self.output.clear()
        self.sock.sendall(line.encode("utf-8") + b"\n")
        for expect in expects:
            self.wait_for(expect, name)
        self.wait_for(PROMPT, name)


def main() -> int:
    phase = os.environ.get("PACHA_EXT4_SYNC_PHASE", "write")
    console = Console(
        os.environ["PACGO_QEMU_CONSOLE"],
        os.environ.get("PACGO_QEMU_CONSOLE_LOG"),
        float(os.environ.get("PACGO_QEMU_TIMEOUT_SECONDS", "30")),
    )
    try:
        console.wait_for(PROMPT, "initial prompt", per_call_timeout=10.0)
        if phase == "write":
            console.run("cleanup stale file", f"/bin/rm {PERSIST_PATH}; /bin/echo clean-file", [b"\r\nclean-file\r\n"])
            console.run("cleanup stale dir", f"/bin/rmdir {PERSIST_DIR}; /bin/echo clean-dir", [b"\r\nclean-dir\r\n"])
            console.run("write file", f"/bin/echo {PERSIST_TEXT} >{PERSIST_PATH}", [])
            console.run("cat written file", f"/bin/cat {PERSIST_PATH}", [f"\r\n{PERSIST_TEXT}\r\n".encode("utf-8")])
            console.run("ls sees file", "/bin/ls /", [b"\r\np\r\n"])
            console.run("mkdir autosync", f"/bin/mkdir {PERSIST_DIR}; /bin/echo mkdir-rc=$?; /bin/sleep 2; /bin/echo dok", [b"\r\nmkdir-rc=0\r\n", b"\r\ndok\r\n"])
            console.run("ls sees autosync dir", "/bin/ls /", [b"\r\nkame\r\n"])
            console.run("sync file", "/bin/sync", [])
            console.run("write ok", "/bin/echo wok", [b"\r\nwok\r\n"])
        elif phase == "read":
            console.run("read persisted", f"/bin/cat {PERSIST_PATH}", [f"\r\n{PERSIST_TEXT}\r\n".encode("utf-8")])
            console.run("ls still sees file", "/bin/ls /", [b"\r\np\r\n"])
            console.run("ls still sees autosync dir", "/bin/ls /", [b"\r\nkame\r\n"])
            console.run("remove file", f"/bin/rm {PERSIST_PATH}", [])
            console.run("remove autosync dir", f"/bin/rmdir {PERSIST_DIR}", [])
            console.run("sync remove", "/bin/sync", [])
            console.run("read ok", "/bin/echo rok", [b"\r\nrok\r\n"])
        else:
            raise ValueError(f"unknown phase: {phase}")
        print(f"\n[qemu-lpr-ext4-sync-persistence] {phase} passed", flush=True)
        return 0
    except Exception as exc:
        sys.stderr.write(f"\n[qemu-lpr-ext4-sync-persistence] failed: {exc}\n")
        return 1
    finally:
        console.close()


if __name__ == "__main__":
    raise SystemExit(main())
