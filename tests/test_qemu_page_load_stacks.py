import re
import argparse
import unittest

from qemu_page_load_stacks import frame_in_stack, register, sample_interval, user_code, raw_kernel_frames_allowed
from unittest.mock import Mock


class StackParsingTests(unittest.TestCase):
    def test_raw_frames_exclude_user_and_halted_contexts(self):
        self.assertTrue(raw_kernel_frames_allowed('RIP=ffffffff80001234 CPL=0 HLT=0'))
        for registers in (
            'RIP=ffffffff80001234 CPL=0 HLT=1',
            'RIP=0000010000001234 CPL=0 HLT=0',
            'RIP=ffffffff80001234 CPL=3 HLT=0',
        ):
            self.assertFalse(raw_kernel_frames_allowed(registers))

    def test_user_code_is_bounded_and_does_not_read_kernel_or_halted_cpu(self):
        hmp = Mock(return_value='0x0000010020000000: ' + ' '.join(f'0x{i:02x}' for i in range(32)))
        self.assertIsNone(user_code('RIP=ffffffff80000000 CPL=0 HLT=0', hmp))
        self.assertIsNone(user_code('RIP=0000010020000000 CPL=3 HLT=1', hmp))
        hmp.assert_not_called()
        result = user_code('RIP=0000010020000000 CPL=3 HLT=0', hmp)
        self.assertEqual(result['hex'], bytes(range(32)).hex())
        hmp.assert_called_once_with('x /32bx 0x10020000000')

    def test_failed_or_short_instruction_read_is_not_a_match(self):
        for raw in ('Cannot access memory', '0x10020000000: 0x01 0x02'):
            result = user_code('RIP=0000010020000000 CPL=3 HLT=0', Mock(return_value=raw))
            self.assertIsNone(result['hex'])

    def test_sample_interval_bounds(self):
        self.assertEqual(sample_interval('.25'), .25)
        for value in ('0', '-1', '.01', '6', 'nan', 'inf'):
            with self.assertRaises(argparse.ArgumentTypeError):
                sample_interval(value)

    def test_kernel_stack_bounds(self):
        rsp = 0xffffffffc0400000
        self.assertTrue(frame_in_stack(rsp + 16, rsp))
        for frame in (rsp - 8, rsp + 1, rsp + 131072):
            self.assertFalse(frame_in_stack(frame, rsp))
        self.assertFalse(frame_in_stack(0x100000010, 0x100000000))

    def test_registers_and_memory_pair(self):
        self.assertEqual(register('RIP=ffffffff80000010 RFL=00000246', 'RIP'),
                         0xffffffff80000010)
        raw = 'ffffffffc0400010: 0xffffffffc0400080 0xffffffff80001000\r\n'
        self.assertEqual(re.findall(r'0x([0-9a-f]{16})(?!:)', raw),
                         ['ffffffffc0400080', 'ffffffff80001000'])
        raw = '0xffffffffc0400010: 0xffffffffc0400080 0xffffffff80001000\r\n'
        self.assertEqual(len(re.findall(r'0x([0-9a-f]{16})(?!:)', raw)), 2)


if __name__ == '__main__':
    unittest.main()
