"""Real-checkout regressions for content-based native input provenance."""

from __future__ import annotations

import hashlib
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import native_build_provenance as provenance
from tools.native_build_provenance import canonical, capture_inputs, file_digest, input_path


class NativeBuildInputsTests(unittest.TestCase):
    # Purpose: Create a private Git fixture without credentials, hooks or global configuration.
    # Inputs: None. Outputs: Minimal checkout and cleanup owner, isolated from the live repository.
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="flitzzip native inputs ")
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
        with tempfile.TemporaryDirectory(prefix="flitzzip relocated inputs ") as directory:
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

    # Purpose: Reject linked, nonregular and oversized inputs instead of hashing content outside the declared snapshot.
    # Inputs: Private NTFS junction, directory and a small file exceeding an explicitly lowered admission bound.
    # Outputs: Refusal leaves the real source untouched; the junction is removed without following its target.
    @unittest.skipUnless(os.name == "nt", "Windows junction semantics")
    def test_link_directory_and_extent_admission(self):
        alias = self.root / "src/alias"
        script = self.root / "junction-fixture.ps1"
        script.write_text(
            "param([string]$LinkPath,[string]$TargetPath)\n"
            "New-Item -ItemType Junction -Path $LinkPath -Value $TargetPath -ErrorAction Stop | Out-Null\n"
        )
        subprocess.run(
            [
                "powershell.exe",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(script),
                str(alias),
                str(self.root / "docs"),
            ],
            check=True,
            capture_output=True,
            timeout=10,
        )
        try:
            with self.assertRaisesRegex(ValueError, "links are not accepted"):
                input_path(self.root, "src/alias")
        finally:
            os.rmdir(alias)
        with self.assertRaisesRegex(ValueError, "bounded regular file"):
            input_path(self.root, "src")
        with patch.object(provenance, "MAX_INPUT_BYTES", 4), self.assertRaisesRegex(ValueError, "bounded regular file"):
            input_path(self.root, "src/main.cpp")
        self.assertTrue((self.root / "docs").is_dir())

    # Purpose: Refuse a digest computed while the owned file is replaced rather than accepting a mixed snapshot.
    # Inputs: A real file extended immediately after the hashing reader opens it.
    # Outputs: The actual size/mtime transition invalidates the result; a clean recapture uses the new complete bytes.
    def test_source_change_during_hash_invalidates_snapshot(self):
        path = self.root / "src/main.cpp"
        original_open = Path.open

        # Purpose: Inject a real concurrent write at the filesystem-open boundary.
        # Inputs: The requested path and open arguments. Outputs: Original reader after one bounded append.
        def opened(selected, *args, **kwargs):
            stream = original_open(selected, *args, **kwargs)
            if selected == path:
                with original_open(path, "ab") as writer:
                    writer.write(b"\nchanged after pre-hash stat")
            return stream

        with patch.object(Path, "open", opened), self.assertRaisesRegex(ValueError, "changed while hashing"):
            file_digest(path)
        self.assertEqual(file_digest(path), hashlib.sha256(path.read_bytes()).hexdigest())

    # Purpose: Bound actual failing external inventory processes and prove they cannot survive a refusal.
    # Inputs: Real Python child processes standing in for command failure, excess stdout and a missed deadline.
    # Outputs: Bounded diagnostics, cancelled/joined timer and reaped owned child for every failure.
    def test_inventory_process_failure_output_limit_and_deadline(self):
        real_popen = subprocess.Popen
        real_timer = threading.Timer
        cases = (
            ("import sys; print('inventory failure'); sys.exit(7)", "Git native inventory failed", False),
            (
                "import sys,time; sys.stdout.buffer.write(b'x'*(4*1024**2+8192)); sys.stdout.flush(); time.sleep(20)",
                "metadata bound",
                False,
            ),
            ("import time; time.sleep(20)", "thirty seconds", True),
        )
        for program, message, deadline in cases:
            children = []
            timers = []

            # Purpose: Retain an actual owned process while replacing only the external Git operation.
            # Inputs: Production argv and pipe options. Outputs: One bounded fault-producing child.
            def launch(arguments, selected_program=program, owned_children=children, **kwargs):
                self.assertIn("core.fsmonitor=false", arguments)
                self.assertIn("core.untrackedCache=false", arguments)
                child = real_popen([sys.executable, "-B", "-c", selected_program], **kwargs)
                owned_children.append(child)
                return child

            # Purpose: Accelerate only the test deadline without changing the production thirty-second policy.
            # Inputs: Production delay/callback. Outputs: A real cancellable timer with the controlled delay.
            def timer(seconds, callback, accelerated=deadline, owned_timers=timers):
                self.assertEqual(seconds, 30)
                value = real_timer(0.15 if accelerated else seconds, callback)
                owned_timers.append(value)
                return value

            with (
                self.subTest(message=message),
                patch.object(provenance.subprocess, "Popen", side_effect=launch),
                patch.object(provenance.threading, "Timer", side_effect=timer),
                self.assertRaisesRegex((ValueError, TimeoutError), message),
            ):
                provenance.git_inventory(self.root)
            self.assertEqual(len(children), 1)
            self.assertIsNotNone(children[0].returncode)
            self.assertFalse(any(value.is_alive() for value in timers))

    # Purpose: Reject missing roots, growing inventories and a changing source set independently from file hashes.
    # Inputs: Controlled inventory responses at the Git boundary; the real fixture contents remain unchanged.
    # Outputs: No incomplete manifest is exported and a later clean capture remains valid.
    def test_inventory_admission_and_capture_race(self):
        for output in (b"", b"src/main.cpp\0", b"CMakeLists.txt\0src/main.cpp\0extra.cpp\0"):
            with (
                self.subTest(output=output),
                patch.object(provenance, "git_inventory", return_value=output),
                patch.object(provenance, "MAX_INPUT_COUNT", 2),
                self.assertRaisesRegex(ValueError, "incomplete or too large"),
            ):
                provenance.native_input_names(self.root)
        with (
            patch.object(provenance, "native_input_names", side_effect=[["CMakeLists.txt"], ["CMakeLists.txt", "new"]]),
            self.assertRaisesRegex(ValueError, "inventory changed"),
        ):
            capture_inputs(self.root)
        self.assertIn("src/main.cpp", capture_inputs(self.root)["files"])

    # Purpose: Exercise the actual provenance CLI and verify its portable content binding against independent hashes.
    # Inputs: Private checkout and real current interpreter. Outputs: Complete canonical JSON and stable native scope.
    def test_provenance_command_line(self):
        process = subprocess.run(
            [sys.executable, "-B", "-m", "tools.native_build_provenance", "--root", str(self.root)],
            cwd=Path(provenance.__file__).resolve().parents[1],
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(process.stdout.encode(), canonical(capture_inputs(self.root)) + b"\n")


if __name__ == "__main__":
    unittest.main()
