#!/usr/bin/env python3
import argparse
import subprocess
import sys


def default_command(package: str) -> str:
    return (
        "mkdir -p /tmp/apk-root/etc/apk "
        "/tmp/apk-root/lib/apk/db "
        "/tmp/apk-root/lib/apk/exec "
        "/tmp/apk-root/var/lib/apk "
        "/tmp/apk-root/var/cache/apk; "
        "mkdir -p /tmp/apk-root/bin "
        "/tmp/apk-root/sbin "
        "/tmp/apk-root/usr/bin "
        "/tmp/apk-root/usr/sbin "
        "/tmp/apk-root/dev; "
        "touch /tmp/apk-root/dev/null; "
        "cp /etc/apk/repositories /tmp/apk-root/etc/apk/repositories; "
        "cp -R /etc/apk/keys /tmp/apk-root/etc/apk/keys; "
        f"apk --no-progress --root /tmp/apk-root --initdb add {package}; "
        f"apk --root /tmp/apk-root info -e {package}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Boot CapabilityOS and run one apk add smoke command.")
    parser.add_argument("--out", default=".artifacts/apk-add-smoke")
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--package", default="zlib")
    parser.add_argument("--command", default=None, help="override command run inside CapabilityOS")
    args = parser.parse_args()

    command = args.command
    if command is None:
        command = default_command(args.package)

    cmd = [
        sys.executable,
        "tools/apk_update_smoke.py",
        "--out",
        args.out,
        "--timeout",
        str(args.timeout),
        "--command",
        command,
    ]
    return subprocess.run(cmd).returncode


if __name__ == "__main__":
    raise SystemExit(main())
