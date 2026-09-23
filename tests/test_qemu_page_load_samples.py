import contextlib
import io
import json
import itertools
import unittest
from unittest.mock import MagicMock, patch

from qemu_page_load_samples import NavigationGate, main


class NavigationGateTests(unittest.TestCase):
    def test_next_navigation_selects_start_not_prior_completion(self):
        gate = NavigationGate('*')
        gate.feed('PAGE_EVENT ms=1 view=0x1 event=3 uri=https://old/\n'
                  'PAGE_EVENT ms=2 view=0x2 event=0 uri=https://new/\n'
                  'PAGE_EVENT ms=3 view=0x3 event=0 uri=https://other/\n'
                  'PAGE_EVENT ms=4 view=0x3 event=3 uri=https://other/\n')
        self.assertEqual(gate.view, '0x2')
        self.assertFalse(gate.complete)
        gate.feed('PAGE_EVENT ms=5 view=0x2 event=1 uri=https://redirect/\n'
                  'PAGE_EVENT ms=6 view=0x2 event=3 uri=https://redirect/\n')
        self.assertTrue(gate.complete)
        self.assertEqual([e['event'] for e in gate.events], [0, 1, 3])
        self.assertEqual(gate.events[0]['uri'], 'https://new/')

    def test_partial_line_and_other_view(self):
        gate = NavigationGate('https://a/')
        gate.feed('PAGE_EVENT ms=1 view=0x1 event=0 uri=https://a/')
        self.assertIsNone(gate.view)
        gate.feed('\r\nPAGE_EVENT ms=2 view=0x2 event=3 uri=https://b/\n')
        self.assertEqual(gate.view, '0x1')
        self.assertFalse(gate.complete)
        gate.feed('PAGE_EVENT ms=3 view=0x1 event=3 uri=https://a/\n')
        self.assertTrue(gate.complete)
        self.assertEqual([e['event'] for e in gate.events], [0, 3])

    def test_redirect_and_later_navigation(self):
        gate = NavigationGate('https://a/')
        gate.feed('PAGE_EVENT ms=1 view=0x1 event=0 uri=https://a/\n'
                  'PAGE_EVENT ms=2 view=0x1 event=1 uri=https://redirect/\n'
                  'PAGE_EVENT ms=3 view=0x1 event=3 uri=https://redirect/\n'
                  'PAGE_EVENT ms=4 view=0x1 event=0 uri=https://a/\n')
        self.assertTrue(gate.complete)
        self.assertEqual(len(gate.events), 3)

    def test_ignore_unrelated_and_malformed(self):
        gate = NavigationGate('https://a/')
        gate.feed('PAGE_EVENT ms=1 view=0x1 event=3 uri=https://a/\n'
                  'PAGE_EVENT ms=2 view=0x2 event=0 uri=https://b/\n'
                  'PAGE_EVENT broken\n')
        self.assertIsNone(gate.view)
        self.assertEqual(gate.events, [])

    def test_qmp_opens_only_after_start_and_stops_after_completion(self):
        log = MagicMock()
        log.read.side_effect = ['', 'PAGE_EVENT ms=1 view=0x1 event=0 uri=https://a/\n',
                                'PAGE_EVENT ms=2 view=0x1 event=3 uri=https://a/\n']
        qmp = MagicMock()
        qmp.execute.side_effect = [[{'cpu-index': 0}], 'RIP=ffffffff80000000']

        def connect(_):
            self.assertEqual(log.read.call_count, 2)
            return qmp

        argv = ['probe', '--qmp', 'unused', '--out', 'unused.json',
                '--page-log', 'unused.log', '--page-uri', 'https://a/']
        with patch('sys.argv', argv), patch('qemu_page_load_samples.Path.exists', return_value=False), \
                patch('qemu_page_load_samples.Path.open', return_value=log), \
                patch('qemu_page_load_samples.Path.write_text') as output, \
                patch('qemu_page_load_samples.QMP', side_effect=connect), \
                patch('qemu_page_load_samples.time.sleep'), contextlib.redirect_stdout(io.StringIO()):
            main()
        data = json.loads(output.call_args.args[0])
        self.assertEqual(data['stop_reason'], 'load_event_3')
        self.assertEqual(len(data['samples']), 1)
        qmp.close.assert_called_once()

    def test_optional_post_load_sampling_is_bounded(self):
        log = MagicMock()
        log.read.side_effect = itertools.chain([
            'PAGE_EVENT ms=1 view=0x1 event=0 uri=https://a/\n',
            'PAGE_EVENT ms=2 view=0x1 event=3 uri=https://a/\n'], itertools.repeat(''))
        qmp = MagicMock()
        qmp.execute.side_effect = lambda name, *args: ([{'cpu-index': 0}]
            if name == 'query-cpus-fast' else 'RIP=ffffffff80000000')
        argv = ['probe', '--qmp', 'unused', '--out', 'unused.json',
                '--page-log', 'unused.log', '--page-uri', 'https://a/',
                '--post-load-seconds', '3']
        with patch('sys.argv', argv), patch('qemu_page_load_samples.Path.exists', return_value=False), \
                patch('qemu_page_load_samples.Path.open', return_value=log), \
                patch('qemu_page_load_samples.Path.write_text') as output, \
                patch('qemu_page_load_samples.QMP', return_value=qmp), \
                patch('qemu_page_load_samples.time.monotonic', side_effect=itertools.count(step=.1)), \
                patch('qemu_page_load_samples.time.sleep'), contextlib.redirect_stdout(io.StringIO()):
            main()
        data = json.loads(output.call_args.args[0])
        self.assertEqual(data['stop_reason'], 'post_load_complete')
        self.assertGreater(len(data['samples']), 1)
        self.assertGreater(data['load_finished_elapsed'], 0)
        self.assertLess(data['samples'][-1]['elapsed'] - data['load_finished_elapsed'], 3.5)
        qmp.close.assert_called_once()


if __name__ == '__main__':
    unittest.main()
