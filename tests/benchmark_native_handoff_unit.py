#!/usr/bin/env python3
import unittest
from benchmark_native_handoff import parse


class HandoffParser(unittest.TestCase):
    def setUp(self):
        self.text = ''.join(
            f'[Thread 1] NATIVE_HANDOFF trial={i} rounds=128000 ticks=3700000000 elapsed_ns=1000000000\n'
            for i in range(1, 4)) + '[Thread 1] NATIVE_HANDOFF=PASS\n'

    def test_valid(self):
        self.assertEqual(len(parse(self.text)), 3)
        self.assertEqual(len(parse(self.text.replace('trial=', 'trial=[Thread 1] '))), 3)

    def test_invalid(self):
        for text in (self.text.replace('trial=3', 'trial=2'),
                     self.text.replace('elapsed_ns=1000000000', 'elapsed_ns=999'),
                     self.text.replace('rounds=128000', 'rounds=0'),
                     self.text.replace('ticks=3700000000', 'ticks=0'),
                     self.text.replace('rounds=128000', 'rounds=128001'),
                     self.text.replace('NATIVE_HANDOFF=PASS', ''),
                     self.text + 'NATIVE_HANDOFF=FAIL code=3\n'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                parse(text)


if __name__ == '__main__':
    unittest.main()
