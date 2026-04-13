#!/usr/bin/env python3
import sys
import time


def main() -> int:
    start = None
    pending = b""
    source = sys.stdin.buffer
    out = sys.stdout

    while True:
        chunk = source.read1(4096)
        if not chunk:
            break
        pending += chunk
        while True:
            newline = pending.find(b"\n")
            if newline < 0:
                break
            raw_line = pending[: newline + 1]
            pending = pending[newline + 1 :]
            now = time.perf_counter()
            if start is None:
                start = now
            elapsed_ms = (now - start) * 1000.0
            text = raw_line.decode("utf-8", errors="replace")
            out.write(f"[+{elapsed_ms:9.3f} ms] {text}")
            out.flush()

    if pending:
        now = time.perf_counter()
        if start is None:
            start = now
        elapsed_ms = (now - start) * 1000.0
        text = pending.decode("utf-8", errors="replace")
        out.write(f"[+{elapsed_ms:9.3f} ms] {text}")
        out.flush()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
