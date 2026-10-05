"""Whole-database C++ security admission and real Git range recurrence contracts."""

import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools import cpp_security_plan as cpp


# Purpose: Prove changed native/query inputs and periodic scans cannot silently skip C++ analysis.
# Inputs: Actual native receipt projection, workflow and isolated real Git histories.
# Outputs: Admission, rejection, output and rename/delete controls pass without compilation or scanning.
class CppSecurityContracts(unittest.TestCase):
    def test_every_native_input_root_and_file_selects_whole_database(self):
        """Purpose: Preserve input coverage. Inputs: Actual receipt projection. Outputs: Every input admits CodeQL."""
        for name in [*cpp.INPUT_FILES, *(root + "/future.input" for root in cpp.INPUT_ROOTS)]:
            with self.subTest(path=name):
                self.assertTrue(cpp.select_cpp([name])["codeql_cpp"])
                self.assertTrue(cpp.select_cpp([name])["whole_database"])

    def test_independent_push_jobs_and_query_changes(self):
        """Purpose: Separate execution roles. Inputs: Passive and policy changes. Outputs: Correct C++ admission."""
        for name in [
            "README.md",
            "docs/security-evidence.md",
            "tools/requirements/crawl4ai.txt",
            "tools/test_binary_corpus_transport.ps1",
        ]:
            self.assertFalse(cpp.select_cpp([name])["codeql_cpp"])
        for name in [
            *cpp.POLICY_FILES,
            ".github/codeql/codeql-config.yml",
            ".github/actions/future/action.yml",
            "fuzz/future.cpp",
            ".clusterfuzzlite/build.sh",
            "future/input.cpp",
            "SRC/core/future.cpp",
        ]:
            self.assertTrue(cpp.select_cpp([name])["codeql_cpp"])
        self.assertTrue(cpp.select_cpp(["README.md", "src/core/future.cpp"])["codeql_cpp"])

    def test_broad_events_and_initial_push(self):
        """Purpose: Retain full scans. Inputs: Scheduled/manual/new-branch events. Outputs: Unconditional admission."""
        for event in ["schedule", "workflow_dispatch"]:
            self.assertTrue(cpp.event_plan(event, "")["codeql_cpp"])
            with self.assertRaises(ValueError):
                cpp.event_plan(event, "0" * 40)
        self.assertTrue(cpp.event_plan("push", "0" * 40)["codeql_cpp"])
        for base in ["", "HEAD", "$(command)", "a" * 39, "A" * 40]:
            with self.assertRaises(ValueError):
                cpp.event_plan("push", base)

    def test_invalid_path_cannot_skip_analysis(self):
        """Purpose: Reject ambiguous inventories. Inputs: Traversal and aliases. Outputs: Failure, never skip."""
        for name in ["", "../src/a.cpp", "/src/a.cpp", "src\\a.cpp", "src/./a.cpp", "src//a.cpp", "C:source.cpp"]:
            with self.assertRaises(ValueError):
                cpp.select_cpp([name])
        with mock.patch.object(cpp, "MAX_PATH_BYTES", 8), self.assertRaises(ValueError):
            cpp.read_paths(io.BytesIO(b"012345678"))

    def test_real_git_delete_rename_and_missing_history(self):
        """Purpose: Bind complete ranges. Inputs: Real Git mutations. Outputs: Deleted/renamed source still selects."""
        with tempfile.TemporaryDirectory(prefix="superzip-cpp-plan-") as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "--quiet", str(root)], check=True)
            for key, value in [("user.name", "Contract Fixture"), ("user.email", "fixture@example.invalid")]:
                subprocess.run(["git", "-C", directory, "config", key, value], check=True)
            (root / "src").mkdir()
            (root / "src/original.cpp").write_text("source fixture\n", encoding="utf-8")
            subprocess.run(["git", "-C", directory, "add", "src/original.cpp"], check=True)
            subprocess.run(["git", "-C", directory, "commit", "--quiet", "-m", "Source fixture"], check=True)
            base = subprocess.check_output(["git", "-C", directory, "rev-parse", "HEAD"], text=True).strip()
            self.assertFalse(cpp.event_plan("push", base, root)["codeql_cpp"])
            (root / "README.md").write_text("source fixture\n", encoding="utf-8")
            subprocess.run(["git", "-C", directory, "rm", "--quiet", "src/original.cpp"], check=True)
            subprocess.run(["git", "-C", directory, "add", "README.md"], check=True)
            subprocess.run(["git", "-C", directory, "commit", "--quiet", "-m", "Rename source fixture"], check=True)
            self.assertTrue(cpp.event_plan("push", base, root)["codeql_cpp"])
            with self.assertRaisesRegex(ValueError, "Git range failed"):
                cpp.event_plan("push", "f" * 40, root)

    def test_output_and_workflow_enforce_the_actual_selector(self):
        """Purpose: Check the consumer. Inputs: CLI/output and workflow. Outputs: Required gate and full queries."""
        with tempfile.TemporaryDirectory(prefix="superzip-cpp-output-") as directory:
            output = Path(directory) / "job-output"
            with mock.patch.object(sys, "argv", ["cpp-plan", "--event", "schedule", "--github-output", str(output)]):
                cpp.main()
            self.assertEqual(output.read_bytes(), b"codeql_cpp=true\n")
        workflow = (cpp.ROOT / ".github/workflows/security-code-scanning.yml").read_text(encoding="utf-8")
        for required in [
            "needs: cpp-security-plan",
            "needs.cpp-security-plan.result != 'success'",
            "needs.cpp-security-plan.outputs.codeql_cpp != 'false'",
            "codeql_cpp: ${{ steps.cpp_plan.outputs.codeql_cpp }}",
            "python -B -m tools.cpp_security_plan",
            "fetch-depth: 0",
            "queries: security-extended,security-and-quality",
            "build-mode: manual",
        ]:
            self.assertIn(required, workflow)
        self.assertNotIn("paths-ignore", workflow)
        self.assertNotIn("continue-on-error", workflow)


if __name__ == "__main__":
    unittest.main()
