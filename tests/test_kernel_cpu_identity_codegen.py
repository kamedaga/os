import unittest

from check_kernel_cpu_identity_codegen import audit, parse_disassembly


class IdentityCodegenTest(unittest.TestCase):
    def check(self, text):
        return audit(*parse_disassembly(text), roots=('entry',))[0]

    def test_follows_calls_tail_calls_and_local_branches(self):
        text = '''100 <entry>:
100: call 200 <helper>
105: jmp 300 <tail>
200 <helper>:
200: test %rax,%rax
203: jne 200 <helper>
205: ret
300 <tail>:
300: mov %rax,%rcx
303: ret
'''
        self.assertEqual([], self.check(text))
        self.assertTrue(self.check(text.replace('mov %rax,%rcx', 'movups (%rax),%xmm0')))

    def test_aggregate_copy_and_implicit_state_instructions_are_rejected(self):
        for instruction in ('movups (%rax),%xmm0', 'movups %xmm0,(%rax)',
                            'pxor %xmm0,%xmm0', 'fld1', 'fninit', 'vzeroupper',
                            'ldmxcsr (%rax)', 'xrstor64 (%rax)', 'emms',
                            'tilezero %tmm0', 'tilerelease', 'ldtilecfg (%rax)',
                            'bndmov %bnd0,(%rax)', 'rdpkru', 'wrpkru', 'xgetbv', 'xsetbv'):
            with self.subTest(instruction=instruction):
                self.assertTrue(self.check('100 <entry>:\n100: ' + instruction + '\n104: ret\n'))

    def test_missing_and_unresolved_scope_fails_closed(self):
        self.assertTrue(self.check(''))
        for instruction in ('call *%rax', 'jmp *%rax', 'call 900 <missing>'):
            with self.subTest(instruction=instruction):
                self.assertTrue(self.check('100 <entry>:\n100: ' + instruction + '\n104: ret\n'))


if __name__ == '__main__':
    unittest.main()
