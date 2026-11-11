"""Real-file atomic publication, failure cleanup and recovery contracts."""

import contextlib
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import atomic_file


class AtomicFileTests(unittest.TestCase):
    def test_complete_empty_and_nested_publication_preserves_siblings(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sibling = root / "unrelated"
            sibling.write_bytes(b"unrelated bytes")
            target = root / "nested" / "state"
            for payload in (b"complete state", b"", bytes(range(256))):
                atomic_file.atomic_write_bytes(target, payload)
                self.assertEqual(target.read_bytes(), payload)
                self.assertEqual(sibling.read_bytes(), b"unrelated bytes")
                self.assertEqual(list(target.parent.iterdir()), [target])

    def test_acquisition_write_short_write_flush_and_replace_failures_recover(self):
        for existing in (False, True):
            for failure in ("create", "write", "short", "flush", "sync", "replace"):
                with self.subTest(existing=existing, failure=failure), tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    target = root / "state"
                    if existing:
                        target.write_bytes(b"previous complete state")
                    pending = None
                    with contextlib.ExitStack() as stack:
                        if failure in ("write", "short", "flush"):
                            pending = stack.enter_context(tempfile.NamedTemporaryFile(dir=root, delete=False))
                            stack.enter_context(
                                patch.object(atomic_file.tempfile, "NamedTemporaryFile", return_value=pending)
                            )
                            if failure == "short":
                                stack.enter_context(patch.object(pending, "write", return_value=1))
                            else:
                                stack.enter_context(
                                    patch.object(pending, failure, side_effect=OSError("controlled failure"))
                                )
                        elif failure == "create":
                            stack.enter_context(
                                patch.object(
                                    atomic_file.tempfile,
                                    "NamedTemporaryFile",
                                    side_effect=OSError("controlled failure"),
                                )
                            )
                        else:
                            operation = "fsync" if failure == "sync" else "replace"
                            stack.enter_context(
                                patch.object(atomic_file.os, operation, side_effect=OSError("controlled failure"))
                            )
                        with self.assertRaises(OSError):
                            atomic_file.atomic_write_bytes(target, b"replacement state")
                        if pending is not None:
                            self.assertTrue(pending.closed)
                    self.assertEqual(target.exists(), existing)
                    if existing:
                        self.assertEqual(target.read_bytes(), b"previous complete state")
                    self.assertEqual(list(root.iterdir()), [target] if existing else [])
                    atomic_file.atomic_write_bytes(target, b"replacement state")
                    self.assertEqual(target.read_bytes(), b"replacement state")
                    self.assertEqual(list(root.iterdir()), [target])


if __name__ == "__main__":
    unittest.main()
