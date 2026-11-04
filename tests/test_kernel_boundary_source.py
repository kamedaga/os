#!/usr/bin/env python3
"""Regression coverage for source masking and mutation-census memoization."""
import unittest

from check_kernel_address_boundaries import code_only, function_body, without_tests
from check_kernel_boot_scratch_boundaries import prepare_source, reference_count


class BoundarySourceTests(unittest.TestCase):
    def test_masks_nested_and_adjacent_blocks_without_moving_code(self):
        first = 'test { if (ok) { work(); } }'
        second = 'test\n{ next(); }'
        source = 'prefix; ' + first + second + '\nfn runtime() {}'
        self.assertEqual(
            without_tests(source),
            'prefix; ' + ' ' * (len(first) + len(second)) + '\nfn runtime() {}',
        )

    def test_keeps_identifiers_and_test_declarations_without_bodies(self):
        source = 'contest { x(); } test_fn { y(); } fn test() {}'
        self.assertEqual(without_tests(source), source)

    def test_ignores_comment_string_and_character_braces(self):
        source = '''test "fixture {" { const c = '}'; // }
            const s = "}"; if (ok) { work(); } }
fn runtime() {}'''
        clean = code_only(source)
        end = clean.index('\nfn runtime')
        masked = without_tests(clean)
        self.assertEqual(masked, ' ' * end + clean[end:])
        self.assertEqual(without_tests(masked), masked)

    def test_rejects_unterminated_blocks(self):
        for source in ('test {', 'test { nested { }', 'test {} test {'):
            with self.subTest(source=source):
                with self.assertRaisesRegex(ValueError, '^unterminated test block$'):
                    without_tests(source)

    def test_function_index_retains_first_declaration_and_exact_name(self):
        source = 'fn ab() { second(); } struct { fn a() { first(); } } fn a() { later(); }'
        self.assertEqual(function_body(source, 'a'), ' first(); ')
        self.assertEqual(function_body(source, 'ab'), ' second(); ')
        # Raw callers deliberately keep the old lexical treatment of comments.
        self.assertEqual(function_body('// fn a() { comment(); }\n' + source, 'a'), ' comment(); ')

    def test_declaration_tail_uses_its_first_body(self):
        for name in ('f', '日本語', '@"odd-name"'):
            with self.subTest(name=name):
                source = f'fn {name}() {{ first(); }} fn {name}() {{ later(); }}'
                self.assertEqual(function_body(source, name), ' first(); ')

    def test_function_index_preserves_return_error_sets_and_quoted_names(self):
        source = 'fn f(p: struct { n: u8 }) error{Oops}!void { return error.Oops; }'
        self.assertEqual(function_body(source, 'f'), ' return error.Oops; ')
        self.assertEqual(function_body('fn @"odd-name"() { quoted(); }', '@"odd-name"'), ' quoted(); ')
        self.assertEqual(function_body('fn 日本語() { word(); }', '日本語'), ' word(); ')

    def test_function_index_preserves_errors_and_changed_source_offsets(self):
        for source, name, error in (
                ('fn f(', 'f', 'unterminated parameters f'),
                ('fn f() error{Oops', 'f', 'unterminated return error set f'),
                ('fn f() {', 'f', 'unterminated function f'),
                ('fn other() {}', 'f', 'missing migrated function f')):
            with self.subTest(source=source):
                with self.assertRaises(ValueError) as caught:
                    function_body(source, name)
                self.assertEqual(str(caught.exception), error)
        original = 'fn f() { old(); }'
        changed = 'const prefix = 123; fn f() { new(); }'
        self.assertEqual(function_body(original, 'f'), ' old(); ')
        self.assertEqual(function_body(changed, 'f'), ' new(); ')
        self.assertEqual(function_body(original, 'f'), ' old(); ')

    def test_cache_separates_source_changes_and_scope(self):
        source = 'test "fixture" { alias.identityBytes(); }'
        self.assertNotIn('identityBytes', prepare_source('kernel/src/pci.zig', source))
        self.assertIn('identityBytes', prepare_source('kernel/src/other.zig', source))
        original = prepare_source('kernel/src/pci.zig', 'const a = 1;')
        changed = prepare_source('kernel/src/pci.zig', 'const a = 2;')
        self.assertNotEqual(original, changed)
        self.assertEqual(original, prepare_source('kernel/src/pci.zig', 'const a = 1;'))

    def test_cached_reference_census_detects_added_alias(self):
        source = 'fn releaseScratch() {} alias.releaseScratch();'
        self.assertEqual(reference_count(source, 'releaseScratch'), 1)
        self.assertEqual(reference_count(source + ' const indirect = alias.releaseScratch;', 'releaseScratch'), 2)
        self.assertEqual(reference_count(source, 'releaseScratch'), 1)
        self.assertEqual(reference_count(source, 'withdrawScratch'), 0)


if __name__ == '__main__':
    unittest.main()
