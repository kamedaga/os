import unittest
from kernel_boot_scratch_snapshot import sealed_records


class SnapshotTests(unittest.TestCase):
    def header(self):
        return dict(previous=2**64-1, end=48, length=8, id=4,
                    lifetime=1, published=1, borrowed=0)

    def test_empty(self):
        self.assertEqual([], sealed_records(0, 4096, 40, 1, lambda _: self.fail()))

    def test_published_chain(self):
        headers = {0: self.header(), 48: dict(previous=0, end=104, length=9, id=5,
                                             lifetime=1, published=1, borrowed=0)}
        self.assertEqual(2, len(sealed_records(104, 4096, 40, 1, headers.__getitem__)))

    def test_rejects_corrupt_or_unfinished_record(self):
        for field, value in (('end', 40), ('end', 56), ('end', 47),
                             ('length', 0), ('length', 9), ('id', 0),
                             ('previous', 0), ('lifetime', 0),
                             ('published', 0), ('borrowed', 1)):
            with self.subTest(field=field, value=value):
                header = self.header()
                header[field] = value
                with self.assertRaises(ValueError):
                    sealed_records(48, 4096, 40, 1, lambda _: header)

    def test_extent_rejected_before_read(self):
        for used in (-1, 4097, 8):
            with self.assertRaises(ValueError):
                sealed_records(used, 4096, 40, 1, lambda _: self.fail())


if __name__ == '__main__':
    unittest.main()
