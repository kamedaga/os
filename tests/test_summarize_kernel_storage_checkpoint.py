#!/usr/bin/env python3
"""A completed marker must not erase a failed or incomplete repeated run."""
import json
from pathlib import Path
import unittest
from unittest.mock import patch

import summarize_kernel_storage_checkpoint as report


class SignalControlValidation(unittest.TestCase):
    def setUp(self):
        self.result = dict(ok=True, workload_complete=True, desktop=True,
                           renderer='virgl (D3D12 (Intel(R) Graphics))', guest_fault=False)
        markers = tuple(marker+'=OK' for marker in report.REQUIRED_SIGNAL_MARKERS)
        self.console = ''.join(
            f'OWNERSHIP_SIGNAL_TRIAL={trial}\r\n' +
            ''.join(marker+'\r\n' for marker in markers) +
            f'OWNERSHIP_SIGNAL_OK={trial}\r\n' for trial in range(1, 21))
        self.console += 'STAGE0_WORKLOAD_DONE\r\n'

    def validate(self, exists=True, expected_kernel_sha256=None):
        def read(path, *args, **kwargs):
            return json.dumps(self.result) if path.name == 'result.json' else self.console
        with patch.object(Path, 'read_text', read), patch.object(Path, 'exists', return_value=exists), \
                patch.object(report, 'digest', return_value='in-memory-fixture'):
            return report.validate_signal_control(Path('/virtual/control'), 20, expected_kernel_sha256)

    def test_twenty_complete_suites(self):
        result = self.validate()
        self.assertTrue(result['strict_ok'])
        self.assertEqual(20, len(result['completed']))

    def test_final_marker_cannot_override_timeout(self):
        self.result['ok'] = False
        self.assertFalse(self.validate()['strict_ok'])

    def test_every_suite_must_complete(self):
        self.console = self.console.replace('ASYNC_SIGNALFD=OK\r\n', '', 1)
        self.assertFalse(self.validate()['strict_ok'])

    def test_duplicate_completion_is_not_a_missing_trial(self):
        self.console = self.console.replace('OWNERSHIP_SIGNAL_OK=19', 'OWNERSHIP_SIGNAL_OK=20')
        self.assertFalse(self.validate()['strict_ok'])

    def test_partial_run_remains_incomplete(self):
        result = self.validate(exists=False)
        self.assertEqual('running-or-incomplete', result['status'])
        self.assertFalse(result['strict_ok'])

    def test_software_renderer_rejected(self):
        self.result['renderer'] = 'virgl (LLVMPIPE)'
        self.assertFalse(self.validate()['strict_ok'])

    def test_missing_guest_completion_rejected(self):
        self.console = self.console.replace('STAGE0_WORKLOAD_DONE\r\n', '')
        self.assertFalse(self.validate()['strict_ok'])

    def test_wrong_kernel_rejected(self):
        self.result['kernel_sha256'] = 'other-artifact'
        self.assertFalse(self.validate(expected_kernel_sha256='expected-artifact')['strict_ok'])

    def test_expected_kernel_accepted(self):
        self.result['kernel_sha256'] = 'expected-artifact'
        self.assertTrue(self.validate(expected_kernel_sha256='expected-artifact')['strict_ok'])

    def test_debugger_run_not_a_baseline(self):
        self.result['debugger_trace'] = 'Hardware breakpoint trace'
        self.assertFalse(self.validate()['strict_ok'])

    def test_fake_or_software_d3d12_renderer_rejected(self):
        for renderer in ('fake D3D12', 'virgl (D3D12 (softpipe))'):
            self.result['renderer'] = renderer
            self.assertFalse(self.validate()['strict_ok'])


if __name__ == '__main__':
    unittest.main()
