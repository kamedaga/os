#!/usr/bin/env python3
import argparse
import csv
import math
import statistics
import subprocess
import sys
from pathlib import Path

import apk_add_smoke
import apk_update_bench


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    index = (len(ordered) - 1) * pct
    lower = math.floor(index)
    upper = math.ceil(index)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def write_summary(out_root: Path, rows: list[dict[str, object]], package: str, command: str) -> None:
    times = [float(row["command_s"]) for row in rows if isinstance(row.get("command_s"), float)]
    passed = sum(1 for row in rows if row.get("result") == "pass")
    lines: list[str] = [
        "# apk add bench summary",
        "",
        f"- package: `{package}`",
        f"- command: `{command}`",
        f"- runs: {len(rows)}",
        f"- pass: {passed}",
    ]
    if times:
        lines.extend(
            [
                f"- min command_s: {min(times):.3f}",
                f"- median command_s: {statistics.median(times):.3f}",
                f"- avg command_s: {statistics.mean(times):.3f}",
                f"- p90 command_s: {percentile(times, 0.90):.3f}",
                f"- p95 command_s: {percentile(times, 0.95):.3f}",
                f"- max command_s: {max(times):.3f}",
                "",
            ]
        )
    run_rows: list[list[str]] = []
    for row in rows:
        run_rows.append(
            [
                str(row["run"]),
                str(row.get("result", "fail")),
                f"{float(row['command_s']):.3f}" if isinstance(row.get("command_s"), float) else "n/a",
                str(row.get("proc.net.tcp_pending_read_defers", "")),
                str(row.get("proc.net.tcp_read_would_block_nowait", "")),
                str(row.get("proc.net.tcp_read_bulk_would_block_nowait", "")),
                str(row.get("proc.net.net_service_active_poll_misses", "")),
            ]
        )
    lines.append(apk_update_bench.markdown_table(
        [
            "Run",
            "result",
            "command_s",
            "pending defers",
            "read nowait",
            "bulk nowait",
            "active poll misses",
        ],
        run_rows,
    ))
    lines.append("")

    failure_rows: list[list[str]] = []
    for row in rows:
        if row.get("result") == "pass":
            continue
        driver = out_root / f"run{int(row['run']):02d}" / "driver.log"
        detail = "driver.log missing"
        if driver.exists():
            text = driver.read_text(encoding="utf-8", errors="replace")
            for needle in (
                "ERROR:",
                "I/O error",
                "not found",
                "BAD signature",
                "temporary error",
                "unavailable",
                "TimeoutError",
                "Traceback",
            ):
                index = text.find(needle)
                if index >= 0:
                    detail = " ".join(text[index:index + 240].split())
                    break
        failure_rows.append([str(row["run"]), detail])
    if failure_rows:
        lines.extend(["", "## Failures", ""])
        lines.append(apk_update_bench.markdown_table(["Run", "First matched detail"], failure_rows))
        lines.append("")

    out_root.joinpath("summary.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run repeated CapabilityOS apk add measurements.")
    parser.add_argument("--label", required=True, help="label used under .artifacts/apk-add-bench-<label>")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--package", default="zlib")
    parser.add_argument("--status", action="store_true", help="append /proc/net/capabilityos after apk add")
    parser.add_argument("--command", default=None, help="override command run inside CapabilityOS")
    args = parser.parse_args()

    root = Path.cwd()
    out_root = root / ".artifacts" / f"apk-add-bench-{args.label}"
    out_root.mkdir(parents=True, exist_ok=True)

    command = args.command
    if command is None:
        command = apk_add_smoke.default_command(args.package)
    if args.status:
        command = f"{command}; cat /proc/net/capabilityos"

    rows: list[dict[str, object]] = []
    for run in range(1, args.runs + 1):
        run_name = f"run{run:02d}"
        run_out = out_root / run_name
        cmd = [
            sys.executable,
            "tools/apk_add_smoke.py",
            "--out",
            str(run_out.relative_to(root)),
            "--timeout",
            str(args.timeout),
            "--package",
            args.package,
            "--command",
            command,
        ]
        print(f"{run_name}: {' '.join(cmd)}", flush=True)
        proc = subprocess.run(cmd, cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        run_out.mkdir(parents=True, exist_ok=True)
        run_out.joinpath("driver.log").write_text(proc.stdout, encoding="utf-8", errors="replace")
        row: dict[str, object] = {"run": run, "out": str(run_out.relative_to(root)), "returncode": proc.returncode}
        row.update(apk_update_bench.parse_stdout(proc.stdout))
        serial_path = run_out / "serial.log"
        if serial_path.exists():
            row.update(apk_update_bench.parse_serial(serial_path.read_text(encoding="utf-8", errors="replace")))
        if proc.returncode != 0:
            row["result"] = "fail"
        rows.append(row)
        command_s = row.get("command_s")
        result = row.get("result", "fail")
        print(f"{run_name}: {result} command_s={command_s if command_s is not None else 'n/a'}", flush=True)

    fieldnames = ["run", "out", "result", "returncode", "boot_wait_s", "command_s"] + apk_update_bench.COUNTER_KEYS
    with out_root.joinpath("runs.tsv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, delimiter="\t", extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)
    write_summary(out_root, rows, args.package, command)
    print(f"wrote {out_root / 'runs.tsv'}")
    print(f"wrote {out_root / 'summary.md'}")
    return 1 if any(row.get("result") != "pass" for row in rows) else 0


if __name__ == "__main__":
    raise SystemExit(main())
