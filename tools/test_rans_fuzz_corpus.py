"""Independent wire contracts for the sanitizer corpus, not an entropy encoder."""

import struct
import tempfile
import unittest
import zipfile
from pathlib import Path

from tools.rans_fuzz_corpus import seed_cases, write_corpus


class RansFuzzCorpusTests(unittest.TestCase):
    def test_fixed_singleton_rows_offsets_and_full_width_states(self):
        seeds = seed_cases()
        expected = (
            struct.pack("<I", 513)
            + bytes((16, 8, 8, 1, 0, 0, 255, 255, 0, 255))
            + struct.pack("<7I", 0, 4, 8, 12, 8388608, 8388608, 8388608)
        )
        self.assertEqual(seeds["singleton-full-width-contexts"], expected)
        self.assertEqual(len(seeds["singleton-unconditional"]), 31)
        self.assertEqual(struct.unpack_from("<I", seeds["singleton-segment-tail"])[0], 4097)

    def test_balanced_binary_has_exact_independent_rows_and_stream(self):
        data = seed_cases()["binary-balanced"]
        self.assertEqual(data[:16], bytes((0, 1, 0, 0, 8, 0, 8, 0, 0, 1, 0, 128, 0, 255, 128, 0)))
        self.assertEqual(struct.unpack_from("<III", data, 16), (0, 36, 8388608))
        self.assertEqual(data[28:], bytes(32))

    def test_corruptions_change_only_the_named_wire_boundary(self):
        seeds = seed_cases()
        ordinary = seeds["singleton-unconditional"]
        self.assertEqual(seeds["bad-terminal-state"][:-4], ordinary[:-4])
        self.assertEqual(struct.unpack("<I", seeds["bad-terminal-state"][-4:])[0], 8388609)
        self.assertEqual(seeds["bad-dense-offset"][:15], ordinary[:15])
        self.assertEqual(seeds["bad-dense-offset"][19:], ordinary[19:])
        self.assertEqual(seeds["bad-model-mass"][:14], seeds["binary-balanced"][:14])
        self.assertEqual(seeds["bad-model-mass"][16:], seeds["binary-balanced"][16:])
        self.assertEqual(seeds["truncated-stream"], seeds["singleton-full-width-contexts"][:-1])
        self.assertEqual(seeds["missing-used-context"][5], 8)

    def test_generated_archive_is_reproducible_and_complete(self):
        with tempfile.TemporaryDirectory() as temporary:
            first, second = (Path(temporary) / name for name in ("first.zip", "second.zip"))
            write_corpus(first)
            write_corpus(second)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            with zipfile.ZipFile(first) as archive:
                self.assertEqual(set(archive.namelist()), set(seed_cases()))
                for name, data in seed_cases().items():
                    self.assertEqual(archive.read(name), data)
                    self.assertEqual(archive.getinfo(name).date_time, (1980, 1, 1, 0, 0, 0))
                    self.assertEqual(archive.getinfo(name).create_system, 3)
                    self.assertEqual(archive.getinfo(name).external_attr, 0o100600 << 16)
                    self.assertEqual(archive.getinfo(name).compress_type, zipfile.ZIP_STORED)


if __name__ == "__main__":
    unittest.main()
