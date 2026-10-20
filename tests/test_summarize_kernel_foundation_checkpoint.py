import json
from pathlib import Path
import unittest
from unittest.mock import patch

import summarize_kernel_foundation_checkpoint as report


class FullRunValidation(unittest.TestCase):
    def setUp(self):
        self.result = dict(ok=True, desktop=True, workload_complete=True, guest_fault=False,
                           kernel_sha256='expected', renderer='virgl (D3D12 (Intel(R) Graphics))')
        self.console = ''.join(f'STAGE0_FS_TRIAL={trial}\nFS_BENCH_DONE fs=ext4\nFS_BENCH_DONE fs=tmpfs\n'
                               for trial in range(1,4))
        for trial in range(1,9):
            self.console += f'STAGE0_SIMD_TRIAL={trial}\n'
            for worker in range(4):
                self.console += f'SIMD_STOP_WORKER id={worker} iters=10 bad=0\n'
                self.console += f'SIMD_STOP_CHILD id={worker} pid={100+worker} status=0\n'
            self.console += f'SIMD_STOP_RC=0\nSTAGE0_SIMD_OK={trial}\n'
        self.console += ''.join(marker+'=OK\n' for marker in report.REQUIRED_SIGNAL_MARKERS)
        self.console += 'STAGE0_WORKLOAD_DONE\n'

    def validate(self):
        def read(path, *args, **kwargs):
            return json.dumps(self.result) if path.name == 'result.json' else self.console
        with patch.object(Path, 'read_text', read), patch.object(report, 'digest', return_value='fixture'):
            return report.validate_run(Path('/virtual/run'), 'expected')['strict_ok']

    def test_complete_run(self):
        self.assertTrue(self.validate())

    def test_bad_artifact_or_renderer_or_trace(self):
        for field, value in (('kernel_sha256','other'), ('renderer','fake D3D12'),
                             ('renderer','virgl (D3D12 (softpipe))'), ('debugger_trace','breakpoints'),
                             ('ok',False), ('guest_fault',True)):
            with self.subTest(field=field):
                original = dict(self.result)
                self.result[field] = value
                self.assertFalse(self.validate())
                self.result = original

    def test_worker_and_child_identity_not_just_total(self):
        for source in ('WORKER id=3', 'CHILD id=3'):
            original = self.console
            self.console = self.console.replace(source, source.replace('3','2'), 1)
            self.assertFalse(self.validate())
            self.console = original

    def test_every_signal_and_completion_required(self):
        for marker in (*report.REQUIRED_SIGNAL_MARKERS, 'STAGE0_WORKLOAD_DONE'):
            original = self.console
            self.console = self.console.replace(marker, 'MISSING', 1)
            self.assertFalse(self.validate())
            self.console = original

    def test_fs_pair_and_simd_return_code_required(self):
        for old, new in (('fs=tmpfs','fs=ext4'), ('SIMD_STOP_RC=0','SIMD_STOP_RC=1'),
                         ('STAGE0_SIMD_OK=1','STAGE0_SIMD_OK=2'), ('iters=10','iters=0'),
                         ('bad=0','bad=1'), ('status=0','status=9')):
            original = self.console
            self.console = self.console.replace(old,new,1)
            self.assertFalse(self.validate())
            self.console = original


if __name__ == '__main__':
    unittest.main()
