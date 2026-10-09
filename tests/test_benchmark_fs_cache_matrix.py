#!/usr/bin/env python3
import unittest

from benchmark_fs_cache_matrix import PHASES, collect, collect_stages, collect_recycle, verify_checkpoint, check_storage_config


def fixture():
    return ''.join(
        f'[ 0.0] FS_CACHE_MATRIX fs={fs} phase={phase} cache={cache} trial={trial} '
        f'ops={12 if cache == "cold" else 24 if phase == "pwrite_fsync" else 1024} ns=1024000\r\n'
        for fs in ('ext4', 'tmpfs') for phase in PHASES
        for cache in (('warm', 'cold') if fs == 'ext4' else ('warm',))
        for trial in range(1, 4))


class MatrixCollectionTest(unittest.TestCase):
    def test_config_mismatch_requires_explicit_historical_label(self):
        profile = '# CONFIG_SLUB_TINY is not set\nCONFIG_EXT4_FS=m\n'
        old = 'CONFIG_SLUB_TINY=y\nCONFIG_EXT4_FS=m\n'
        with self.assertRaises(RuntimeError):
            check_storage_config(old, profile)
        self.assertEqual(check_storage_config(old, profile, True),
                         {'CONFIG_SLUB_TINY': {'actual': 'y', 'expected': 'n'}})
        self.assertEqual(check_storage_config(profile, profile), {})

    def recycle_fixture(self):
        return ''.join(
            f'FS_RECYCLE_CONTROL fs={r["fs"]} phase={r["phase"]} cache={r["cache"]} '
            f'trial={r["trial"]} scrub={scrub} ops={r["ops"] // 2} ns={r["ns"] // 2} max_ns=90000 '
            f'tail_ops=0 tail_ns=0 io_probed={int(r["fs"] == "ext4" and r["cache"] == "warm" and r["phase"] in ("create_close", "rename", "unlink", "mkdir_rmdir", "pwrite_fsync"))} '
            'reads=0 writes=0 switches=0 tail_reads=0 tail_writes=0 tail_switches=0\r\n'
            for r in collect(fixture()) for scrub in (0, 1))

    def test_recycle_complete_balanced_and_conserved(self):
        rows = collect_recycle(self.recycle_fixture(), collect(fixture()))['rows']
        self.assertEqual(len(rows), 252)
        self.assertEqual({r['scrub'] for r in rows}, {False, True})

    def test_recycle_tail_kept_in_full_total(self):
        text = self.recycle_fixture().replace('max_ns=90000 tail_ops=0 tail_ns=0',
                                              'max_ns=100000 tail_ops=1 tail_ns=100000', 1)
        row = collect_recycle(text, collect(fixture()))['rows'][0]
        self.assertEqual(row['ns'], 512000)
        self.assertEqual(row['tail_ns'], 100000)
        self.assertEqual(row['tail_ops'], 1)

    def test_recycle_missing_duplicate_counts_and_totals_rejected(self):
        text = self.recycle_fixture()
        lines = text.splitlines(keepends=True)
        for invalid in (''.join(lines[1:]), text + lines[0],
                        text.replace('ops=512', 'ops=511', 1),
                        text.replace('ns=512000', 'ns=512001', 1),
                        text.replace('max_ns=90000', 'max_ns=0', 1),
                        text.replace('max_ns=90000', 'max_ns=999999', 1),
                        text.replace('tail_ops=0', 'tail_ops=1', 1),
                        text.replace('io_probed=0', 'io_probed=1', 1),
                        text.replace('tail_reads=0', 'tail_reads=1', 1)):
            with self.assertRaises(RuntimeError):
                collect_recycle(invalid, collect(fixture()))

    def stage_fixture(self):
        multipliers = {'open_close_d0': 2, 'open_close_d8': 2, 'create_close': 2,
                       'mkdir_rmdir': 2, 'readdir_256': 3, 'pwrite_fsync': 2}
        return 'FS_CACHE_STAGE_CLOCK ticks=2000000 ns=1000000\r\n' + ''.join(
            f'FS_CACHE_STAGES fs={r["fs"]} phase={r["phase"]} cache={r["cache"]} '
            f'trial={r["trial"]} ops={r["ops"]} calls={r["ops"] * multipliers.get(r["phase"], 1)} '
            'encode=100 submit=200 wait=300 inspect=400 recycle=500\r\n'
            for r in collect(fixture()))

    def test_stage_counts_and_tick_conversion(self):
        stages = collect_stages(self.stage_fixture(), collect(fixture()))
        self.assertEqual(len(stages['rows']), 126)
        row = stages['rows'][0]
        self.assertEqual(row['stages_ns_per_op']['encode'], 50 / row['ops'])

    def test_missing_duplicate_and_bad_stage_counts(self):
        text = self.stage_fixture()
        lines = text.splitlines(keepends=True)
        for invalid in (''.join(lines[1:]), ''.join(lines[:-1]), text + lines[1],
                        text + lines[0], text.replace('calls=1024', 'calls=1023', 1),
                        text.replace('encode=100', 'encode=0', 1)):
            with self.assertRaises(RuntimeError):
                collect_stages(invalid, collect(fixture()))

    def test_checkpoint_policy_and_count(self):
        text = ('FS_CACHE_POLICY cold=checkpoint_then_drop drop_passes=4\r\n'
                'FS_CACHE_CHECKPOINT prepared=504\r\n')
        self.assertEqual(verify_checkpoint(text, 504), 504)
        for invalid in ('', text + text, text.replace('504', '503'),
                        text.replace('checkpoint_then_drop', 'drop_only')):
            with self.assertRaises(RuntimeError):
                verify_checkpoint(invalid, 504)

    def test_complete_matrix(self):
        self.assertEqual(len(collect(fixture())), 126)

    def test_missing_and_duplicate_are_not_results(self):
        lines = fixture().splitlines(keepends=True)
        for text in (''.join(lines[1:]), fixture() + lines[0]):
            with self.assertRaises(RuntimeError):
                collect(text)

    def test_unknown_phase_and_invalid_counts_are_not_results(self):
        for text in (fixture().replace('stat_d0', 'unknown', 1),
                     fixture().replace('fs=ext4', 'fs=unknown', 1),
                     fixture().replace('cache=warm', 'cache=unknown', 1),
                     fixture().replace('ops=1024', 'ops=0', 1),
                     fixture().replace('ns=1024000', 'ns=0', 1)):
            with self.assertRaises(RuntimeError):
                collect(text)


if __name__ == '__main__':
    unittest.main()
