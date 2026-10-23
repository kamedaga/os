#!/usr/bin/env python3
"""The movq spelling must not permit MMX/SSE in a GPR-only ABI entry."""
from pathlib import Path
import runpy
import sys
import unittest
from unittest.mock import patch

CHECKER = Path(__file__).with_name('check_kobox2_notification_fp.py')
with patch.object(sys, 'argv', [str(CHECKER)]):
    leaf = runpy.run_path(str(CHECKER))['leaf']


class IntegerMoveAudit(unittest.TestCase):
    def check(self, instruction):
        with patch.dict(leaf.__globals__, instruction_copies=lambda *_: [[instruction, ('ret', '')]]):
            return leaf(Path('fixture'), 'entry', set())

    def test_integer_movq(self):
        self.check(('movq', '$0x800,0x10(%rax)'))

    def test_unsigned_poll_comparison(self):
        for register in ('%bpl', '%sil'):
            with self.subTest(register=register):
                self.check(('setb', register))

    def test_vector_moves_rejected(self):
        for operand in ('%mm0,%rax', '%rax,%mm1', '%xmm0,%rax', '%rax,%xmm1'):
            with self.subTest(operand=operand), self.assertRaises(AssertionError):
                self.check(('movq', operand))

    def test_other_fp_and_unknown_rejected(self):
        for instruction in (('movaps', '%xmm0,%xmm1'), ('fld1', ''), ('unknown', '')):
            with self.subTest(instruction=instruction), self.assertRaises(AssertionError):
                self.check(instruction)


if __name__ == '__main__':
    unittest.main()
