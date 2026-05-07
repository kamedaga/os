#!/usr/bin/env python3
import argparse
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
    ("seed2_boot started", (r"\[seed2_boot\] started",)),
    ("seed2_boot bootfs ready", (r"\[seed2_boot\] bootfs ready",)),
    ("VirtioBlk started", (r"VirtioBlk: started",)),
    ("VirtioBlk queue ready", (r"VirtioBlk: queue ready",)),
    ("VirtioBlk connect request", (r"VirtioBlk: connect request",)),
    ("fat_server endpoint ready", (r"FatServer: endpoint ready",)),
    ("RootVfs endpoint ready", (r"RootVfs: endpoint ready",)),
    ("ExecService endpoint ready", (r"ExecService: endpoint ready",)),
    ("LinuxAbiServer started", (r"LinuxAbiServer: started",)),
    ("DashShim dash spawned", (r"DashShim: dash spawned", r"ExecService: spawn ok")),
]

REQUIRED_MILESTONES = {
    "ExecService endpoint ready",
    "LinuxAbiServer started",
    "DashShim dash spawned",
}


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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", nargs="?", default=".artifacts/serial-timed.log")
    parser.add_argument(
        "--check",
        action="store_true",
        help="return non-zero when the interactive boot milestones are incomplete",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    path = Path(args.path)
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
    ordered_found = sorted(
        ((elapsed_ms, label, matched_text) for label, (elapsed_ms, matched_text) in found.items()),
        key=lambda item: item[0],
    )
    for elapsed_ms, label, matched_text in ordered_found:
        delta_ms = 0.0 if previous is None else elapsed_ms - previous
        print(f"{elapsed_ms:9.3f} ms  (+{delta_ms:8.3f} ms)  {label}")
        print(f"  {matched_text}")
        previous = elapsed_ms

    missing = [label for label, _patterns in MILESTONES if label not in found]
    if missing:
        print("missing milestones:")
        for label in missing:
            print(f"  {label}")

    missing_required = [
        label
        for label, _patterns in MILESTONES
        if label in REQUIRED_MILESTONES and label not in found
    ]
    if missing_required:
        print("missing required interactive boot milestones:")
        for label in missing_required:
            print(f"  {label}")
    else:
        print("interactive boot complete")

    end = found.get("DashShim dash spawned")
    start = found.get("BdsDxe loading")
    if start and end:
        total_ms = end[0] - start[0]
        print(f"total to interactive shell spawned: {total_ms:.3f} ms")

    if args.check and missing_required:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
