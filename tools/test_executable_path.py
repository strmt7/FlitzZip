"""Executable lookup tests with real path admission and a public Git consumer."""

import contextlib
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.executable_path import executable_path
from tools.semgrep_coverage import tracked_paths


class ExecutablePathTests(unittest.TestCase):
    def setUp(self):
        """Purpose: Isolate executable fixtures. Inputs: None. Outputs: Owned temporary directories."""
        directory = tempfile.TemporaryDirectory(prefix="executable-lookup-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.trusted = self.root / "tools with spaces"
        self.trusted.mkdir()
        self.work = self.root / "work"
        self.work.mkdir()

    @staticmethod
    def program(directory: Path, name: str = "probe") -> Path:
        """Purpose: Create an unexecuted lookup fixture. Inputs: Directory/name. Outputs: Executable regular path."""
        file = directory / (name + (".exe" if os.name == "nt" else ""))
        file.write_bytes(b"lookup fixture; not executable machine code\n")
        file.chmod(file.stat().st_mode | stat.S_IXUSR)
        return file

    def test_absolute_path_and_search_order(self):
        """Purpose: Preserve explicit selection and PATH order. Inputs: Two real files. Outputs: Exact absolute path."""
        first = self.program(self.trusted)
        second = self.program(self.work)
        self.assertEqual(executable_path(str(second), search_path=str(self.trusted)), str(second))
        self.assertEqual(
            executable_path("probe", search_path=os.pathsep.join((str(self.trusted), str(self.work)))), str(first)
        )
        self.assertEqual(
            executable_path("probe", search_path=os.pathsep.join((str(self.work), str(self.trusted)))), str(second)
        )

    def test_current_directory_and_relative_path_entries_are_not_implicit(self):
        """Purpose: Reject CWD-dependent lookup. Inputs: Shadow/relative/empty PATH. Outputs: Trusted path only."""
        self.program(self.work)
        trusted = self.program(self.trusted)
        relative = self.work / "relative"
        relative.mkdir()
        self.program(relative)
        with contextlib.chdir(self.work):
            for value in ("", ".", "relative", os.pathsep.join(("", ".", "relative"))):
                with self.subTest(path=value), self.assertRaises(FileNotFoundError):
                    executable_path("probe", search_path=value)
            value = os.pathsep.join(("", ".", "relative", str(self.trusted)))
            self.assertEqual(executable_path("probe", search_path=value), str(trusted))

    def test_invalid_command_paths_fail_before_lookup(self):
        """Purpose: Reject ambiguous command paths. Inputs: Relative/root-dependent/NUL names. Outputs: ValueError."""
        cases = ["", ".", "..", "relative/probe", "./probe", "../probe", "probe\0suffix"]
        if os.name == "nt":
            cases.extend((r"relative\probe.exe", r"\probe.exe", "C:probe.exe"))
        for command in cases:
            with self.subTest(command=command), self.assertRaises(ValueError):
                executable_path(command, search_path=str(self.trusted))

    def test_missing_paths_and_directories_fail(self):
        """Purpose: Require a regular executable file. Inputs: Absent names/directories. Outputs: No admission."""
        name = "probe.exe" if os.name == "nt" else "probe"
        directory = self.trusted / name
        directory.mkdir()
        for command in ("absent", "probe", str(directory), str(self.trusted / "missing")):
            with self.subTest(command=command), self.assertRaises(FileNotFoundError):
                executable_path(command, search_path=str(self.trusted))

    def test_explicit_empty_path_does_not_use_environment(self):
        """Purpose: Preserve explicit empty overrides. Inputs: Populated ambient PATH. Outputs: Empty search fails."""
        program = self.program(self.trusted)
        with patch.dict(os.environ, {"PATH": str(self.trusted)}):
            self.assertEqual(executable_path("probe"), str(program))
            with self.assertRaises(FileNotFoundError):
                executable_path("probe", search_path="")

    @unittest.skipIf(os.name == "nt", "POSIX execute permissions")
    def test_nonexecutable_file_is_skipped(self):
        """Purpose: Honor POSIX execute admission. Inputs: Nonexecuting first candidate. Outputs: Later executable."""
        first = self.program(self.trusted)
        first.chmod(stat.S_IRUSR | stat.S_IWUSR)
        second = self.program(self.work)
        self.assertEqual(
            executable_path("probe", search_path=os.pathsep.join((str(self.trusted), str(self.work)))), str(second)
        )

    @unittest.skipUnless(os.name == "nt", "Windows native executable suffixes")
    def test_command_scripts_and_pathext_cannot_redirect_native_selection(self):
        """Purpose: Avoid implicit shell execution. Inputs: CMD/BAT shadows and PATHEXT. Outputs: Native EXE only."""
        program = self.program(self.trusted)
        for extension in (".cmd", ".bat", ".ps1"):
            file = self.work / ("probe" + extension)
            file.write_text("never execute this fixture", encoding="utf-8")
            with self.subTest(extension=extension), self.assertRaises(ValueError):
                executable_path(str(file))
        with patch.dict(os.environ, {"PATHEXT": ".CMD;.BAT;.PS1;.EXE"}):
            path = os.pathsep.join((str(self.work), str(self.trusted)))
            self.assertEqual(executable_path("probe", search_path=path), str(program))
            self.assertEqual(executable_path("probe.EXE", search_path=path), str(program.parent / "probe.EXE"))

    def test_git_consumer_ignores_working_directory_shadow(self):
        """Purpose: Exercise Git inventory despite a CWD shadow. Inputs: Real repository. Outputs: Exact names."""
        git = executable_path("git")
        subprocess.run([git, "init", "--quiet", str(self.work)], check=True, timeout=15)
        (self.work / "tracked.cpp").write_text("// Independent tracked inventory fixture.\n", encoding="utf-8")
        subprocess.run([git, "-C", str(self.work), "add", "--", "tracked.cpp"], check=True, timeout=15)
        shadow = self.program(self.work, "git")
        shadow_bytes = shadow.read_bytes()
        with contextlib.chdir(self.work), patch.dict(os.environ, {"PATH": str(Path(git).parent)}):
            self.assertEqual(tracked_paths(self.work), ["tracked.cpp"])
        self.assertEqual(shadow.read_bytes(), shadow_bytes)

    def test_direct_cli_entrypoints_work_outside_checkout(self):
        """Purpose: Preserve direct entrypoints. Inputs: External CWD/empty PYTHONPATH. Outputs: Real CLI help."""
        source = Path(__file__).resolve().parent
        for name in ("redact_trufflehog", "semgrep_coverage", "cocoindex_agent_search"):
            process = subprocess.run(
                [sys.executable, "-B", str(source / (name + ".py")), "--help"],
                cwd=self.work,
                env=dict(os.environ, PYTHONPATH=""),
                check=True,
                capture_output=True,
                text=True,
                timeout=15,
            )
            self.assertIn("usage:", process.stdout)


if __name__ == "__main__":
    unittest.main()
