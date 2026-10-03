"""Real-checkout regressions for content-based native input provenance."""

from __future__ import annotations

import hashlib
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.native_build_provenance import canonical, capture_inputs, file_digest, input_path


class NativeBuildInputsTests(unittest.TestCase):
    # Purpose: Create a private Git fixture without credentials, hooks or global configuration.
    # Inputs: None. Outputs: Minimal checkout and cleanup owner, isolated from the live repository.
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="superzip native inputs ")
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        subprocess.run(["git", "-C", str(self.root), "init", "--quiet"], check=True, capture_output=True, timeout=30)
        (self.root / "src").mkdir()
        (self.root / "docs").mkdir()
        (self.root / "CMakeLists.txt").write_text("project(Fixture)\n", encoding="utf-8")
        (self.root / "src/main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
        subprocess.run(
            ["git", "-C", str(self.root), "add", "--", "CMakeLists.txt", "src"],
            check=True,
            capture_output=True,
            timeout=30,
        )

    # Purpose: Prove deterministic hashes across captures and checkout relocation.
    # Inputs: Equivalent fixture file bytes at another path. Outputs: Requires identical path-free manifests.
    def test_content_identity_survives_relocation(self):
        first = capture_inputs(self.root)
        self.assertEqual(first, capture_inputs(self.root))
        with tempfile.TemporaryDirectory(prefix="superzip relocated inputs ") as directory:
            relocated = Path(directory)
            subprocess.run(
                ["git", "-C", str(relocated), "init", "--quiet"], check=True, capture_output=True, timeout=30
            )
            for name in first["files"]:
                target = relocated / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((self.root / name).read_bytes())
            self.assertEqual(first, capture_inputs(relocated))
        self.assertNotIn(str(self.root), canonical(first).decode("ascii"))
        expected = hashlib.sha256((self.root / "src/main.cpp").read_bytes()).hexdigest()
        self.assertEqual(first["files"]["src/main.cpp"], expected)

    # Purpose: Detect unstaged and staged edits while retaining unchanged document-only identity.
    # Inputs: Real file mutations and Git staging. Outputs: Requires byte-sensitive native digest changes only.
    def test_changes_are_content_based(self):
        original = capture_inputs(self.root)
        (self.root / "docs/new.md").write_text("Documentation only.\n", encoding="utf-8")
        self.assertEqual(original, capture_inputs(self.root))
        (self.root / "src/main.cpp").write_text("int main() { return 1; }\n", encoding="utf-8")
        changed = capture_inputs(self.root)
        self.assertNotEqual(original["inputs_sha256"], changed["inputs_sha256"])
        subprocess.run(
            ["git", "-C", str(self.root), "add", "--", "src/main.cpp"], check=True, capture_output=True, timeout=30
        )
        self.assertEqual(changed, capture_inputs(self.root))
        (self.root / "src/main.cpp").write_text("int main() { return 2; }\n", encoding="utf-8")
        self.assertNotEqual(changed["inputs_sha256"], capture_inputs(self.root)["inputs_sha256"])

    # Purpose: Preserve nonignored additions and reject deleted tracked inputs.
    # Inputs: Real native source addition/deletion. Outputs: Requires inventory growth and failed missing-file capture.
    def test_inventory_changes(self):
        original = capture_inputs(self.root)
        (self.root / "src/new.cpp").write_text("int additional = 1;\n", encoding="utf-8")
        self.assertIn("src/new.cpp", capture_inputs(self.root)["files"])
        self.assertNotEqual(original["inputs_sha256"], capture_inputs(self.root)["inputs_sha256"])
        (self.root / "src/main.cpp").unlink()
        with self.assertRaises(FileNotFoundError):
            capture_inputs(self.root)

    # Purpose: Reject unsafe or noncanonical names without opening outside the fixture.
    # Inputs: Traversal, absolute, alias and Windows separator forms. Outputs: Requires path rejection.
    def test_unsafe_input_paths(self):
        for name in ("", "../secret", "/absolute", "C:/absolute", "src\\main.cpp", "src//main.cpp", "src/./main.cpp"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                input_path(self.root, name)

    # Purpose: Exercise streaming hashing over multiple chunks and an uneven tail.
    # Inputs: Bounded binary fixture. Outputs: Requires an independent full-content SHA-256 match.
    def test_streamed_file_hash(self):
        content = bytes(range(256)) * 8193 + b"tail"
        path = self.root / "src/bounded.bin"
        path.write_bytes(content)
        self.assertEqual(file_digest(input_path(self.root, "src/bounded.bin")), hashlib.sha256(content).hexdigest())


if __name__ == "__main__":
    unittest.main()
