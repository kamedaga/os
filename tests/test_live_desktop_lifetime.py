#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise actual host teardown policy without touching SSH or a GPU."""

from pathlib import Path
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]


class DesktopLifetime(unittest.TestCase):
    def test_desktop_feedback_gaps_retry_without_stop(self):
        source = (REPO / "pack/scripts/run_amdgpu_inspection.sh").read_text()
        handler = source[source.index("feedback_gap() {"):source.index("cleanup() {")]
        script = 'set -eu\nsmoke_kind=desktop\nfeedback_warning=0\nemergency_stop=0\n' + handler + '''
feedback_gap 'SSH log read failed'
feedback_gap 'temperature log sample delayed'
test "$emergency_stop" = 0
test "$feedback_warning" = 1
'''
        result = subprocess.run(["bash", "-c", script], capture_output=True,
                                text=True, check=True)
        self.assertEqual(result.stderr.count("no stop or poweroff requested"), 1)
        self.assertNotIn("ending finite test", result.stderr)
        self.assertIn("if feedback_gap 'SSH log read failed'; then", source)
        self.assertIn("if ! feedback_gap 'temperature log sample delayed'; then", source)

    def test_finite_test_feedback_failure_remains_an_error(self):
        source = (REPO / "pack/scripts/run_amdgpu_inspection.sh").read_text()
        handler = source[source.index("feedback_gap() {"):source.index("cleanup() {")]
        result = subprocess.run(["bash", "-c", 'smoke_kind=gears\nfeedback_warning=0\n'
                                 + handler + "\nfeedback_gap 'SSH log read failed'\n"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("ending finite test", result.stderr)

    def cleanup_commands(self, emergency: bool, kind: str = "desktop",
                         identity_changed: bool = False) -> str:
        source = (REPO / "pack/scripts/run_amdgpu_inspection.sh").read_text()
        cleanup = source[source.index("cleanup() {"):source.index("trap cleanup EXIT")]
        with tempfile.TemporaryDirectory(prefix="desktop-lifetime-") as directory:
            # Use the production function, substituting only command execution:
            # tests must never send a stop or poweroff to a physical machine.
            script = r'''
set -eu
session_dir="$1"
smoke_kind="$2"
emergency_stop="$3"
host_identity_changed="$4"
probe_sent=1
smoke_pid=0
target=mock
ssh_options=()
record_service_lines() { :; }
timeout() {
    printf '%s\n' "$*" >> "$session_dir/commands"
    case "$*" in
        *"tail -n 100"*) echo 'PCI DRM probe stopped status=0';;
    esac
}
''' + cleanup + "\ncleanup\n"
            result = subprocess.run(
                ["bash", "-c", script, "test", directory, kind, str(int(emergency)),
                 str(int(identity_changed))],
                capture_output=True, text=True, check=True,
            )
            commands_path = Path(directory) / "commands"
            commands = commands_path.read_text() if commands_path.exists() else ""
            return commands + result.stderr

    def test_normal_desktop_exit_preserves_gpu_and_session(self):
        output = self.cleanup_commands(False)
        self.assertNotIn("gpu-control", output)
        self.assertNotIn("poweroff -f", output)
        self.assertNotIn("kill -TERM", output)
        self.assertIn("no stop or poweroff requested", output)

    def test_changed_host_key_retires_monitor_without_guest_cleanup(self):
        for kind in ("desktop", "gears"):
            output = self.cleanup_commands(True, kind, identity_changed=True)
            self.assertNotIn("gpu-control", output)
            self.assertNotIn("poweroff -f", output)
            self.assertNotIn("cat /run/pacha/", output)
            self.assertIn("no guest cleanup requested", output)

    def test_changed_host_key_is_not_retried(self):
        source = (REPO / "pack/scripts/run_amdgpu_inspection.sh").read_text()
        handler = source[source.index("check_host_identity() {"):source.index("feedback_gap() {")]
        with tempfile.TemporaryDirectory(prefix="desktop-identity-") as directory:
            log = Path(directory) / "monitor-ssh-last.log"
            for message, changed in (("Connection timed out", False),
                                     ("WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!", True),
                                     ("Host key verification failed.", True)):
                log.write_text(message)
                script = 'set -eu\nhost_identity_changed=0\nsession_dir="$1"\n' + handler + '''
if check_host_identity; then test "$host_identity_changed" = 0
else test "$host_identity_changed" = 1; exit 42; fi
'''
                result = subprocess.run(["bash", "-c", script, "test", directory],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 42 if changed else 0)
        self.assertIn("if ! check_host_identity; then exit 1; fi", source)

    def test_safety_failure_still_stops_and_powers_off(self):
        output = self.cleanup_commands(True)
        self.assertIn("gpu-control", output)
        self.assertIn("poweroff -f", output)
        self.assertIn("kill -TERM", output)

    def test_finite_smoke_retains_owned_teardown(self):
        output = self.cleanup_commands(False, "gears")
        self.assertIn("gpu-control", output)
        self.assertIn("poweroff -f", output)

    def test_guest_launcher_waits_for_user_session(self):
        source = (REPO / "pack/scripts/run_live_desktop_session.sh").read_text()
        self.assertIn('wait "$xinit_pid"', source)
        self.assertNotIn("OBSERVATION_COMPLETE", source)
        self.assertNotIn('"$count" -lt 30', source)

    def test_host_desktop_has_no_job_deadline(self):
        source = (REPO / "pack/scripts/run_amdgpu_inspection.sh").read_text()
        self.assertIn('[[ "$smoke_kind" != desktop ]] && (( $(date +%s) - started_at >= 150 ))', source)
        self.assertIn("busybox setsid busybox sh $guest_smoke", source)
        self.assertNotIn("timeout 95s", source)

    def test_guest_thermal_monitor_has_no_elapsed_time_abort(self):
        source = (REPO / "kobox2/linux-sandbox/kobox/boot/device_launch.c").read_text()
        self.assertIn("temperature >= KOBOX_INSPECT_TEMP_LIMIT_MC", source)
        self.assertNotIn("KOBOX_INSPECT_MAX_NS", source)
        self.assertNotIn("return -ETIME;", source)


if __name__ == "__main__":
    unittest.main()
