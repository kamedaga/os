#!/usr/bin/env python3
import re
import sys
from pathlib import Path


TIMED_LINE_RE = re.compile(r"^\[\+\s*([0-9]+(?:\.[0-9]+)?) ms\] (.*)$")
ANSI_RE = re.compile(r"\x1b(?:\[[0-9;?]*[ -/]*[@-~]|[@-_])")

MILESTONES = [
    ("BdsDxe loading", (r"BdsDxe: loading Boot0001",)),
    ("BdsDxe starting", (r"BdsDxe: starting Boot0001",)),
    ("RAW ENTER MAIN", (r"^RAW ENTER MAIN$",)),
    ("ENTER MAIN", (r"^ENTER MAIN$",)),
    ("collectMemoryStats begin", (r"\[stage\] collectMemoryStats begin",)),
    ("collectMemoryStats got map", (r"\[stage\] collectMemoryStats got map",)),
    ("USER_PAGE_READY", (r"^USER_PAGE_READY$",)),
    ("boot manifest ok", (r"boot manifest ok",)),
    ("rootfs ready", (r"rootfs ready",)),
    ("seed exec ready", (r"seed exec ready",)),
    ("manager bootstrap table ready", (r"manager bootstrap table ready",)),
    ("spawn_exec seed", (r"spawn_exec ready child=1 thread=1",)),
    ("seed spawn returned", (r"seed spawn returned",)),
    ("manager grants begin", (r"manager grants begin",)),
    ("manager grants ready", (r"manager grants ready",)),
    ("seed slot", (r"seed slot=1",)),
    ("rootfs reader ready", (r"rootfs reader ready",)),
    ("startup manifest ready", (r"rootfs startup manifest ready",)),
    ("open_exec shell ok", (r"open_exec shell ok",)),
    ("open_exec block_driver ok", (r"open_exec block_driver ok",)),
    ("open_exec persistent_fs ok", (r"open_exec persistent_fs ok",)),
    ("spawning shell", (r"spawning shell from rootfs",)),
    ("Shell started", (r"Shell: started",)),
    ("shell spawned", (r"shell spawned",)),
    ("Shell keyboard ready", (r"Shell: keyboard ready",)),
    ("VirtioBlk started", (r"VirtioBlk: started",)),
    ("spawn block_driver ok", (r"spawn block_driver ok",)),
    ("spawn persistent_fs ok", (r"spawn persistent_fs ok",)),
    ("PersistentFs started", (r"PersistentFs: started",)),
    ("startup manifest done", (r"startup manifest done",)),
    ("share_cap begin", (r"share_cap begin from=spawned exec to=spawned exec ep=0x82",)),
    ("share_cap done", (r"share_cap done from=spawned exec to=spawned exec ep=0x82",)),
    ("Shell ui ready", (r"Shell: ui ready",)),
    ("VirtioBlk queue ready", (r"VirtioBlk: queue ready",)),
    ("VirtioBlk connect request", (r"VirtioBlk: connect request",)),
    ("PersistentFs block ready", (r"PersistentFs: block ready",)),
    ("PersistentFs endpoint ready", (r"PersistentFs: endpoint ready",)),
]


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def parse_timed_lines(path: Path):
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = TIMED_LINE_RE.match(raw_line)
        if not match:
            continue
        elapsed_ms = float(match.group(1))
        text = strip_ansi(match.group(2))
        yield elapsed_ms, text


def main() -> int:
    if len(sys.argv) > 2:
        print(f"usage: {Path(sys.argv[0]).name} [timed-log-path]", file=sys.stderr)
        return 1

    path = Path(sys.argv[1]) if len(sys.argv) == 2 else Path(".artifacts/serial-timed.log")
    if not path.is_file():
        print(f"missing timed log: {path}", file=sys.stderr)
        return 1

    found = {}
    for elapsed_ms, text in parse_timed_lines(path):
        for label, patterns in MILESTONES:
            if label in found:
                continue
            if any(re.search(pattern, text) for pattern in patterns):
                found[label] = (elapsed_ms, text)

    print(f"Boot timing summary from {path}")

    previous = None
    for label, _patterns in MILESTONES:
        record = found.get(label)
        if record is None:
            continue
        elapsed_ms, matched_text = record
        delta_ms = 0.0 if previous is None else elapsed_ms - previous
        print(f"{elapsed_ms:9.3f} ms  (+{delta_ms:8.3f} ms)  {label}")
        print(f"  {matched_text}")
        previous = elapsed_ms

    missing = [label for label, _patterns in MILESTONES if label not in found]
    if missing:
        print("missing milestones:")
        for label in missing:
            print(f"  {label}")

    end = found.get("PersistentFs endpoint ready")
    start = found.get("BdsDxe loading")
    if start and end:
        total_ms = end[0] - start[0]
        print(f"total to PersistentFs endpoint ready: {total_ms:.3f} ms")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
