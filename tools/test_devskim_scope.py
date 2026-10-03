"""Fail-closed contracts for passive report separation without excluding code or tests."""

import hashlib
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from urllib.parse import unquote

from tools import devskim_scope as scope


class DevSkimScopeTests(unittest.TestCase):
    # Purpose: Isolate scope mutation fixtures. Inputs: None. Outputs: Owned data directory with automatic cleanup.
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.data = self.root / scope.REPORT_DIRECTORY
        self.data.mkdir(parents=True)

    # Purpose: Admit passive record roles independently of dates. Inputs: Three producer kinds. Outputs: Exact paths.
    def test_report_roles_and_exact_directory_scope(self):
        for index, kind in enumerate(sorted(scope.REPORT_KINDS)):
            (self.data / f"report-{index}.json").write_text(
                json.dumps({"schema_version": 1, "benchmark_kind": kind, "cases": []}), encoding="utf-8"
            )
        paths = scope.validate_report_directory(self.root)
        self.assertEqual(len(paths), 3)
        globs = scope.ignore_globs(self.root)
        self.assertEqual(globs, ["**/.git/**", self.data.as_posix() + "/**"])
        for glob in globs:
            self.assertNotIn("tests/", glob)
            self.assertNotIn("src/", glob)
            self.assertNotIn("corpora/", glob)

    # Purpose: Preserve literal checkout names and refuse options-file replacement.
    # Inputs: Checkout containing spaces, commas and glob operators.
    # Outputs: Root-anchored scope with no rule overrides.
    def test_literal_checkout_and_options_collision(self):
        root = self.root / "checkout [scope], (owned)"
        (root / scope.REPORT_DIRECTORY).mkdir(parents=True)
        output = self.root / "options.json"
        scope.write_options(root, output)
        options = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(set(options), {"Globs"})
        self.assertEqual(len(options["Globs"]), 2)
        self.assertIn("checkout ?scope?? (owned)", options["Globs"][1])
        with self.assertRaises(FileExistsError):
            scope.write_options(root, output)

    # Purpose: Exercise the pinned scanner's actual matcher, including checkout punctuation and neighboring code.
    # Inputs: Explicit provisioned DevSkim executable and controlled fixture bytes.
    # Outputs: Complete retained finding paths.
    @unittest.skipUnless(os.environ.get("SUPERZIP_DEVSKIM"), "Actual scanner requires the complete pinned tool")
    def test_actual_scanner_directory_boundary(self):
        root = self.root / "checkout [scope], (owned)"
        root.mkdir()
        digest = hashlib.sha256(b"noncredential scanner scope boundary fixture").hexdigest()
        expected = {
            "src/sample.cpp",
            "tests/test_sample.cpp",
            "docs/benchmarks/corpora/source-pin.json",
            ".github/config.json",
            "src/docs/benchmarks/data/nested.cpp",
            "bin/helper.cpp",
        }
        for name in expected:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            payload = (
                json.dumps({"source_sha256": digest}) if path.suffix == ".json" else f'const char* id = "{digest}";'
            )
            path.write_text(payload, encoding="utf-8")
        report_path = root / scope.REPORT_DIRECTORY / "future/report.json"
        report_path.parent.mkdir(parents=True)
        report_path.write_text(
            json.dumps({"schema_version": 1, "benchmark_kind": "suzip_ram", "runs": [], "source_sha256": digest}),
            encoding="utf-8",
        )
        options = self.root / "scope.json"
        scope.write_options(root, options)
        for scoped in (False, True):
            output = self.root / ("scoped.sarif" if scoped else "control.sarif")
            arguments = [
                os.environ["SUPERZIP_DEVSKIM"],
                "analyze",
                "-I",
                str(root),
                "--base-path",
                str(root),
                "-O",
                str(output),
                "-f",
                "sarif",
                "--disable-supression",
                "--skip-excerpts",
                "--disable-console",
            ]
            arguments += ["--options-json", str(options)] if scoped else ["-g", "**/.git/**"]
            result = subprocess.run(arguments, check=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0)
            self.assertLessEqual(output.stat().st_size, 1024 * 1024)
            report = json.loads(output.read_text(encoding="utf-8-sig"))
            paths = {
                unquote(row["locations"][0]["physicalLocation"]["artifactLocation"]["uri"]).replace("\\", "/")
                for run in report["runs"]
                for row in run["results"]
            }
            wanted = expected if scoped else expected | {"docs/benchmarks/data/future/report.json"}
            self.assertEqual(paths, wanted)

    # Purpose: Prevent source, fixtures and download configuration from entering the excluded role.
    # Inputs: Deliberately misplaced files and unknown JSON roles. Outputs: Every placement fails admission.
    def test_code_tests_and_configuration_refused(self):
        for name, payload in (
            ("test_parser.cpp", "void fixture() {}"),
            ("helper.py", "print('source')"),
            ("source-pin.json", '{"schema_version":1,"download_url":"https://example.invalid/source"}'),
            ("fixture.json", '{"schema_version":1,"benchmark_kind":"test_fixture","cases":[]}'),
            ("source-pin.json", '{"schema_version":1,"benchmark_kind":"suzip_ram","runs":[],"download_url":"x"}'),
        ):
            path = self.data / name
            path.write_text(payload, encoding="utf-8")
            with self.subTest(name=name), self.assertRaises(ValueError):
                scope.ignore_globs(self.root)
            path.unlink()

    # Purpose: Refuse malformed or oversized records and redirected directories before applying an exclusion.
    # Inputs: Ambiguous JSON, nonfinite data, small admission limits and a junction fixture. Outputs: Failures.
    def test_decoder_and_resource_boundaries(self):
        path = self.data / "record.json"
        for payload in ('{"schema_version":1,"schema_version":1}', "[NaN]", '{"schema_version":true}'):
            path.write_text(payload, encoding="utf-8")
            with self.assertRaises(ValueError):
                scope.validate_report_directory(self.root)
        path.write_text('{"schema_version":1,"benchmark_kind":"suzip_ram","runs":[]}', encoding="utf-8")
        with patch.object(scope, "MAX_BYTES", 1), self.assertRaises(ValueError):
            scope.validate_report_directory(self.root)
        with patch.object(scope, "MAX_ENTRIES", 0), self.assertRaises(ValueError):
            scope.validate_report_directory(self.root)
        with patch.object(Path, "is_junction", return_value=True), self.assertRaises(ValueError):
            scope.validate_report_directory(self.root)

    # Purpose: Keep the real repository and hosted scanner on one enforced input contract.
    # Inputs: Current reports, corpus configuration and workflow. Outputs: Data admission plus shared invocation checks.
    def test_repository_and_hosted_consumer(self):
        root = Path(__file__).resolve().parents[1]
        self.assertTrue(scope.validate_report_directory(root))
        self.assertTrue((root / "docs/benchmarks/corpora/household-power-source.json").is_file())
        self.assertFalse((root / "docs/benchmarks/data/corpora/household-power-source.json").exists())
        workflow = (root / ".github/workflows/security-code-scanning.yml").read_text(encoding="utf-8")
        self.assertIn('python -m tools.devskim_scope --root . --output "$RUNNER_TEMP/devskim-options.json"', workflow)
        self.assertIn(
            '-f sarif --options-json "$RUNNER_TEMP/devskim-options.json" --disable-supression --skip-excerpts',
            workflow,
        )


if __name__ == "__main__":
    unittest.main()
