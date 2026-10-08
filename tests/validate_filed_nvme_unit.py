#!/usr/bin/env python3
import unittest
import io
import re
import tempfile
from pathlib import Path
from types import SimpleNamespace

from validate_filed_nvme import (PHASES, digest, line_marker, summarize,
                                 validate_clocks, validate_producers, validate_stress)
from qemu_filed_cutover_smoke import CutoverGuest


class ValidationTest(unittest.TestCase):
    def test_cycle_summary_uses_each_filesystem_trial_calibration(self):
        data = {fs: {phase: [{'total_ns': 1000000000, 'ops': 1000000,
                             'ns_per_op': 1000}] for phase in PHASES}
                for fs in ('ext4', 'tmpfs')}
        old = {fs: {phase: {'pacha_median_ns': 2000} for phase in PHASES}
               for fs in data}
        summary = summarize(data, data, old,
                            {'pacha': [2e9, 3e9], 'linux': [1e9, 1.5e9]})
        for fs, cycles in (('ext4', 2000), ('tmpfs', 3000)):
            row = summary[fs]['stat_d0']
            self.assertEqual(row['pacha_median_tsc_cycles'], cycles)
            self.assertEqual(row['tsc_cycles_ratio'], 2)

    def test_current_producers_must_match_all_recorded_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory) / 'producer'
            artifact.write_bytes(b'normal producer')
            entry = {'source': str(artifact), 'sha256': digest(artifact)}
            paths = ('/lib/pacha/lpr-linux-x86_64.so', '/sbin/filed.elf',
                     '/srv/kobox2/storage/core.so', '/srv/kobox2/storage/manifest.bin',
                     '/srv/kobox2/storage/sandbox.elf')
            base = {'kernel': entry, 'init_app': entry, 'installs': [],
                    'bootfs_inputs': [dict(entry, path=p) for p in paths]}
            validate_producers(base)
            missing = dict(base, bootfs_inputs=base['bootfs_inputs'][:-1])
            with self.assertRaises(RuntimeError):
                validate_producers(missing)
            artifact.write_bytes(b'changed producer')
            with self.assertRaises(RuntimeError):
                validate_producers(base)

    def test_prompt_is_not_split_by_unrelated_serial_output(self):
        guest = object.__new__(CutoverGuest)
        serial, console = object(), object()
        guest.process = SimpleNamespace(stdout=serial)
        guest.log = io.BytesIO()
        guest.text = bytearray()
        guest.record_chunk(serial, b'Linux # SMP PREEMPT\r\n')
        guest.record_chunk(console, b'\x1b[?2004hbash-')
        guest.record_chunk(serial, b'[unix] diagnostic\r\n')
        guest.record_chunk(console, b'5.2# ')
        guest.record_chunk(serial, b'[unix] another diagnostic\r\n')
        prompt = re.compile(rb'(?:^|\n)[^\r\n]*bash-\d+\.\d+# (?=\r?\n|$)')
        self.assertIsNone(prompt.search(guest.text))
        self.assertIsNotNone(prompt.search(guest.console_text))
        self.assertEqual(guest.text, guest.log.getvalue())

    def test_marker_must_be_an_emitted_line_not_echoed_command(self):
        marker = line_marker('DONE')
        self.assertIsNone(marker.search(b'bash# echo DONE\r\n'))
        self.assertIsNotNone(marker.search(b'\r\nDONE\r\n'))

    def test_clock_count_and_consistency(self):
        complete = 'tsc_hz=3686394000\n' * 6
        self.assertEqual(len(validate_clocks({'pacha': complete, 'linux': complete}, 3)['pacha']), 6)
        with self.assertRaises(RuntimeError):
            validate_clocks({'pacha': complete[:-20]}, 3)
        with self.assertRaises(RuntimeError):
            validate_clocks({'pacha': complete, 'linux': 'tsc_hz=2000000000\n' * 6}, 3)

    def test_real_stress_cycles_cannot_be_skipped_or_repeated(self):
        output = '\n'.join(('APK_VERSION_OK', 'APK_UPDATE_OK', 'CLANG_VERSION_OK',
            'CLANG_COMPILE_ONLY_OK', 'CLANG_LINK_RUN_OK', 'NVME_STRESS_SYNC_OK')) + '\n'
        for stage in ('APK_ADD_OK', 'APK_DEL_OK', 'CLANG_ITER_OK'):
            output += ''.join(f'{stage} iteration={n}\n' for n in range(1, 6))
        validate_stress(output, 5)
        with self.assertRaises(RuntimeError):
            validate_stress(output + 'ERROR: busybox.trigger: script exited with error 127\n', 5)
        with self.assertRaises(RuntimeError):
            validate_stress(output.replace('APK_DEL_OK iteration=3\n', ''), 5)
        with self.assertRaises(RuntimeError):
            validate_stress(output + 'CLANG_ITER_OK iteration=5\n', 5)


if __name__ == '__main__':
    unittest.main()
