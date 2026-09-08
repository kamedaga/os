"""Small synthetic trace: clock fit, nested waits, FD reuse and damaged logs."""
import json
from pathlib import Path
import tempfile
import unittest

from analyze_gui_startup import analyze


class StartupTraceTest(unittest.TestCase):
    def analyze_fixture(self, damage=None, overlap=False, console=False):
        hz = 3600000000
        tsc = lambda seconds: round(seconds * hz)
        lines = [
            f'2.1 [gui-prof] begin 42 42 2 {tsc(2.1)} 0 36000000',
            f'20.1 [gui-prof] end 42 {tsc(20.1)} 18000000000',
            '20.101 [gui-prof] bin 42 4 16 18000000 1',
            '20.102 [gui-prof] detail 42 4 38 18000000 1',
            f'20.103 [gui-prof] wait 42 {tsc(2.14)} {tsc(2.145)} 9 1 1 1 999 1 100 1',
            f'20.104 [gui-prof] peer 42 {tsc(2.12)} 9 100',
            '20.105 [gui-peer] /tmp/socket-old',
            f'20.106 [gui-prof] peer 42 {tsc(2.13)} 9 200',
            '20.107 [gui-peer] /tmp/socket-new',
            '20.108 [gui-prof] dropped 42 0 0',
            '20.109 [gui-prof] done 42',
        ]
        if damage is not None:
            lines[2] = damage
        if overlap:
            lines += [f'1.0 [gui-prof] begin 41 41 1 {tsc(1)} 0 36000000',
                      f'1.9 [gui-prof] end 41 {tsc(1.9)} 900000000',
                      '2.2 [gui-prof] done 41']
        with tempfile.TemporaryDirectory(prefix='gui-trace-test-') as name:
            path = Path(name)
            (path/('console.log' if console else 'serial.log')).write_text('\n'.join(lines)+'\n')
            (path/'results.json').write_text(json.dumps(dict(events=[dict(
                event='terminal-unit', start_host_s=2, lower_s=.25, upper_s=.27,
                paint_lower_host_s=2.25, paint_upper_host_s=2.27)])))
            return analyze(path)[0]

    def test_exclusive_total_and_nested_wait(self):
        row = self.analyze_fixture()
        total = row['prefix_ms'] + row['outside_syscalls_ms'] + row['paint_tail_ms']
        total += sum(c['ms'] for c in row['categories'])
        self.assertAlmostEqual(total, row['total_ms'])
        self.assertAlmostEqual(row['native_waits'][0]['ms'], 5)
        self.assertEqual(row['native_waits'][0]['path'], '/tmp/socket-old')
        self.assertEqual(row['trace_dropped'], [0, 0])

    def test_corrupted_bin_rejected(self):
        row = self.analyze_fixture('20.101 [gui-prof] bin 42[netd] 4 16 18000000 1')
        self.assertIn('error', row)

    def test_corrupted_tag_rejected(self):
        row = self.analyze_fixture('20.101 [gui-prof] bi[netd]n 42 4 16 18000000 1')
        self.assertIn('error', row)

    def test_short_numeric_record_rejected(self):
        row = self.analyze_fixture('20.101 [gui-prof] bin 42 4')
        self.assertIn('error', row)

    def test_previous_dump_overlap_rejected(self):
        self.assertIn('overlaps', self.analyze_fixture(overlap=True)['error'])

    def test_console_transport(self):
        self.assertNotIn('error', self.analyze_fixture(console=True))


if __name__ == '__main__':
    unittest.main()
