#!/usr/bin/env python3
import argparse
import fcntl
import json
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path


def socket_ping(socket_path: Path, timeout_s: float) -> bool:
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(timeout_s)
            client.connect(os.fspath(socket_path))
            client.sendall(b'{"cmd":"ping"}\n')
            data = client.recv(128)
    except OSError:
        return False

    try:
        message = json.loads(data.decode("utf-8"))
    except Exception:
        return False
    return message.get("ok") is True


def cmd_serve(args: argparse.Namespace) -> int:
    socket_path = Path(args.socket)
    socket_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        socket_path.unlink()
    except FileNotFoundError:
        pass

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
        server.bind(os.fspath(socket_path))
        server.listen()
        while True:
            conn, _addr = server.accept()
            with conn:
                try:
                    payload = conn.recv(128)
                    message = json.loads(payload.decode("utf-8"))
                except Exception:
                    conn.sendall(b'{"ok":false}\n')
                    continue

                if message.get("cmd") == "ping":
                    conn.sendall(b'{"ok":true}\n')
                else:
                    conn.sendall(b'{"ok":false}\n')


def cmd_ensure(args: argparse.Namespace) -> int:
    socket_path = Path(args.socket)
    if socket_ping(socket_path, args.timeout):
        return 0

    try:
        socket_path.unlink()
    except FileNotFoundError:
        pass

    log_path = Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("ab") as log_file:
        subprocess.Popen(
            [sys.executable, __file__, "serve", "--socket", os.fspath(socket_path)],
            stdin=subprocess.DEVNULL,
            stdout=log_file,
            stderr=log_file,
            start_new_session=True,
        )

    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        if socket_ping(socket_path, 0.2):
            return 0
        time.sleep(0.05)

    print(f"launcher did not become ready: {socket_path}", file=sys.stderr)
    return 1


def cmd_spawn_writeback(args: argparse.Namespace) -> int:
    ready_r, ready_w = os.pipe()
    with Path(args.log).open("ab") as log_file:
        child = subprocess.Popen(
            [
                sys.executable,
                __file__,
                "writeback",
                "--lock",
                args.lock,
                "--source",
                args.source,
                "--dest",
                args.dest,
                "--meta",
                args.meta,
                "--dirty",
                args.dirty,
                "--ready-fd",
                str(ready_w),
            ],
            stdin=subprocess.DEVNULL,
            stdout=log_file,
            stderr=log_file,
            start_new_session=True,
            pass_fds=(ready_w,),
        )
    os.close(ready_w)
    try:
        os.read(ready_r, 1)
    finally:
        os.close(ready_r)
    return 0 if child.pid else 1


def cmd_writeback(args: argparse.Namespace) -> int:
    ready_fd = args.ready_fd
    lock_path = Path(args.lock)
    source = Path(args.source)
    dest = Path(args.dest)
    meta = Path(args.meta)
    dirty = Path(args.dirty)
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_dest = dest.with_name(dest.name + ".tmp")
    with lock_path.open("a+b") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        if ready_fd is not None:
            os.write(ready_fd, b"1")
            os.close(ready_fd)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, tmp_dest)
        os.replace(tmp_dest, dest)
        stat = dest.stat()
        meta.write_text(f"{stat.st_size}:{int(stat.st_mtime)}", encoding="utf-8")
        try:
            dirty.unlink()
        except FileNotFoundError:
            pass
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    serve = subparsers.add_parser("serve")
    serve.add_argument("--socket", required=True)
    serve.set_defaults(func=cmd_serve)

    ensure = subparsers.add_parser("ensure")
    ensure.add_argument("--socket", required=True)
    ensure.add_argument("--log", required=True)
    ensure.add_argument("--timeout", type=float, default=5.0)
    ensure.set_defaults(func=cmd_ensure)

    writeback = subparsers.add_parser("writeback")
    writeback.add_argument("--lock", required=True)
    writeback.add_argument("--source", required=True)
    writeback.add_argument("--dest", required=True)
    writeback.add_argument("--meta", required=True)
    writeback.add_argument("--dirty", required=True)
    writeback.add_argument("--ready-fd", type=int)
    writeback.set_defaults(func=cmd_writeback)

    spawn_writeback = subparsers.add_parser("spawn-writeback")
    spawn_writeback.add_argument("--lock", required=True)
    spawn_writeback.add_argument("--source", required=True)
    spawn_writeback.add_argument("--dest", required=True)
    spawn_writeback.add_argument("--meta", required=True)
    spawn_writeback.add_argument("--dirty", required=True)
    spawn_writeback.add_argument("--log", required=True)
    spawn_writeback.set_defaults(func=cmd_spawn_writeback)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
