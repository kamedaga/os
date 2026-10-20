import struct
import unittest
from check_kernel_stack_frames import audit, parse_disassembly, frame, switch_fingerprint


class StackFramesTest(unittest.TestCase):
    def report(self, text, roots=('entry',), boot_roots=(), **kwargs):
        return audit(*parse_disassembly(text), roots=roots, boot_roots=boot_roots, **kwargs)

    def test_limits_and_boot_becoming_runtime(self):
        text = '''100 <entry>:
100: ret
200 <boot>:
200: call 300 <iommu.init>
205: ret
300 <iommu.init>:
300: sub $0x19000,%rsp
305: add $0x19000,%rsp
310: ret
'''
        self.assertFalse(self.report(text, boot_roots=('boot',))['errors'])
        changed = text.replace('100: ret', '100: call 300 <iommu.init>\n105: ret')
        errors = self.report(changed, boot_roots=('boot',))['errors']
        self.assertTrue(any('reached from runtime root' in e for e in errors))
        self.assertTrue(any('exceeds 32768' in e for e in errors))
        self.assertTrue(self.report(text.replace('0x19000', '0x21000'), boot_roots=('boot',))['errors'])

    def test_branch_mid_body_push_and_alignment_count(self):
        text = '''100 <entry>:
100: sub $0x7fc8,%rsp
105: test %rax,%rax
110: je 140 <entry+0x40>
115: push %rbx
120: and $0xffffffffffffffe0,%rsp
125: call 200 <helper>
130: ud2
140: add $0x7fc8,%rsp
145: ret
200 <helper>:
200: ret
'''
        # Return slot + allocation + push + 31 alignment + outgoing return slot.
        report = self.report(text)
        self.assertEqual(8 + 0x7fc8 + 8 + 31 + 8, report['functions'][0]['maximum_bytes'])
        self.assertFalse(report['errors'])
        self.assertTrue(self.report(text.replace('115: push %rbx', '115: push %rbx\n117: push %r12'))['errors'])
        self.assertTrue(self.report(text.replace('0xffffffffffffffe0', '0xffffffffffffffc0'))['errors'])
        self.assertTrue(self.report(text.replace('115: push %rbx', '115: sub $0x80,%rsp'))['errors'])

    def test_saved_stack_restore_does_not_accumulate_calls(self):
        text = '''100 <entry>:
100: push %r15
101: mov %rsp,%r15
102: and $0xfffffffffffffff0,%rsp
103: sub $0x20,%rsp
104: call 200 <helper>
105: mov %r15,%rsp
106: test %rax,%rax
107: jne 101 <entry+0x1>
108: pop %r15
109: ret
200 <helper>:
200: ret
'''
        result = self.report(text)
        self.assertFalse(result['errors'])
        self.assertEqual(16 + 15 + 32 + 8, result['functions'][0]['maximum_bytes'])

    def test_opt_in_switch_variant_is_exact_and_uses_its_own_index(self):
        old = "100 <entry>:\n100: mov %rax,%rsp\n105: ret\n"
        new = "100 <entry>:\n100: push %rbx\n101: mov %rax,%rsp\n105: ret\n"
        old_body = parse_disassembly(old)[0][0x100]
        new_body = parse_disassembly(new)[0][0x100]
        contract = dict(sha256=switch_fingerprint(old_body, ()),
                        switches={'0': dict(initial_bytes=0)},
                        variants=[dict(sha256=switch_fingerprint(new_body, ()),
                                       switches={'1': dict(initial_bytes=0)})])
        for text in (old, new):
            self.assertFalse(self.report(text, contracts={'entry': contract})['errors'])
        mutated = new.replace('push %rbx', 'sub %rcx,%rsp')
        self.assertTrue(self.report(mutated, contracts={'entry': contract})['errors'])
        contract['variants'][0]['switches'] = {'0': dict(initial_bytes=0)}
        self.assertTrue(self.report(new, contracts={'entry': contract})['errors'])

    def test_unknown_rsp_and_dynamic_alloca_fail_closed(self):
        for op in ('sub %rax,%rsp', 'mov %rax,%rsp', 'and %rax,%rsp',
                   'lea (%rsp,%rax,8),%rsp', 'pop %rsp', 'xchg %rsp,%rax',
                   'inc %rsp', 'mov %eax,%esp', 'enter $0x10,$0x0',
                   'data16 push %rbx', 'rex.W sub %rax,%rsp', 'lretq',
                   'lcall $0x8,$0x200', 'sysenter', 'bnd call 200 <callee>'):
            with self.subTest(op=op):
                self.assertTrue(self.report(f'100 <entry>:\n100: {op}\n110: ret\n')['errors'])
        self.assertTrue(self.report('100 <entry>:\n100: push %rax\n105: jmp 100 <entry>\n')['errors'])
        self.assertTrue(self.report('')['errors'])

    def test_jump_table_late_branch_and_tail_reachability(self):
        text = '''100 <entry>:
100: jmp *0x1000(,%rax,8)
110: sub $0x8000,%rsp
115: ud2
120: jmp 200 <tail>
200 <tail>:
200: ret
'''
        sections = [(0x1000, struct.pack('<QQ', 0x120, 0x110), False)]
        result = self.report(text, sections=sections)
        self.assertTrue(any('exceeds 32768' in e for e in result['errors']))
        self.assertTrue(result['functions'][1]['runtime_reachable'])
        self.assertTrue(self.report(text)['errors'])  # missing table is not success

    def test_verified_aggregate_must_stay_unreachable(self):
        text = '''100 <entry>:
100: ret
200 <pacha_eevdf_charge>:
200: sub $0x4800,%rsp
205: add $0x4800,%rsp
210: ret
'''
        self.assertFalse(self.report(text)['errors'])
        self.assertTrue(self.report(text.replace('100: ret', '100: call 200 <pacha_eevdf_charge>\n105: ret'))['errors'])

    def test_local_far_return_and_no_interprocedural_budget(self):
        text = """100 <entry>:
100: push $0x8
101: push $0x103
102: lretq
103: sub $0x5000,%rsp
104: call 200 <callee>
105: add $0x5000,%rsp
106: ret
200 <callee>:
200: sub $0x5000,%rsp
201: add $0x5000,%rsp
202: ret
"""
        result = self.report(text)
        self.assertFalse(result['errors'])
        self.assertEqual(0x5000 + 16, result['functions'][0]['maximum_bytes'])
        self.assertEqual(0x5000 + 8, result['functions'][1]['maximum_bytes'])

    def test_reviewed_switch_cannot_hide_new_rsp_operation(self):
        text = '100 <entry>:\n100: mov %rax,%rsp\n105: push %rbx\n110: ud2\n'
        bodies, names = parse_disassembly(text)
        contract = {'sha256': switch_fingerprint(bodies[0x100], ()),
                    'switches': {'0': {'initial_bytes': 0, 'reason': 'fixture stack switch'}}}
        self.assertFalse(self.report(text, contracts={'entry': contract})['errors'])
        self.assertTrue(self.report(text.replace('push %rbx', 'sub %rcx,%rsp'), contracts={'entry': contract})['errors'])


if __name__ == '__main__':
    unittest.main()
