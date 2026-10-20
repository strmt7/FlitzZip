"""Offline regression tests for comparator measurement cache integrity and invalidation."""

from __future__ import annotations

import contextlib
import copy
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import benchmark_cache as cache


class BenchmarkCacheTests(unittest.TestCase):
    # Purpose: Build owned metadata fixtures without running compression or external tools.
    # Inputs: None; every hash and timing is synthetic test data.
    # Outputs: A complete identity/result pair for offline validation only.
    def setUp(self) -> None:
        self.identity = {
            "methodology_sha256": "a" * 64,
            "corpus_sha256": "b" * 64,
            "corpus_subject": "HIGGS",
            "tool": "Zstd",
            "tool_version": "1.5.7",
            "binary_sha256": "c" * 64,
            "binary_dependencies_sha256": {},
            "extract_reference_sha256": "f" * 64,
            "scope": {"edition": None, "modules": []},
            "settings": {"format": "zst", "level": 5, "timer": "whole-command", "cache": "warm"},
            "host": {"cpu_model": "owned fixture", "ram_bytes": 1024},
        }
        self.result = {
            "tool": "Zstd",
            "measured_at_utc": "2026-10-01T00:00:00+00:00",
            "independent_verified": True,
            "extract_reference_sha256": "f" * 64,
            "contention_review": "accepted",
            "resource_context": {"before": {"cpu_load_percent": 0}, "after": {"cpu_load_percent": 0}},
            "compress_seconds": [2.0] * 5,
            "extract_seconds": [2.0] * 5,
            "archive_bytes": [100] * 5,
            "archive_sha256": ["d" * 64] * 5,
        }

    # Purpose: Invalidate every material input without coupling competitor measurements to FlitzZip edits.
    # Inputs: Identity variants and reordered equivalent dictionaries.
    # Outputs: Equivalent JSON keeps its key; changes to method, input, tool, settings or host change it.
    def test_identity_invalidation(self) -> None:
        key = cache.measurement_id(self.identity)
        self.assertEqual(key, cache.measurement_id(dict(reversed(list(self.identity.items())))))
        for field in self.identity:
            changed = copy.deepcopy(self.identity)
            changed[field] = {"changed": True} if isinstance(changed[field], dict) else "e" * 64
            if field == "scope":
                changed[field] = {"edition": "Standard", "modules": []}
            elif field == "binary_dependencies_sha256":
                changed[field] = {"decoder.dll": "e" * 64}
            with self.subTest(field=field):
                self.assertNotEqual(key, cache.measurement_id(changed))
        with self.assertRaises(ValueError):
            cache.measurement_id({**self.identity, "flitzzip_commit": "must-not-affect-competitors"})

    # Purpose: Reject unreviewed, malformed and undersampled records without altering evidence.
    # Inputs: Result variants covering booleans, NaN, invalid dates and missing correctness/resources.
    # Outputs: The original passes; every invalid variant raises ValueError.
    def test_invalid_results(self) -> None:
        cache.validate_result(self.identity, self.result)
        invalid = {
            "compress_seconds": [True] * 5,
            "extract_seconds": [1.0] * 4,
            "archive_bytes": [True] * 5,
            "archive_sha256": ["wrong"] * 5,
            "independent_verified": False,
            "extract_reference_sha256": "e" * 64,
            "contention_review": "unreviewed",
            "resource_context": {},
            "measured_at_utc": "2026-10-01T00:00:00",
        }
        for field, value in invalid.items():
            with self.subTest(field=field), self.assertRaises(ValueError):
                cache.validate_result(self.identity, {**self.result, field: value})
        with self.assertRaises(ValueError):
            cache.measurement_id({**self.identity, "host": {"load": float("nan")}})

    # Purpose: Bind reuse to decoder dependencies, edition/module scope and exact reader input.
    # Inputs: Invalid dependency/scope metadata and a changed extraction-reference identity.
    # Outputs: Malformed scopes fail; different reference bytes miss the old cached entry.
    def test_scope_and_reader_identity(self) -> None:
        variants = (
            {"binary_dependencies_sha256": {"../decoder.dll": "a" * 64}},
            {"binary_dependencies_sha256": {"decoder.dll": True}},
            {"scope": {"edition": None}},
            {"scope": {"edition": "", "modules": []}},
            {"scope": {"edition": None, "modules": ["zstd", "lz4"]}},
            {"scope": {"edition": None, "modules": ["lz4", "lz4"]}},
            {"scope": {"edition": None, "modules": [False]}},
        )
        for fields in variants:
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                cache.measurement_id({**self.identity, **fields})
        with tempfile.TemporaryDirectory() as temporary, patch.object(cache, "ROOT", Path(temporary)):
            directory = Path(temporary) / "out" / "benchmarks" / "cache"
            cache.store_result(directory, self.identity, self.result)
            changed = {**self.identity, "extract_reference_sha256": "e" * 64}
            self.assertIsNone(cache.lookup(directory, changed))

    # Purpose: Preserve original dates, detect damaged cache bytes, and refuse replacement.
    # Inputs: A temporary ignored-output tree with synthetic metadata only.
    # Outputs: Miss/store/reuse succeed; collisions and tampered samples fail explicitly.
    def test_storage_and_corruption(self) -> None:
        with tempfile.TemporaryDirectory() as temporary, patch.object(cache, "ROOT", Path(temporary)):
            directory = Path(temporary) / "out" / "benchmarks" / "cache"
            self.assertIsNone(cache.lookup(directory, self.identity))
            path = cache.store_result(directory, self.identity, self.result)
            self.assertEqual(cache.lookup(directory, self.identity), self.result)
            with self.assertRaises(FileExistsError):
                cache.store_result(directory, self.identity, self.result)
            entry = json.loads(path.read_text(encoding="utf-8"))
            entry["result"]["compress_seconds"][0] = 1.0
            path.write_text(json.dumps(entry), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "checksum"):
                cache.lookup(directory, self.identity)
            with self.assertRaisesRegex(ValueError, "beneath"):
                cache.lookup(Path(temporary) / "elsewhere", self.identity)

    # Purpose: Enforce current tool and corpus permission checks even when cached bytes already exist.
    # Inputs: Mocked metadata-only release discovery and explicit cache CLI arguments.
    # Outputs: Stale releases and denied permissions cannot reach result lookup.
    def test_cli_rechecks_eligibility(self) -> None:
        with (
            patch.object(cache.sys, "argv", ["cache", "--identity", "owned.json"]),
            patch.object(cache, "read_json", return_value=self.identity),
            patch.object(cache, "fetch_release_source", return_value="owned fixture"),
            patch.object(cache, "parse_release", return_value="1.5.8"),
            patch.object(cache, "lookup") as lookup,
        ):
            with self.assertRaisesRegex(ValueError, "not the current stable"):
                cache.main()
            lookup.assert_not_called()
        with (
            patch.object(cache.sys, "argv", ["cache", "--identity", "owned.json"]),
            patch.object(cache, "read_json", return_value=self.identity),
            patch.object(cache, "fetch_release_source", return_value="owned fixture"),
            patch.object(cache, "parse_release", return_value="1.5.7"),
            patch.object(cache, "require_permission", side_effect=ValueError("permission denied")),
            patch.object(cache, "lookup") as lookup,
        ):
            with self.assertRaisesRegex(ValueError, "permission denied"):
                cache.main()
            lookup.assert_not_called()

    # Purpose: Reject structurally ambiguous cache metadata even when one interpretation has a valid result hash.
    # Inputs: Duplicate properties at either depth and a boolean in place of the integer schema version.
    # Outputs: Each malformed record is refused rather than silently selecting its final interpretation.
    def test_duplicate_metadata_and_boolean_schema_cannot_qualify_cache(self) -> None:
        with tempfile.TemporaryDirectory() as temporary, patch.object(cache, "ROOT", Path(temporary)):
            directory = Path(temporary) / "out/benchmarks/cache"
            path = cache.store_result(directory, self.identity, self.result)
            original = path.read_bytes()
            entry = json.loads(original)
            entry["schema_version"] = True
            path.write_text(json.dumps(entry))
            with self.subTest(case="boolean schema"), self.assertRaisesRegex(ValueError, "schema"):
                cache.lookup(directory, self.identity)
            for payload in (b'{"version":1,"version":2}', b'{"nested":{"tool":"first","tool":"second"}}'):
                path.write_bytes(payload)
                with self.subTest(payload=payload), self.assertRaisesRegex(ValueError, "duplicate"):
                    cache.read_json(path)

    # Purpose: Keep malformed identity, correctness and sample data from entering evidence storage.
    # Inputs: Independent one-field type/scope mutations; no benchmark or external comparator runs.
    # Outputs: Admission rejects each record without creating a cache entry.
    def test_identity_and_result_type_boundaries(self) -> None:
        identities = (
            {"binary_sha256": False},
            {"tool": " "},
            {"tool_version": 1},
            {"corpus_subject": ""},
            {"settings": []},
            {"host": {}},
            {"binary_dependencies_sha256": []},
            {"binary_dependencies_sha256": {"decoder.dll": "uppercase-not-a-hash"}},
        )
        for changes in identities:
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                cache.measurement_id({**self.identity, **changes})
        for result in (
            None,
            {**self.result, "tool": "different"},
            {**self.result, "measured_at_utc": ""},
            {**self.result, "measured_at_utc": False},
        ):
            with self.subTest(result=result), self.assertRaises(ValueError):
                cache.validate_result(self.identity, result)
        for field in ("compress_seconds", "extract_seconds"):
            for value in (None, [], [0] * 5, [-1] * 5, [float("inf")] * 5, [float("nan")] * 5):
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    cache.validate_result(self.identity, {**self.result, field: value})

    # Purpose: Enforce complete metadata and byte bounds while preserving existing evidence on a failed store.
    # Inputs: Missing/directory/oversized JSON, oversized result and mismatched cache envelope.
    # Outputs: Explicit failures, no replacement and no cache output on rejected data.
    def test_json_and_record_bounds_preserve_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary, patch.object(cache, "ROOT", Path(temporary)):
            root = Path(temporary)
            for path in (root, root / "missing.json"):
                with self.subTest(path=path.name), self.assertRaisesRegex(ValueError, "regular file"):
                    cache.read_json(path)
            path = root / "oversized.json"
            path.write_bytes(b" " * (cache.MAX_RECORD_BYTES + 1))
            with self.assertRaisesRegex(ValueError, "exceeds one MiB"):
                cache.read_json(path)
            directory = root / "out/benchmarks/cache"
            result = {**self.result, "resource_context": {"large": "x" * cache.MAX_RECORD_BYTES}}
            with self.assertRaisesRegex(ValueError, "exceeds one MiB"):
                cache.store_result(directory, self.identity, result)
            self.assertFalse(directory.exists())
            path = cache.store_result(directory, self.identity, self.result)
            entry = json.loads(path.read_text())
            entry["identity"]["tool_version"] = "different"
            path.write_text(json.dumps(entry))
            with self.assertRaisesRegex(ValueError, "schema or identity differs"):
                cache.lookup(directory, self.identity)

    # Purpose: Stop cache publication through a redirected directory before creating or replacing caller data.
    # Inputs: A real private Windows junction from the allowed output tree to an outside directory.
    # Outputs: Link traversal is refused and the target remains empty; cleanup removes only the junction itself.
    @unittest.skipUnless(os.name == "nt", "Windows junction semantics")
    def test_junction_cache_ancestry_is_rejected_before_writes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary, patch.object(cache, "ROOT", Path(temporary)):
            root = Path(temporary)
            target = root / "outside"
            target.mkdir()
            directory = root / "out/benchmarks"
            directory.mkdir(parents=True)
            link = directory / "redirected"
            script = root / "junction.ps1"
            script.write_text(
                "param([string]$Link,[string]$Target)\n"
                "New-Item -ItemType Junction -Path $Link -Target $Target -ErrorAction Stop | Out-Null\n"
            )
            subprocess.run(
                [
                    "powershell.exe",
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(script),
                    str(link),
                    str(target),
                ],
                check=True,
                capture_output=True,
                timeout=10,
            )
            try:
                with self.assertRaisesRegex(ValueError, "must not traverse links or junctions"):
                    cache.store_result(link / "nested", self.identity, self.result)
                self.assertEqual(list(target.iterdir()), [])
            finally:
                os.rmdir(link)

    # Purpose: Exercise cache miss, store and read through the command handler with actual temporary evidence files.
    # Inputs: Metadata-only discovery and reviewed permission boundary observations.
    # Outputs: Exact original result, original collection date and permission checks for every operation.
    def test_command_line_store_lookup_and_miss(self) -> None:
        with tempfile.TemporaryDirectory() as temporary, patch.object(cache, "ROOT", Path(temporary)):
            root = Path(temporary)
            identity = root / "identity.json"
            result = root / "result.json"
            identity.write_text(json.dumps(self.identity))
            result.write_text(json.dumps(self.result))
            arguments = ["cache", "--identity", str(identity), "--cache-root", str(root / "out/benchmarks/cache")]
            with (
                patch.object(cache, "fetch_release_source", return_value="owned fixture"),
                patch.object(cache, "parse_release", return_value="1.5.7"),
                patch.object(cache, "require_permission") as permission,
            ):
                for options, expected in (([], 2), (["--result", str(result)], 0), ([], 0)):
                    stdout, stderr = io.StringIO(), io.StringIO()
                    with (
                        patch.object(cache.sys, "argv", [*arguments, *options]),
                        contextlib.redirect_stdout(stdout),
                        contextlib.redirect_stderr(stderr),
                    ):
                        self.assertEqual(cache.main(), expected)
                    if expected == 2:
                        self.assertEqual(stderr.getvalue(), "measurement cache miss\n")
                    elif not options:
                        self.assertEqual(json.loads(stdout.getvalue()), self.result)
                    else:
                        self.assertTrue(Path(stdout.getvalue().strip()).is_file())
                    self.assertEqual(permission.call_args_list[-2].args, ("HIGGS", "publish_results"))
                    self.assertEqual(permission.call_args.kwargs, {"version": "1.5.7", "edition": None, "modules": ()})
                self.assertEqual(permission.call_count, 6)

    # Purpose: Verify the process-level failure contract without network access or installed comparator tools.
    # Inputs: Malformed identity file rejected before discovery. Outputs: Nonzero status and actionable stderr only.
    def test_cli_invalid_identity_fails_before_external_discovery(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            identity = Path(temporary) / "bad.json"
            identity.write_text("{}")
            process = subprocess.run(
                [sys.executable, "-B", "-m", "tools.benchmark_cache", "--identity", str(identity)],
                cwd=Path(cache.__file__).resolve().parents[1],
                capture_output=True,
                text=True,
                timeout=10,
            )
        self.assertEqual(process.returncode, 1)
        self.assertEqual(process.stdout, "")
        self.assertIn("measurement identity fields are missing or unknown", process.stderr)


if __name__ == "__main__":
    unittest.main()
