#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import unittest
from run_kobox2_native_fixture import gates_in

class GateParsing(unittest.TestCase):
    def test_actual_lines(self):
        for text in ('GATE=PASS\n', '[Thread 123] GATE=PASS\r\n'):
            self.assertTrue(gates_in(text, ['GATE=PASS'])['GATE=PASS'])

    def test_quoted_truncated_and_wrong_gate(self):
        for text in ('expected GATE=PASS\n', 'GATE=PASSextra\n', 'GATE=PAS\n', 'OTHER=PASS\n'):
            self.assertFalse(gates_in(text, ['GATE=PASS'])['GATE=PASS'])

    def test_all_required(self):
        self.assertEqual(gates_in('ONE=PASS\n', ['ONE=PASS', 'TWO=PASS']),
                         {'ONE=PASS': True, 'TWO=PASS': False})

if __name__ == '__main__':
    unittest.main()
