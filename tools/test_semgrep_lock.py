"""Artifact binding rejects drift and preserves independently framed lock bytes."""

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import atomic_file, semgrep_lock


class SemgrepLockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = Path(__file__).resolve().parents[1] / ".github/requirements/semgrep-distribution.json"
        cls.manifest = json.loads(path.read_text(encoding="utf-8"))
        cls.version = cls.manifest["version"]
        cls.hashes = sorted({pin["derived_sha256"] for pin in cls.manifest["wheels"].values()})
        cls.prefix = "--index-url https://pypi.org/simple\n--only-binary :all:\n\n"
        cls.prefix += "alpha[crypto]==1.0 \\\n    --hash=sha256:" + "1" * 64 + "\n# retain this comment\n"
        cls.suffix = "\nomega==2.0 \\\n    --hash=sha256:" + "2" * 64 + "\n"
        lines = [f"semgrep=={cls.version} \\"]
        lines.extend(
            f"    --hash=sha256:{value}" + (" \\" if i + 1 < len(cls.hashes) else "")
            for i, value in enumerate(cls.hashes)
        )
        cls.expected = cls.prefix + "\n".join(lines) + "\n" + cls.suffix

    def candidate(self, hashes=None, version=None, one_line=False):
        values = self.hashes[:1] if hashes is None else hashes
        head = f"semgrep=={version or self.version}"
        separator = " " if one_line else " \\\n    "
        return (
            self.prefix + head + separator + separator.join("--hash=sha256:" + v for v in values) + "\n" + self.suffix
        )

    def test_each_platform_projection_binds_all_reviewed_artifacts_and_is_idempotent(self):
        for value in self.hashes:
            for one_line in (False, True):
                with self.subTest(hash=value, one_line=one_line):
                    actual = semgrep_lock.bind_artifact_hashes(self.candidate([value], one_line=one_line))
                    self.assertEqual(actual, self.expected)
                    self.assertEqual(semgrep_lock.bind_artifact_hashes(actual), actual)

    def test_foreign_duplicate_missing_and_malformed_inputs_are_rejected(self):
        cases = [
            self.candidate(["f" * 64]),
            self.candidate([self.hashes[0]] * 2),
            self.candidate(version="0.0.0"),
            self.candidate([]),
            self.candidate().replace("semgrep==", "unrelated=="),
            self.candidate() + self.candidate(),
            self.candidate().replace("https://pypi.org/simple", "https://example.invalid/simple"),
            self.candidate().replace("alpha[crypto]==1.0", "alpha[crypto]>=1.0"),
            self.candidate().replace("semgrep==", "semgrep[extra]=="),
        ]
        for contents in cases:
            with self.subTest(contents=contents[:90]), self.assertRaises(ValueError):
                semgrep_lock.bind_artifact_hashes(contents)

    def test_check_mode_never_writes_and_real_completion_recovers(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "requirements.txt"
            original = self.candidate().encode()
            target.write_bytes(original)
            with self.assertRaisesRegex(ValueError, "needs reviewed artifact binding"):
                semgrep_lock.finalize_lock(target, check_only=True)
            self.assertEqual(target.read_bytes(), original)
            with (
                patch.object(atomic_file.os, "replace", side_effect=OSError("controlled publication failure")),
                self.assertRaisesRegex(OSError, "controlled publication failure"),
            ):
                semgrep_lock.finalize_lock(target)
            self.assertEqual(target.read_bytes(), original)
            self.assertEqual(list(target.parent.iterdir()), [target])
            self.assertTrue(semgrep_lock.finalize_lock(target))
            self.assertEqual(target.read_text(), self.expected)
            self.assertFalse(semgrep_lock.finalize_lock(target, check_only=True))
            self.assertFalse(semgrep_lock.finalize_lock(target))

    def test_observed_concurrent_edit_is_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "requirements.txt"
            original = self.candidate().encode()
            edited = original + b"# another writer's complete edit\n"
            target.write_bytes(original)
            real_read = semgrep_lock.read_lock_bytes
            reads = []

            def read_then_edit(path):
                result = real_read(path)
                if not reads:
                    target.write_bytes(edited)
                reads.append(True)
                return result

            with (
                patch.object(semgrep_lock, "read_lock_bytes", side_effect=read_then_edit),
                self.assertRaisesRegex(ValueError, "changed during"),
            ):
                semgrep_lock.finalize_lock(target)
            self.assertEqual(target.read_bytes(), edited)
            self.assertEqual(list(target.parent.iterdir()), [target])

    def test_crlf_invalid_encoding_oversize_and_nonfile_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "requirements.txt"
            target.write_bytes(self.candidate().replace("\n", "\r\n").encode())
            self.assertTrue(semgrep_lock.finalize_lock(target))
            self.assertEqual(target.read_bytes(), self.expected.encode())
            for invalid in (b"\xff", b"X" * (semgrep_lock.MAX_LOCK_BYTES + 1)):
                target.write_bytes(invalid)
                with self.assertRaises((ValueError, UnicodeError)):
                    semgrep_lock.finalize_lock(target)
                self.assertEqual(target.read_bytes(), invalid)
            for path in (root / "absent", root):
                with self.assertRaisesRegex(ValueError, "regular source file"):
                    semgrep_lock.finalize_lock(path)


if __name__ == "__main__":
    unittest.main()
