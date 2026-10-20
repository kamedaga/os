import unittest

from inventory_kernel_foundations import subsystem, thread_storage_references


class ThreadStorageInventoryTests(unittest.TestCase):
    def test_calls_aliases_and_direct_arrays_are_not_dropped(self):
        source = '''const borrow = scheduler.threadContext;
const ctx = scheduler.threadContextMutable(id);
const image = entry.x_state_addr;
const old = state.thread_table.contexts;
const timers = state.thread_table.signal_timers;
const hot = state.thread_table.hot_states;
extern var thread_contexts_ptr: *anyopaque;
'''
        rows = thread_storage_references(source)
        self.assertEqual(len(rows), 7)
        self.assertEqual([r['line'] for r in rows], list(range(1, 8)))
        self.assertTrue(all(r['kind'] == 'code' for r in rows))
        self.assertTrue(all(r['exclusion'] is None and r['borrow_end'] is None for r in rows))

    def test_comment_and_assembly_are_separate_not_success_evidence(self):
        rows = thread_storage_references(
            '// threadContext is mentioned here\n'
            'asm volatile ("mov thread_contexts_ptr, %rax");\n'
            'const n = unrelated_threadContext;\n'
            'test "borrow" { _ = threadContext(0); }\n')
        self.assertEqual([r['kind'] for r in rows], ['non-code', 'non-code', 'code'])
        self.assertEqual([r['line'] for r in rows], [1, 2, 4])

    def test_extracted_runtime_has_its_own_responsibility(self):
        self.assertEqual(subsystem('kernel/src/xstate_runtime.zig'), 'thread-xstate-transition')


if __name__ == '__main__':
    unittest.main()
