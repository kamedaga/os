import json
from pathlib import Path
import struct
import tempfile
import unittest
from kernel_tlb_profile import attach, totals


class TlbProfileTest(unittest.TestCase):
    def test_all_cpu_rows_and_shape(self):
        data = struct.pack('<1024Q', *range(1024))
        self.assertEqual(totals(data)[5], sum(cpu * 16 + 5 for cpu in range(64)))
        with self.assertRaises(ValueError):
            totals(data[:-8])

    def test_boot_capacity_above_mask_width(self):
        for cpus in (1,65,72,257):
            data=struct.pack('<'+str(cpus*16)+'Q',*range(cpus*16))
            self.assertEqual(totals(data,cpus)[8],sum(cpu*16+8 for cpu in range(cpus)))
            with self.assertRaises(ValueError):totals(data,cpus+1)

    def test_reads_only_stopped_and_keeps_cumulative_separate(self):
        class Profile:
            stopped = False
            value = 20
            def __init__(self):
                self.qmp = self
            def execute(self, command):
                self.stopped = command == 'stop'
            def stop_clean(self):
                self.stopped = True
            def read(self, key, label):
                assert self.stopped
                return struct.pack('<1024Q', *([self.value] * 1024))
            def reset_stopped(self):
                assert self.stopped
            def decode(self, raw):
                return dict(histograms=[])
        profile = Profile()
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'full.json'
            attach(profile, output)
            self.assertEqual(json.loads(output.read_text())['smp_perf'][5], 20 * 64)
            self.assertFalse(profile.stopped)
            profile.stop_clean()
            profile.reset_stopped()
            profile.value += 7
            self.assertEqual(profile.decode({})['smp_perf'][5], 7 * 64)
            profile.value = 0
            with self.assertRaises(ValueError):
                profile.decode({})


if __name__ == '__main__':
    unittest.main()
