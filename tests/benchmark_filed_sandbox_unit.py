#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import itertools
import unittest

from benchmark_filed_sandbox import (check_runtime_flags, parse, parse_guards, parse_handoff,
                                     parse_publication, summarize, summarize_publication)


class BenchTest(unittest.TestCase):
    def test_matched_all_task_counts_are_complete(self):
        marker = 'FS_MATCHED_SWITCHES all=2010\n'
        text = ('FS_MATCHED_BENCH owned=1 scope=1 recycle=1 inline=0\n' +
                self.fixture().replace('task_ns=500000000\n',
                                       'task_ns=500000000\n' + marker))
        opcodes = {24: 'stat', 19: 'pread', 21: 'pwrite'}
        rows = parse(text, opcodes)
        self.assertEqual(len(rows), 54)
        self.assertEqual(summarize(rows)['ext4']['stat']['0']['all_linux_task_switches_per_op'], 2.01)
        for bad in (text.replace(marker, '', 1), text.replace(marker, marker * 2, 1)):
            with self.assertRaises(RuntimeError):
                parse(bad, opcodes)

    def test_private_executor_requires_worker_control_and_no_owner_reclaim(self):
        text = ''.join(
            f'FS_INTERNAL_PUBLICATION fs={fs} opcode={opcode} depth={depth} '
            f'trial={trial} mode={mode} ops=1000 ticks=3000000000 elapsed_ns=1000000000 '
            f'publication_ticks=3000000 copy_ticks=300000 retirement_ticks=600000 '
            f'worker_publications=1000 switches=2000\n'
            f'FS_INTERNAL_COMPLETION wake_calls=100 batches={1000 if mode == "worker" else 0}\n'
            for fs, opcode, depth, trial, mode in itertools.product(
                ('ext4', 'tmpfs'), (24, 19, 21), (1, 8), (1, 2, 3), ('worker', 'executor')))
        text += 'FS_EXECUTOR_CHECKS fs=ext4 status=0\nFS_EXECUTOR_CHECKS fs=tmpfs status=0\nFS_INTERNAL_BENCH_DONE status=0\n'
        opcodes = {24: 'stat', 19: 'pread', 21: 'pwrite'}
        rows = parse_publication(text, opcodes, True)
        self.assertEqual(len(rows), 72)
        self.assertEqual(summarize_publication(rows, True)['ext4']['stat']['8']['executor']['collection_batches_per_op'], 0)
        for bad in (text.replace('mode=executor', 'mode=owner'),
                    text.replace('batches=0', 'batches=1'),
                    text.replace('worker_publications=1000', 'worker_publications=999')):
            with self.assertRaises(RuntimeError):
                parse_publication(bad, opcodes, True)
        flags = ['-DKOBOX_BOOT_FS=1', '-DKOBOX_FS_INTERNAL_BENCH=1', '-DKOBOX_FS_BENCH_ELAPSED_ONLY=1',
                 '-DKOBOX_FS_PUBLICATION_BENCH=1', '-DKOBOX_FS_EXECUTOR_BENCH=1', '-DKOBOX_FS_INLINE_DISABLED=1']
        check_runtime_flags(flags, False, True, False, True, True)
        for f in (flags[:-1], flags[:-2], flags):
            with self.assertRaises(RuntimeError):
                check_runtime_flags(f, False, True, False, True, f is not flags)

    def test_private_publication_pairs(self):
        text = ''.join(
            f'[Thread 7] FS_INTERNAL_PUBLICATION fs={fs} opcode={opcode} depth={depth} '
            f'trial={trial} mode={mode} ops=1000 ticks=3000000000 elapsed_ns=1000000000 '
            f'publication_ticks=3000000 copy_ticks=300000 retirement_ticks=600000 '
            f'worker_publications={1000 if mode == "worker" else 0} switches=2000\n'
            for fs, opcode, depth, trial, mode in itertools.product(
                ('ext4', 'tmpfs'), (24, 19, 21), (1, 8), (1, 2, 3), ('owner', 'worker')))
        text += '[Thread 7] FS_INTERNAL_BENCH_DONE status=0\n'
        opcodes = {24: 'stat', 19: 'pread', 21: 'pwrite'}
        rows = parse_publication(text, opcodes)
        self.assertEqual(len(rows), 72)
        summary = summarize_publication(rows)['ext4']['pread']['1']['worker']
        self.assertEqual(summary['publication_ns_per_request'], 1000)
        self.assertEqual(summary['copy_ns_per_request'], 100)
        self.assertEqual(summary['retirement_ns_per_request'], 200)
        self.assertEqual(summary['worker_publication_fraction'], 1)
        counted = text.replace('switches=2000\n', 'switches=2000\nFS_INTERNAL_COMPLETION wake_calls=100 batches=250\n')
        counts = summarize_publication(parse_publication(counted, opcodes))['ext4']['pread']['8']['worker']
        self.assertEqual(counts['completion_wake_calls_per_op'], 0.1)
        self.assertEqual(counts['collection_batches_per_op'], 0.25)
        for broken in (counted.replace('FS_INTERNAL_COMPLETION wake_calls=100 batches=250\n', '', 1),
                       counted.replace('wake_calls=100', 'wake_calls=1001'),
                       counted.replace('batches=250', 'batches=1001'),
                       counted.replace('batches=250', 'batches=bad')):
            with self.assertRaises(RuntimeError):
                parse_publication(broken, opcodes)
        for broken in (text[:-100], text.replace('trial=3', 'trial=2'),
                       text.replace('status=0', 'status=-5'),
                       text.replace('elapsed_ns=1000000000', 'elapsed_ns=999'),
                       text.replace('worker_publications=1000', 'worker_publications=1001'),
                       text.replace('worker_publications=0', 'worker_publications=1'),
                       text.replace('copy_ticks=300000', 'copy_ticks=4000000'),
                       text + 'FS_INTERNAL_PUBLICATION bad\n', text + 'FS_NATIVE_BEGIN\n'):
            with self.subTest(tail=broken[-100:]), self.assertRaises(RuntimeError):
                parse_publication(broken, opcodes)
        flags = ['-DKOBOX_BOOT_FS=1', '-DKOBOX_FS_INTERNAL_BENCH=1',
                 '-DKOBOX_FS_BENCH_ELAPSED_ONLY=1', '-DKOBOX_FS_PUBLICATION_BENCH=1']
        check_runtime_flags(flags, False, True, False, True)
        for args in ((flags, False, True), (flags[:-1], False, True, False, True),
                     (flags, True, True, False, True), (flags, False, False, False, True),
                     (flags, False, True, True, True)):
            with self.assertRaises(RuntimeError):
                check_runtime_flags(*args)

    def test_handoff_only_workload_and_lifetime(self):
        text = ''.join(f'[Thread 7] FS_INTERNAL_HANDOFF cpu=0 trial={trial} rounds=100000 '
                       'ticks=3000000000 elapsed_ns=1000000000 switches=200001\n'
                       for trial in (1, 2, 3)) + '[Thread 7] FS_INTERNAL_HANDOFF_DONE status=0\n'
        self.assertEqual(len(parse_handoff(text)), 3)
        for broken in (text.replace('trial=3', 'trial=2'), text.replace('elapsed_ns=1000000000', 'elapsed_ns=999'),
                       text.replace('switches=200001', 'switches=5'), text.replace('status=0', 'status=-5'),
                       text.replace('cpu=0 trial=3', 'cpu=1 trial=3'), text[:-60],
                       text + 'FS_INTERNAL_BOUNDARIES bad\n', text + 'FS_NATIVE_BEGIN\n',
                       text + 'FS_SERVICE_PROFILE bad\n'):
            with self.subTest(broken=broken[-100:]), self.assertRaises(RuntimeError):
                parse_handoff(broken)
        lean = ['-DKOBOX_BOOT_FS=1', '-DKOBOX_FS_INTERNAL_BENCH=1', '-DKOBOX_FS_BENCH_ELAPSED_ONLY=1']
        flags = lean + ['-DKOBOX_FS_HANDOFF_BENCH_ONLY=1']
        check_runtime_flags(flags, False, True, True)
        for f, verify, elapsed, handoff in ((flags, False, True, False), (lean, False, True, True),
                                          (flags, True, True, True), (flags, False, False, True)):
            with self.assertRaises(RuntimeError):
                check_runtime_flags(f, verify, elapsed, handoff)

    def test_guard_boundary_and_completeness(self):
        text = ''.join(f'[Thread 7] FS_INTERNAL_GUARD kind={kind} trial={trial} '
                       'ops=1000 ticks=4000000000 elapsed_ns=1000000000 task_ns=900000000\n'
                       for kind, trial in itertools.product(('irq', 'preempt'), (1, 2, 3)))
        self.assertEqual(len(parse_guards(text)), 6)
        self.assertEqual(parse_guards('historical log without guards'), [])
        for broken in (text.replace('trial=3', 'trial=2'), text.replace('kind=irq', 'kind=unknown'),
                       text.replace('elapsed_ns=1000000000', 'elapsed_ns=999'), text[:100]):
            with self.assertRaises(RuntimeError):
                parse_guards(broken)
        extended = text + text.replace('kind=irq', 'kind=clock').replace('kind=preempt', 'kind=fp')
        self.assertEqual(len(parse_guards(extended)), 12)
        with self.assertRaises(RuntimeError):
            parse_guards(extended.replace('kind=fp', 'kind=clock'))

    def test_host_boundary_counts_are_complete_not_physical_cpu_time(self):
        marker = '[Thread 7] FS_INTERNAL_BOUNDARIES preempt=100000 irq=4000 clock=2000 handoff=2000\n'
        text = self.fixture().replace('[Thread 7] FS_SERVICE_PROFILE', marker + '[Thread 7] FS_SERVICE_PROFILE')
        rows = parse(text, {24: 'stat', 19: 'pread', 21: 'pwrite'})
        stats = summarize(rows)['ext4']['pread']['1']['boundary_calls_per_op']
        self.assertEqual(stats, {'preempt': 100, 'irq': 4, 'clock': 2, 'handoff': 2})
        for broken in (text.replace(marker, '', 1), text.replace(marker, marker * 2, 1),
                       text.replace('handoff=2000', 'handoff=bad', 1)):
            with self.assertRaises(RuntimeError):
                parse(broken, {24: 'stat', 19: 'pread', 21: 'pwrite'})

    def test_native_handoff_counts_are_not_cpu_time(self):
        markers = ('[Thread 7] FS_NATIVE_HANDOFF count=2000 publish_ticks=300000 enter_ticks=4000 enter_waits=2\n'
                   '[Thread 7] FS_NATIVE_WAKE calls=2000 ticks=60000\n'
                   '[Thread 7] FS_NATIVE_PARK calls=2000 fast=20 waits=1980 wait_ticks=500000\n'
                   '[Thread 7] FS_NATIVE_OWNER_WAIT calls=10 ticks=50000\n')
        text = self.fixture().replace('[Thread 7] FS_SERVICE_PROFILE', markers + '[Thread 7] FS_SERVICE_PROFILE')
        rows = parse(text, {24: 'stat', 19: 'pread', 21: 'pwrite'})
        stats = summarize(rows)['ext4']['pread']['1']
        self.assertEqual(stats['native_per_op']['handoffs'], 2)
        self.assertEqual(stats['native_per_op']['publish_ticks'], 300)
        self.assertEqual(stats['native_publish_ticks_per_handoff'], 150)
        for broken in (text.replace(markers, '', 1), text.replace(markers, markers * 2, 1),
                       text.replace('wait_ticks=500000', 'wait_ticks=bad', 1)):
            with self.assertRaises(RuntimeError):
                parse(broken, {24: 'stat', 19: 'pread', 21: 'pwrite'})

    def test_combined_elapsed_is_separate_from_wake_elapsed(self):
        marker = ('[Thread 7] FS_NATIVE_HANDOFF count=2000 publish_ticks=300000 enter_ticks=4000 enter_waits=2\n'
                  '[Thread 7] FS_NATIVE_WAKE calls=100 ticks=60000\n'
                  '[Thread 7] FS_NATIVE_PARK calls=2000 fast=20 waits=1980 wait_ticks=500000\n'
                  '[Thread 7] FS_NATIVE_OWNER_WAIT calls=10 ticks=50000\n')
        combined = '[Thread 7] FS_NATIVE_COMBINED calls=1900 ticks=900000\n'
        text = self.fixture().replace('[Thread 7] FS_SERVICE_PROFILE',
            marker + combined + '[Thread 7] FS_SERVICE_PROFILE')
        opcodes = {24: 'stat', 19: 'pread', 21: 'pwrite'}
        stats = summarize(parse(text, opcodes))['ext4']['pread']['1']['native_per_op']
        self.assertEqual(stats['combined_calls'], 1.9)
        self.assertEqual(stats['combined_ticks'], 900)
        self.assertEqual(stats['wake_ticks'], 60)
        for broken in (text.replace(combined, '', 1), text.replace(combined, combined * 2, 1),
                       text.replace('ticks=900000', 'ticks=bad', 1)):
            with self.assertRaises(RuntimeError):
                parse(broken, opcodes)

    def test_normal_artifact_must_not_contain_diagnostics(self):
        normal = ['-DKOBOX_BOOT_FS=1']
        diagnostic = normal + ['-DKOBOX_FS_INTERNAL_BENCH=1', '-DKOBOX_FS_SERVICE_PROFILE=1']
        check_runtime_flags(normal, True)
        check_runtime_flags(diagnostic, False)
        for flags, verify in ((normal, False), (diagnostic, True),
                              (normal + ['-DKOBOX_RUNTIME_GATES=1'], True), ([], True)):
            with self.subTest(flags=flags, verify=verify), self.assertRaises(RuntimeError):
                check_runtime_flags(flags, verify)

    def test_elapsed_only_mode_requires_matching_artifact(self):
        normal = ['-DKOBOX_BOOT_FS=1']
        lean = normal + ['-DKOBOX_FS_INTERNAL_BENCH=1', '-DKOBOX_FS_BENCH_ELAPSED_ONLY=1']
        detailed = normal + ['-DKOBOX_FS_INTERNAL_BENCH=1', '-DKOBOX_FS_SERVICE_PROFILE=1']
        check_runtime_flags(lean, False, True)
        for flags, verify, mode in ((lean, False, False), (lean, True, False),
                                   (detailed, False, True), (normal, False, True),
                                   (normal, True, True),
                                   (lean + ['-DKOBOX_FS_SERVICE_PROFILE=1'], False, True),
                                   (lean + ['-DKOBOX_FS_BOUNDARY_CALLERS=1'], False, True)):
            with self.subTest(flags=flags), self.assertRaises(RuntimeError):
                check_runtime_flags(flags, verify, mode)

    def test_elapsed_only_has_no_fabricated_profile_fields(self):
        text = '\n'.join(line for line in self.fixture().splitlines()
                         if 'FS_SERVICE_PROFILE' not in line and 'FS_WORKER_PROFILE' not in line) + '\n'
        opcodes = {24: 'stat', 19: 'pread', 21: 'pwrite'}
        rows = parse(text, opcodes, elapsed_only=True)
        self.assertEqual(len(rows), 54)
        self.assertTrue(all('stages' not in row and 'owner' not in row for row in rows))
        summary = summarize(rows)['ext4']['pread']['1']
        self.assertEqual(summary['ops_per_second'], 1000)
        self.assertNotIn('stage_ticks_per_op', summary)
        self.assertNotIn('owner_ticks_per_op', summary)
        with self.assertRaises(RuntimeError):
            parse(text, opcodes)
        for record in ('FS_SERVICE_PROFILE opcode=24 calls=bad', 'FS_WORKER_META new_work=bad',
                       'FS_INTERNAL_BOUNDARIES preempt=bad', 'FS_NATIVE_BEGIN',
                       'FS_BOUNDARY_SITE label=bad', 'FS_INTERNAL_SWITCHES switches=bad'):
            with self.subTest(record=record), self.assertRaises(RuntimeError):
                parse(text + record + '\n', opcodes, elapsed_only=True)
        for broken in (text.replace('trial=3', 'trial=2'), text.replace('status=0', 'status=-5'),
                       text.replace('elapsed_ns=1000000000', 'elapsed_ns=999'), text[:-100]):
            with self.assertRaises(RuntimeError):
                parse(broken, opcodes, elapsed_only=True)

    def test_handoff_detail_completeness_and_accounting(self):
        base = ('[Thread 7] FS_NATIVE_HANDOFF count=2000 publish_ticks=300000 enter_ticks=4000 enter_waits=2\n'
                '[Thread 7] FS_NATIVE_WAKE calls=2000 ticks=60000\n'
                '[Thread 7] FS_NATIVE_PARK calls=2000 fast=20 waits=1980 wait_ticks=500000\n'
                '[Thread 7] FS_NATIVE_OWNER_WAIT calls=10 ticks=50000\n')
        detail = ('[Thread 7] FS_NATIVE_RESUME count=2000 ticks=290000 tail_ticks=10000 missing=0\n'
                  '[Thread 7] FS_NATIVE_LOCAL count=1800 ticks=240000\n'
                  '[Thread 7] FS_NATIVE_REMOTE count=200 ticks=60000\n')
        text = self.fixture().replace('[Thread 7] FS_SERVICE_PROFILE', base + detail + '[Thread 7] FS_SERVICE_PROFILE')
        opcodes = {24: 'stat', 19: 'pread', 21: 'pwrite'}
        stats = summarize(parse(text, opcodes))['ext4']['pread']['1']['native_per_op']
        self.assertEqual(stats['resume_ticks'], 290)
        self.assertEqual(stats['remote_count'], .2)
        unsupported = text.replace('LOCAL count=1800 ticks=240000', 'LOCAL count=0 ticks=0').replace(
            'REMOTE count=200 ticks=60000', 'REMOTE count=0 ticks=0')
        self.assertEqual(parse(unsupported, opcodes)[0]['native']['remote_count'], 0)
        with self.assertRaises(RuntimeError):
            parse(unsupported.replace('LOCAL count=0 ticks=0', 'LOCAL count=0 ticks=1', 1), opcodes)
        missing = text.replace('RESUME count=2000 ticks=290000', 'RESUME count=1999 ticks=280000').replace(
            'tail_ticks=10000 missing=0', 'tail_ticks=10000 missing=1')
        self.assertEqual(parse(missing, opcodes)[0]['native']['resume_missing'], 1)
        for broken in (text.replace(detail, '', 1), text.replace(detail, detail * 2, 1),
                       text.replace('RESUME count=2000', 'RESUME count=1999', 1),
                       text.replace('tail_ticks=10000', 'tail_ticks=9999', 1),
                       text.replace('LOCAL count=1800', 'LOCAL count=1799', 1),
                       text.replace('REMOTE count=200 ticks=60000', 'REMOTE count=200 ticks=59999', 1),
                       text.replace('REMOTE count=200', 'REMOTE count=bad', 1),
                       text.replace(base, '', 1)):
            with self.assertRaises(RuntimeError):
                parse(broken, opcodes)

    def test_worker_path_intervals_and_counts(self):
        marker = '[Thread 7] FS_WORKER_PATH calls=1000 submit_nonwake=20 ready_wake=180 completion_to_collect=300\n'
        text = self.fixture().replace('calls=1000 submit=200 release=100\n',
            'calls=1000 submit=200 release=100\n' + marker)
        opcodes = {24: 'stat', 19: 'pread', 21: 'pwrite'}
        stats = summarize(parse(text, opcodes))['ext4']['pread']['1']['worker_path_ticks_per_op']
        self.assertEqual(stats, {'submit_nonwake': .02, 'ready_wake': .18, 'completion_to_collect': .3})
        for broken in (text.replace(marker, '', 1), text.replace(marker, marker * 2, 1),
                       text.replace('FS_WORKER_PATH calls=1000', 'FS_WORKER_PATH calls=999', 1),
                       text.replace('ready_wake=180', 'ready_wake=179', 1),
                       text.replace('completion_to_collect=300', 'completion_to_collect=bad', 1)):
            with self.assertRaises(RuntimeError):
                parse(broken, opcodes)

    def fixture(self):
        lines = []
        for fs, opcode, depth, trial in itertools.product(('ext4', 'tmpfs'), (24, 19, 21), (0, 1, 8), (1, 2, 3)):
            lines.append(f'[Thread 7] FS_INTERNAL_BENCH fs={fs} opcode={opcode} depth={depth} trial={trial} '
                         'ops=1000 ticks=4000000000 elapsed_ns=1000000000 task_ns=500000000')
            lines.append(f'[Thread 7] FS_SERVICE_PROFILE opcode={opcode} calls=1000 snapshot=10 '
                         'auth=20 execute=30 release=40 task_work=50 queue=60')
            if depth:
                lines.append('[Thread 7] FS_WORKER_PROFILE calls=1000 submit=200 release=100')
        lines.append('[Thread 7] FS_INTERNAL_BENCH_DONE status=0')
        return '\n'.join(lines) + '\n'

    def test_thread_prefix_counts_and_throughput_boundary(self):
        rows = parse(self.fixture(), {24: 'stat', 19: 'pread', 21: 'pwrite'})
        self.assertEqual(len(rows), 54)
        row = summarize(rows)['tmpfs']['pread']['8']
        self.assertEqual(row['ops_per_second'], 1000)
        self.assertEqual(row['linux_task_ns_per_op'], 500000)
        self.assertEqual(row['stage_ticks_per_op']['queue'], .06)

    def test_task_switch_diagnostics_must_be_complete(self):
        original = self.fixture()
        text = original.replace('\n[Thread 7] FS_SERVICE_PROFILE',
                                '\n[Thread 7] FS_INTERNAL_SWITCHES switches=2000'
                                '\n[Thread 7] FS_SERVICE_PROFILE')
        rows = parse(text, {24: 'stat', 19: 'pread', 21: 'pwrite'})
        self.assertEqual(summarize(rows)['ext4']['pread']['1']['linux_task_switches_per_op'], 2)
        for broken in (text.replace('[Thread 7] FS_INTERNAL_SWITCHES switches=2000\n', '', 1),
                       text.replace('FS_INTERNAL_SWITCHES switches=2000',
                                    'FS_INTERNAL_SWITCHES switches=2000\n'
                                    '[Thread 7] FS_INTERNAL_SWITCHES switches=1', 1)):
            with self.assertRaises(RuntimeError):
                parse(broken, {24: 'stat', 19: 'pread', 21: 'pwrite'})

    def test_metadata_diagnostics_match_successful_admission(self):
        text = self.fixture().replace('calls=1000 submit=200 release=100',
            'calls=1000 submit=200 release=100\n'
            '[Thread 7] FS_WORKER_META new_work=0 reused_work=1000 new_client=0 reused_client=500')
        rows = parse(text, {24: 'stat', 19: 'pread', 21: 'pwrite'})
        stats = summarize(rows)['ext4']['pread']['1']['metadata_per_op']
        self.assertEqual(stats['new_work'], 0)
        self.assertEqual(stats['reused_work'], 1)
        self.assertEqual(stats['reused_client'], .5)
        for broken in (text.replace('reused_work=1000', 'reused_work=999'),
                       text.replace('reused_client=500', 'reused_client=1001'),
                       text.replace('[Thread 7] FS_WORKER_META new_work=0 reused_work=1000 '
                                    'new_client=0 reused_client=500\n', '', 1)):
            with self.assertRaises(RuntimeError):
                parse(broken, {24: 'stat', 19: 'pread', 21: 'pwrite'})

    def test_buffers_are_complete_and_match_successful_admission(self):
        marker = '[Thread 7] FS_WORKER_BUFFER new=1 reused=999\n'
        text = self.fixture().replace('calls=1000 submit=200 release=100\n',
            'calls=1000 submit=200 release=100\n' + marker)
        rows = parse(text, {24: 'stat', 19: 'pread', 21: 'pwrite'})
        self.assertEqual(summarize(rows)['ext4']['pread']['1']['buffers_per_op'],
                         {'new': .001, 'reused': .999})
        for broken in (text.replace(marker, '', 1), text.replace(marker, marker * 2, 1),
                       text.replace('reused=999', 'reused=998', 1),
                       text.replace('reused=999', 'reused=bad', 1)):
            with self.assertRaises(RuntimeError):
                parse(broken, {24: 'stat', 19: 'pread', 21: 'pwrite'})

    def test_placement_is_complete_and_matches_successful_admission(self):
        marker = '[Thread 7] FS_WORKER_PLACEMENT inline=0 woken=100 queued=900 empty=40\n'
        text = self.fixture().replace('calls=1000 submit=200 release=100\n',
            'calls=1000 submit=200 release=100\n' + marker)
        rows = parse(text, {24: 'stat', 19: 'pread', 21: 'pwrite'})
        self.assertEqual(summarize(rows)['ext4']['pread']['1']['placement_per_op'],
                         {'inline': 0, 'woken': .1, 'queued': .9, 'empty': .04})
        for broken in (text.replace(marker, '', 1), text.replace(marker, marker * 2, 1),
                       text.replace('queued=900', 'queued=899', 1),
                       text.replace('queued=900', 'queued=bad', 1)):
            with self.assertRaises(RuntimeError):
                parse(broken, {24: 'stat', 19: 'pread', 21: 'pwrite'})

    def test_partial_failed_duplicate_short_or_wrong_counts_are_rejected(self):
        text = self.fixture()
        for broken in (text.replace('status=0', 'status=-5'),
                       text.replace('trial=3', 'trial=2'),
                       text.replace('elapsed_ns=1000000000', 'elapsed_ns=900000000'),
                       text.replace('calls=1000', 'calls=999'), text[:-100]):
            with self.subTest(broken=broken[-80:]), self.assertRaises(RuntimeError):
                parse(broken, {24: 'stat', 19: 'pread', 21: 'pwrite'})


if __name__ == '__main__':
    unittest.main()
