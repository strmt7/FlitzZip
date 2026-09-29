"""Focused safety tests for the bounded external archive benchmark runner."""

from __future__ import annotations

import bz2
import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import run_archive_comparison as comparison


class ComparisonRunnerTests(unittest.TestCase):
    # Purpose: Accept only an exact corpus payload with the author's hash.
    # Inputs: One temporary file and patched small-file manifest.
    # Outputs: Matching bytes pass; a one-byte edit fails.
    def test_corpus_hash_gate(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            raw = Path(temporary) / "raw"
            downloads = Path(temporary) / "downloads"
            raw.mkdir()
            downloads.mkdir()
            path = raw / "sample"
            path.write_bytes(b"known corpus")
            (downloads / "sample.bz2").write_bytes(bz2.compress(b"known corpus"))
            expected = {"sample": (path.stat().st_size, hashlib.md5(b"known corpus").hexdigest())}
            with patch.dict(comparison.EXPECTED, expected, clear=True):
                self.assertEqual(comparison.verify_corpus(raw, downloads)[0]["name"], "sample")
                path.write_bytes(b"wrong corpus")
                with self.assertRaisesRegex(ValueError, "MD5 mismatch"):
                    comparison.verify_corpus(raw, downloads)

    # Purpose: Keep independent extraction output and cleanup inside owned paths.
    # Inputs: One expected file plus an unrelated outside file.
    # Outputs: Exact tree passes; extra content and outside deletion are refused.
    def test_tree_and_cleanup_boundary(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            owned = root / "owned"
            owned.mkdir()
            target = owned / "decoded"
            target.mkdir()
            (target / "sample").write_bytes(b"abc")
            manifest = {"sample": {"bytes": 3, "sha256": hashlib.sha256(b"abc").hexdigest()}}
            comparison.verify_tree(target, ("sample",), manifest)
            (target / "extra").write_bytes(b"x")
            with self.assertRaisesRegex(RuntimeError, "paths differ"):
                comparison.verify_tree(target, ("sample",), manifest)
            outside = root / "unrelated"
            outside.write_bytes(b"leave intact")
            with self.assertRaisesRegex(RuntimeError, "outside"):
                comparison.discard(outside, owned)
            self.assertEqual(outside.read_bytes(), b"leave intact")


if __name__ == "__main__":
    unittest.main()
