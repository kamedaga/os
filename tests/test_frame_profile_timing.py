#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Deterministic host-marker conversion; no display or guest is required."""
import contextlib
import io
from pathlib import Path
import runpy
import unittest
from unittest.mock import patch


class HostTiming(unittest.TestCase):
    def run_markers(self, lines, stamps):
        process = type("Process", (), {"stdout": lines, "wait": lambda self: 0})()
        output = io.StringIO()
        script = Path(__file__).with_name("time-frame-profile.py")
        with patch("sys.argv", [str(script), "unused"]), \
             patch("subprocess.Popen", return_value=process), \
             patch("time.monotonic_ns", side_effect=stamps), \
             contextlib.redirect_stdout(output):
            with self.assertRaises(SystemExit) as result:
                runpy.run_path(str(script), run_name="__main__")
            self.assertEqual(result.exception.code, 0)
        return output.getvalue()

    def test_frame_conversion_uses_host_time(self):
        output = self.run_markers([
            "FRAME_BEGIN mode=pipeline mono_us=100 tsc=0\n",
            "FRAME_END completed=240 elapsed_us=2000000 elapsed_cycles=20000000000\n",
            "FRAME_STAGE mode=pipeline phase=swap unit=tsc_cycles n=240 "
            "mean=2000000 p50=1000000 p95=3000000 p99=4000000 max=5000000\n",
        ], [0, 10_000_000_000, 11_000_000_000])
        self.assertIn("fps=24.000 guest_clock_ratio=0.200000", output)
        self.assertIn("mean=1000.000 p50=500.000 p95=1500.000", output)

    def test_sleep_control_has_no_frame_fps(self):
        output = self.run_markers([
            "CLOCK_BEGIN sample=0 mono_us=0 tsc=0\n",
            "CLOCK_END sample=0 elapsed_us=2000000 elapsed_cycles=8000000000\n",
        ], [0, 4_000_000_000])
        self.assertIn("guest_clock_ratio=0.500000 tsc_hz_estimate=2000000000", output)
        self.assertNotIn("fps=", output)

    def test_poll_and_ioctl_costs_use_same_run_calibration(self):
        output = self.run_markers([
            "FRAME_BEGIN mode=queued mono_us=0 tsc=0\n",
            "FRAME_END completed=240 elapsed_us=2000000 elapsed_cycles=20000000000\n",
            "FRAME_POLL mode=queued phase=swap fd=3 x11=1 nfds=1 timeout_ms=-1 "
            "events=0x1 revents=0x1 calls=10 cycles=20000000 max=6000000 "
            "ready=10 timeouts=0 errors=0\n",
            "FRAME_RPC mode=queued phase=swap operation=0xc02864c3 "
            "calls=4 cycles=8000000 max=4000000 errors=0\n",
        ], [0, 10_000_000_000, 11_000_000_000, 12_000_000_000])
        self.assertIn("FRAME_HOST_POLL mode=queued phase=swap fd=3 x11=1", output)
        self.assertIn("total=10000.000 mean=1000.000 max=3000.000", output)
        self.assertIn("FRAME_HOST_RPC mode=queued phase=swap operation=0xc02864c3", output)
        self.assertIn("errors=0 calls=4 unit=estimated_wall_us", output)
        self.assertIn("total=4000.000 mean=1000.000 max=2000.000", output)

    def test_unpaired_markers_do_not_claim_measurement(self):
        output = self.run_markers([
            "FRAME_END completed=1 elapsed_us=1 elapsed_cycles=1\n",
        ], [0])
        self.assertNotIn("FRAME_HOST", output)


if __name__ == "__main__":
    unittest.main()
