#!/usr/bin/env python3
from __future__ import annotations

import os
import re
import select
import socket
import sys
import time


PROMPT = b"bash-5.3# "
DONE = b"__FASTFETCH_RC="
ANSI = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]")


def wait_for(
    console: socket.socket,
    output: bytearray,
    needle: bytes,
    deadline: float,
) -> None:
    while time.monotonic() < deadline:
        if needle in output:
            return
        readable, _, _ = select.select([console], [], [], 0.1)
        if not readable:
            continue
        chunk = console.recv(65536)
        if not chunk:
            raise EOFError("virtio console closed")
        output.extend(chunk)
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
    raise AssertionError(f"missing {needle!r}; tail={bytes(output[-8000:])!r}")


def host_cpu_model() -> bytes:
    with open("/proc/cpuinfo", "rb") as cpuinfo:
        for line in cpuinfo:
            if line.startswith(b"model name"):
                return line.split(b":", 1)[1].strip()
    raise AssertionError("host /proc/cpuinfo has no model name")


def require(output: bytes, needle: bytes) -> None:
    if needle not in output:
        raise AssertionError(f"missing {needle!r}; output={output[-12000:]!r}")


def main() -> int:
    deadline = time.monotonic() + float(
        os.environ.get("PACGO_QEMU_TIMEOUT_SECONDS", "120")
    )
    output = bytearray()
    console = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        console.connect(os.environ["PACGO_QEMU_CONSOLE"])
        console.setblocking(False)
        wait_for(console, output, PROMPT, deadline)
        output.clear()
        console.sendall(
            b"if command -v fastfetch >/dev/null 2>&1; then "
            b"echo '__FASTFETCH_DEFAULT=PRESENT__'; else "
            b"echo '__FASTFETCH_DEFAULT=ABSENT__'; "
            b"apk add --no-progress fastfetch; fi; "
            b"printf '__FASTFETCH_INSTALL_RC=%d__\\n' $?; "
            b"uname -a; "
            b"head -n 8 /proc/cpuinfo; printf '__CPUINFO_RC=%d__\\n' $?; "
            b"flag_line=; while IFS= read -r line; do case \"$line\" in "
            b"flags*) flag_line=$line; break;; esac; done < /proc/cpuinfo; "
            b"case \" $flag_line \" in *\" fpu \"*\" sse2 \"*\" lm \"*) "
            b"echo '__CPUINFO_FLAGS=OK__';; *) "
            b"echo '__CPUINFO_FLAGS=FAIL__';; esac; "
            b"n=0; for entry in /proc/*; do name=${entry##*/}; "
            b"case $name in ''|*[!0-9]*) ;; *) n=$((n+1));; esac; done; "
            b"printf '__PROC_PID_COUNT=%d__\\n' $n; "
            b"fastfetch --config /etc/fastfetch/config.jsonc; "
            b"printf '__FASTFETCH_RC=%d__\\n' $?\n"
        )
        wait_for(console, output, DONE, deadline)
        wait_for(console, output, PROMPT, deadline)
        clean = ANSI.sub(b"", bytes(output)).replace(b"\r", b"")

        require(
            clean,
            b"Linux pachaos 6.12.0 PachaOS Linux shim x86_64 GNU/Linux",
        )
        require(clean, b"OS: ")
        require(clean, b"Alpine Linux")
        require(clean, b"Uptime: ")
        require(clean, b"CPU: ")
        require(clean, host_cpu_model())
        require(clean, b"Memory: ")
        require(clean, b"Swap: ")
        require(clean, b"Disk: ")
        require(clean, b"(ext4)")
        require(clean, b"__FASTFETCH_DEFAULT=ABSENT__")
        require(clean, b"__FASTFETCH_INSTALL_RC=0__")
        require(clean, b"__CPUINFO_RC=0__")
        require(clean, b"__CPUINFO_FLAGS=OK__")
        require(clean, b"__FASTFETCH_RC=0__")

        pid_count = re.search(rb"__PROC_PID_COUNT=([0-9]+)__", clean)
        if pid_count is None or int(pid_count.group(1)) == 0:
            raise AssertionError("/proc did not enumerate any live process IDs")
        processes = re.search(rb"Processes:\s*([0-9]+)", clean)
        if processes is None or int(processes.group(1)) == 0:
            raise AssertionError("fastfetch still reports zero processes")

        forbidden = (
            b"clock_gettime(CLOCK_BOOTTIME) failed",
            b'ffReadFileBuffer("/proc/cpuinfo") failed',
            b'ffReadFileData("/proc/meminfo"',
            b'setmntent("/proc/mounts", "r") == NULL',
        )
        for message in forbidden:
            if message in clean:
                raise AssertionError(f"fastfetch still reported {message!r}")

        output.clear()
        console.sendall(
            b"rm -f /tmp/fastfetch-pty-rc; "
            b"printf '%s\\n' 'swaybg_command -' 'xwayland disable' "
            b"'exec /usr/bin/foot /bin/bash -c \"fastfetch --config "
            b"/etc/fastfetch/config.jsonc; printf %s \\$? "
            b">/tmp/fastfetch-pty-rc\"' >/tmp/fastfetch-pty-sway.conf; "
            b"/usr/bin/sway -c /tmp/fastfetch-pty-sway.conf "
            b">/tmp/fastfetch-pty-sway.log 2>&1 & sway_pid=$!; "
            b"ticks=0; while test $ticks -lt 900 && "
            b"test ! -r /tmp/fastfetch-pty-rc && kill -0 $sway_pid 2>/dev/null; "
            b"do sleep 0.1; ticks=$((ticks+1)); done; "
            b"if test -r /tmp/fastfetch-pty-rc; then "
            b"rc=$(head -n 1 /tmp/fastfetch-pty-rc); "
            b"printf '__FASTFETCH_PTY_%s=%s__\\n' RC \"$rc\"; else "
            b"tail -n 120 /tmp/fastfetch-pty-sway.log; "
            b"printf '__FASTFETCH_PTY_%s=%s__\\n' RC HUNG; fi; "
            b"kill $sway_pid 2>/dev/null || true; wait $sway_pid 2>/dev/null || true\n"
        )
        wait_for(console, output, b"__FASTFETCH_PTY_RC=0__", deadline)
        wait_for(console, output, PROMPT, deadline)
        serial_path = os.environ.get("PACGO_QEMU_SERIAL_LOG")
        if serial_path:
            with open(serial_path, "rb") as serial_file:
                serial = serial_file.read()
            if b"[termd] op=9 status=-512" in serial:
                raise AssertionError("termd leaked ERESTARTSYS from a PTY ioctl")
        print("\nFASTFETCH_SYSTEM_INFO_QEMU=OK", flush=True)
        return 0
    except Exception as exc:
        print(f"FASTFETCH_SYSTEM_INFO_QEMU=FAIL error={exc}", file=sys.stderr)
        return 1
    finally:
        console.close()


if __name__ == "__main__":
    raise SystemExit(main())
