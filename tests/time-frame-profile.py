#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Timestamp flushed SSH frame markers without trusting a guest tick clock."""
import re
import subprocess
import sys
import time

if len(sys.argv) < 2:
    sys.exit("usage: time-frame-profile.py command [arguments ...]")
process = subprocess.Popen(sys.argv[1:], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, bufsize=1)
started = None
tsc_hz = None
assert process.stdout is not None
for line in process.stdout:
    received = time.monotonic_ns()
    print(line, end="", flush=True)
    if line.startswith(("FRAME_BEGIN ", "CLOCK_BEGIN ")):
        started = received
        tsc_hz = None
    elif line.startswith("CLOCK_END ") and started is not None:
        fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
        seconds = (received - started) / 1e9
        tsc_hz = int(fields['elapsed_cycles']) / seconds
        print(f"CLOCK_HOST sample={fields['sample']} elapsed_seconds={seconds:.6f} "
              f"guest_clock_ratio={int(fields['elapsed_us']) / 1e6 / seconds:.6f} "
              f"tsc_hz_estimate={int(fields['elapsed_cycles']) / seconds:.0f} "
              "includes_ssh_marker_delivery_jitter=1", flush=True)
        started = None
    elif line.startswith("FRAME_END ") and started is not None:
        fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
        seconds = (received - started) / 1e9
        print(f"FRAME_HOST completed={fields['completed']} elapsed_seconds={seconds:.6f} "
              f"fps={int(fields['completed']) / seconds:.3f} "
              f"guest_clock_ratio={int(fields['elapsed_us']) / 1e6 / seconds:.6f} "
              "includes_ssh_marker_delivery_jitter=1", flush=True)
        started = None
        tsc_hz = int(fields['elapsed_cycles']) / seconds
    elif line.startswith("FRAME_STAGE ") and "unit=tsc_cycles " in line and tsc_hz:
        fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
        # Derive conversion from the same run's host markers, never from the
        # drifting guest clock or a processor model's advertised GHz value.
        stats = " ".join(f"{key}={int(fields[key]) * 1e6 / tsc_hz:.3f}"
                         for key in ("mean", "p50", "p95", "p99", "max"))
        print(f"FRAME_HOST_STAGE mode={fields['mode']} phase={fields['phase']} "
              f"unit=estimated_wall_us n={fields['n']} {stats} "
              "includes_descheduling_and_ssh_calibration_jitter=1", flush=True)
    elif line.startswith(("FRAME_POLL ", "FRAME_RPC ")) and tsc_hz:
        fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
        calls = int(fields['calls'])
        total_us = int(fields['cycles']) * 1e6 / tsc_hz
        maximum_us = int(fields['max']) * 1e6 / tsc_hz
        shape = (" ".join(f"{key}={fields[key]}" for key in
                 ("fd", "x11", "nfds", "timeout_ms", "events", "revents",
                  "ready", "timeouts", "errors")) if line.startswith("FRAME_POLL ")
                 else f"operation={fields['operation']} errors={fields['errors']}")
        kind = 'POLL' if line.startswith('FRAME_POLL ') else 'RPC'
        print(f"FRAME_HOST_{kind} mode={fields['mode']} phase={fields['phase']} "
              f"{shape} calls={calls} unit=estimated_wall_us total={total_us:.3f} "
              f"mean={total_us / calls if calls else 0:.3f} max={maximum_us:.3f} "
              "includes_descheduling_and_ssh_calibration_jitter=1", flush=True)
sys.exit(process.wait())
