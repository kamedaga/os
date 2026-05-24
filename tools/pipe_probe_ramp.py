#!/usr/bin/env python3
import argparse
import csv
import subprocess
import sys
from pathlib import Path

import apk_update_bench


def parse_levels(text: str) -> list[int]:
    levels: list[int] = []
    for item in text.split(","):
        item = item.strip()
        if not item:
            continue
        value = int(item)
        if value <= 0:
            raise ValueError("levels must be positive")
        levels.append(value)
    if not levels:
        raise ValueError("at least one level is required")
    return levels


def main() -> int:
    parser = argparse.ArgumentParser(description="Ramp pipe_probe loops until failure or a configured cap.")
    parser.add_argument("--label", required=True, help="label used under .artifacts/pipe-probe-ramp-<label>")
    parser.add_argument("--levels", default="32,128,512,2048,8192")
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--window", type=int, default=8)
    parser.add_argument("--keep-going", action="store_true", help="continue after a failed level")
    args = parser.parse_args()

    root = Path.cwd()
    out_root = root / ".artifacts" / f"pipe-probe-ramp-{args.label}"
    out_root.mkdir(parents=True, exist_ok=True)
    levels = parse_levels(args.levels)

    rows: list[dict[str, object]] = []
    for loops in levels:
        run_name = f"loops{loops}"
        run_out = out_root / run_name
        command = (
            "env CAPABILITYOS_EXEC_PROFILE=1 CAPABILITYOS_EXEC_PROFILE_DETAIL=1 "
            f"PIPE_PROBE_LOOPS={loops} PIPE_PROBE_WINDOW={args.window} pipe_probe"
        )
        cmd = [
            sys.executable,
            "tools/apk_update_smoke.py",
            "--out",
            str(run_out.relative_to(root)),
            "--timeout",
            str(args.timeout),
            "--command",
            command,
        ]
        print(f"{run_name}: {' '.join(cmd)}", flush=True)
        proc = subprocess.run(cmd, cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        run_out.mkdir(parents=True, exist_ok=True)
        run_out.joinpath("driver.log").write_text(proc.stdout, encoding="utf-8", errors="replace")

        row: dict[str, object] = {"loops": loops, "out": str(run_out.relative_to(root)), "returncode": proc.returncode}
        row.update(apk_update_bench.parse_stdout(proc.stdout))
        serial_path = run_out / "serial.log"
        if serial_path.exists():
            row.update(apk_update_bench.parse_serial(serial_path.read_text(encoding="utf-8", errors="replace")))
        if proc.returncode != 0:
            row["result"] = "fail"
        rows.append(row)
        print(f"{run_name}: {row.get('result', 'fail')} command_s={row.get('command_s', 'n/a')}", flush=True)
        if proc.returncode != 0 and not args.keep_going:
            break

    fieldnames = ["loops", "out", "result", "returncode", "boot_wait_s", "command_s"] + apk_update_bench.COUNTER_KEYS
    with out_root.joinpath("runs.tsv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, delimiter="\t", extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)

    lines = ["# pipe_probe ramp", ""]
    lines.append(apk_update_bench.markdown_table(
        ["Loops", "Result", "command_s", "create", "read_calls", "write_calls", "write_again", "broken", "epoll_ready"],
        [
            [
                str(row["loops"]),
                str(row.get("result", "fail")),
                f"{float(row['command_s']):.3f}" if isinstance(row.get("command_s"), float) else "n/a",
                str(row.get("LinuxAbiServer.perf.pipe.create_calls", "")),
                str(row.get("LinuxAbiServer.perf.pipe.read_calls", "")),
                str(row.get("LinuxAbiServer.perf.pipe.write_calls", "")),
                str(row.get("LinuxAbiServer.perf.pipe.write_again", "")),
                str(row.get("LinuxAbiServer.perf.pipe.write_broken", "")),
                str(row.get("LinuxAbiServer.perf.pipe.epoll_ready", "")),
            ]
            for row in rows
        ],
    ))
    lines.append("")
    out_root.joinpath("summary.md").write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {out_root / 'runs.tsv'}")
    print(f"wrote {out_root / 'summary.md'}")
    return 1 if any(row.get("result") != "pass" for row in rows) else 0


if __name__ == "__main__":
    raise SystemExit(main())
