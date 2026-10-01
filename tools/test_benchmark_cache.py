"""Offline regression tests for comparator measurement cache integrity and invalidation."""

from __future__ import annotations

import copy
import json
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

    # Purpose: Invalidate every material input without coupling competitor measurements to SuperZip edits.
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
            cache.measurement_id({**self.identity, "superzip_commit": "must-not-affect-competitors"})

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


if __name__ == "__main__":
    unittest.main()
