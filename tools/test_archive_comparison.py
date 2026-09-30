"""Focused safety tests for the bounded external archive benchmark runner."""

from __future__ import annotations

import bz2
import hashlib
import json
import tempfile
import unittest
from pathlib import Path, PureWindowsPath
from types import SimpleNamespace
from unittest.mock import patch

from tools import run_archive_comparison as comparison


class ComparisonRunnerTests(unittest.TestCase):
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
