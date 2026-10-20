import unittest
from check_kernel_scalar_codegen import audit

class ScalarGate(unittest.TestCase):
    def check(self, opcode, args='', name='indirect.callback'):
        return audit({16:[(16,opcode,args)]},{name:16}, roots=())[0]
    def test_scalar(self):
        self.assertEqual(self.check('mov','%rax,%r12'), [])
    def test_all_simd_families(self):
        for op,args in [('movaps','%xmm0,%xmm1'),('pxor','%mm0,%mm0'),
                        ('vmovdqa','%ymm0,%ymm1'),('kmovq','%k0,%rax'),
                        ('fld1',''),('vzeroupper',''),('ldmxcsr','(%rax)')]:
            with self.subTest(op=op): self.assertTrue(self.check(op,args))
    def test_boundary_is_instruction_not_body_exemption(self):
        self.assertFalse(self.check('xsave64','(%rax)','saveCurrentThreadXState'))
        self.assertTrue(self.check('pxor','%xmm0,%xmm0','saveCurrentThreadXState'))
        self.assertTrue(self.check('xrstor64','(%rax)','saveCurrentThreadXState'))
    def test_unreviewed_state_boundary(self):
        self.assertTrue(self.check('xsave64','(%rax)'))
    def test_missing_roots(self):
        self.assertTrue(audit({}, {})[0])
if __name__ == '__main__': unittest.main()
