"""Focused safety tests for the bounded external archive benchmark runner."""

from __future__ import annotations

import bz2
import copy
import hashlib
import json
import tempfile
import unittest
from pathlib import Path, PureWindowsPath
from types import SimpleNamespace
from unittest.mock import patch

from tools import run_archive_comparison as comparison


class ComparisonRunnerTests(unittest.TestCase):
    # Purpose: Verify replacement-corpus provenance, permissions and actual bytes before any product execution.
    # Inputs: An owned temporary input and manifest with a mocked reviewed permission decision.
    # Outputs: Exact input/cases pass; malformed metadata, wrong bytes and denied permissions fail.
    def test_licensed_manifest(self) -> None:
        payload = b"owned synthetic CSV fixture\n"
        source = "https://archive.ics.uci.edu/dataset/280/higgs"
        spec = {
            "schema_version": 1,
            "name": "owned offline fixture, not benchmark data",
            "files": [
                {
                    "name": "sample.csv",
                    "bytes": len(payload),
                    "sha256": hashlib.sha256(payload).hexdigest(),
                    "subject": "HIGGS",
                    "source_url": source,
                    "transformation": "owned test fixture, no downloaded data",
                }
            ],
            "cases": [{"name": "numeric", "format": "zst", "files": ["sample.csv"], "tools": ["SuperZip", "Zstd"]}],
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            corpus = root / "raw"
            corpus.mkdir()
            raw = corpus / "sample.csv"
            raw.write_bytes(payload)
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps(spec), encoding="utf-8")
            with patch.object(comparison, "require_permission", return_value={"evidence": [source]}) as permission:
                files, cases, metadata = comparison.verify_manifest(manifest, corpus)
                self.assertEqual(files, spec["files"])
                self.assertEqual(cases, (("numeric", "zst", ("sample.csv",), ("SuperZip", "Zstd")),))
                self.assertEqual(metadata["manifest_sha256"], comparison.digest(manifest))
                permission.assert_called_once_with("HIGGS", "execute")
                for field, value in (
                    ("name", "../escape"),
                    ("name", "-switch"),
                    ("name", "NUL.csv"),
                    ("name", "COM1.txt"),
                    ("name", "trailing."),
                    ("bytes", True),
                    ("bytes", 65 * 1024**2),
                    ("sha256", "bad"),
                    ("source_url", "https://unreviewed.invalid/data"),
                    ("transformation", ""),
                ):
                    modified = copy.deepcopy(spec)
                    modified["files"][0][field] = value
                    manifest.write_text(json.dumps(modified), encoding="utf-8")
                    with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                        comparison.verify_manifest(manifest, corpus)
                for invalid in (
                    {**spec, "schema_version": True},
                    {**spec, "cases": []},
                    {**spec, "unexpected": True},
                    {**spec, "files": [spec["files"][0], spec["files"][0]]},
                    {**spec, "cases": spec["cases"] * 2},
                ):
                    manifest.write_text(json.dumps(invalid), encoding="utf-8")
                    with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                        comparison.verify_manifest(manifest, corpus)
                manifest.write_text(json.dumps(spec), encoding="utf-8")
                raw.write_bytes(b"x" * len(payload))
                with self.assertRaisesRegex(ValueError, "SHA-256 differs"):
                    comparison.verify_manifest(manifest, corpus)
            with (
                patch.object(comparison, "require_permission", side_effect=ValueError("permission denied")),
                self.assertRaisesRegex(ValueError, "permission denied"),
            ):
                comparison.verify_manifest(manifest, corpus)

    # Purpose: Reject case mutations that change the measurement or exceed the filesystem budget.
    # Inputs: Incompatible formats/tools, repeated inputs and oversized aggregate cases.
    # Outputs: No unsupported case reaches a subprocess; valid ZIP and Zstd pairings normalize exactly.
    def test_manifest_case_boundaries(self) -> None:
        files = {"first": {"bytes": 33 * 1024**2}, "second": {"bytes": 33 * 1024**2}}
        row = {"name": "files", "format": "zip", "files": ["first"], "tools": ["SuperZip", "7-Zip"]}
        self.assertEqual(comparison.manifest_case(row, files), ("files", "zip", ("first",), ("SuperZip", "7-Zip")))
        for field, value in (
            ("name", "../escape"),
            ("format", "suzip"),
            ("format", None),
            ("files", ["missing"]),
            ("files", ["first", "first"]),
            ("files", ["first", "second"]),
            ("files", [False]),
            ("tools", ["SuperZip", "SuperZip"]),
            ("tools", ["7-Zip", "SuperZip"]),
            ("tools", ["SuperZip", "WinZip"]),
        ):
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                comparison.manifest_case({**row, field: value}, files)
        with self.assertRaises(ValueError):
            comparison.manifest_case(
                {**row, "format": "zst", "files": ["first", "second"], "tools": ["SuperZip", "Zstd"]}, files
            )

    # Purpose: Preserve all nine efforts and reject malformed values before starting a process.
    # Inputs: Each supported software/format pairing and valid or invalid effort values.
    # Outputs: Exact effort switches pass; non-integers and out-of-range efforts fail.
    def test_all_effort_commands(self) -> None:
        for level in range(1, 10):
            for _, fmt, files, tools in comparison.CASES:
                for tool in tools:
                    with self.subTest(level=level, tool=tool, fmt=fmt):
                        create, extract = comparison.command_templates(tool, fmt, files, level)
                        switch = {"SuperZip": str(level), "7-Zip": f"-mx={level}", "Zstd": f"-{level}"}[tool]
                        self.assertIn(switch, create)
                        self.assertTrue(extract)
        for level in (0, 10, -1, True, False, 5.0, "5", None):
            with self.subTest(level=level), self.assertRaisesRegex(ValueError, "unsupported comparison effort"):
                comparison.command_templates("SuperZip", "zip", ("sample",), level)

    # Purpose: Require a real total-processor counter rather than a nullable per-CPU CIM field.
    # Inputs: Simulated Windows performance-counter output, then a missing total-load value.
    # Outputs: A valid snapshot passes and the missing counter fails closed.
    def test_host_snapshot_requires_total_processor_load(self) -> None:
        sample = {
            "cpu_model": "Test CPU",
            "cpu_cores": 8,
            "cpu_threads": 16,
            "gpu_model": "Test GPU",
            "gpu_driver": "1",
            "os_name": "Windows",
            "os_build": "1",
            "ram_bytes": 16 * 1024**3,
            "ram_modules": 2,
            "ram_configured_mts": 5200,
            "storage_model": "Test SSD",
            "cpu_load_percent": 2,
            "free_ram_bytes": 8 * 1024**3,
            "paging_pages_per_second": 0,
            "disk_busy_percent": 0,
            "gpu_engine_max_percent": 0,
            "workspace_bus_type": "NVMe",
        }
        with (
            patch.object(
                comparison.subprocess,
                "run",
                return_value=SimpleNamespace(returncode=0, stdout=json.dumps(sample), stderr=""),
            ) as command,
            patch.object(comparison, "ROOT", PureWindowsPath("C:/SuperZip")),
        ):
            self.assertEqual(comparison.host_snapshot()["cpu_load_percent"], 2)
            self.assertIn("Win32_PerfFormattedData_PerfOS_Processor", command.call_args.args[0][-1])
            self.assertIn("Get-Partition -DriveLetter 'C'", command.call_args.args[0][-1])
        sample["cpu_load_percent"] = None
        with (
            patch.object(
                comparison.subprocess,
                "run",
                return_value=SimpleNamespace(returncode=0, stdout=json.dumps(sample), stderr=""),
            ),
            patch.object(comparison, "ROOT", PureWindowsPath("C:/SuperZip")),
            self.assertRaisesRegex(RuntimeError, "missing required fields"),
        ):
            comparison.host_snapshot()

    # Purpose: Accept only an exact corpus payload with the pinned SHA-256 digest.
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
            expected = {"sample": (path.stat().st_size, hashlib.sha256(b"known corpus").hexdigest())}
            with patch.dict(comparison.EXPECTED, expected, clear=True):
                self.assertEqual(comparison.verify_corpus(raw, downloads)[0]["name"], "sample")
                (downloads / "sample.bz2").write_bytes(bz2.compress(b"wrong corpus"))
                with self.assertRaisesRegex(ValueError, "download differs"):
                    comparison.verify_corpus(raw, downloads)
                (downloads / "sample.bz2").write_bytes(bz2.compress(b"known corpus"))
                path.write_bytes(b"wrong corpus")
                with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
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
            unexpected_directory = target / "unexpected-empty-directory"
            unexpected_directory.mkdir()
            with self.assertRaisesRegex(RuntimeError, "paths differ"):
                comparison.verify_tree(target, ("sample",), manifest)
            unexpected_directory.rmdir()
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
