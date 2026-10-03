"""Offline acquisition safety tests; generated fixtures are never performance evidence."""

from __future__ import annotations

import copy
import hashlib
import io
import json
import tempfile
import unittest
import zipfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from tools import acquire_benchmark_corpus as acquisition
from tools import run_archive_comparison as comparison


# Purpose: Supply small owned ZIP bytes with CRLF rows and a missing-value marker.
# Inputs: None; all source bytes are generated locally and have no real-data or permission claim.
# Outputs: Source specification, ZIP bytes and original literal member for independent slice comparisons.
def fixture_source() -> tuple[dict, bytes, bytes]:
    member = b"header;value\r\n" + b"1;?\r\n" * 32
    archive = io.BytesIO()
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as output:
        output.writestr("sample.txt", member)
    payload = archive.getvalue()
    spec = {
        "schema_version": 1,
        "name": "owned generated correctness fixture",
        "subject": "owned-fixture",
        "source_url": "https://example.invalid/owned-fixture",
        "download_url": "https://example.invalid/owned-fixture.zip",
        "archive_bytes": len(payload),
        "archive_sha256": hashlib.sha256(payload).hexdigest(),
        "member": {"name": "sample.txt", "bytes": len(member), "sha256": hashlib.sha256(member).hexdigest()},
        "windows": [
            {"label": "first", "offset": 0, "bytes": 30},
            {"label": "middle", "offset": 60, "bytes": 30},
            {"label": "last", "offset": len(member) - 30, "bytes": 30},
        ],
        "attribution": "Owned generated correctness fixture; no downloaded or benchmark data.",
    }
    return spec, payload, member


class CorpusAcquisitionTests(unittest.TestCase):
    # Purpose: Compare retained ranges with independent literal slices and complete expected CRLF records.
    # Inputs: Generated source with both partial-row and exact-row-boundary windows.
    # Outputs: No padding, row loss at an exact boundary, missing-value change or newline conversion passes.
    def test_literal_complete_rows(self) -> None:
        spec, payload, member = fixture_source()
        rows = acquisition.decode_windows(payload, spec)
        self.assertEqual(rows[0]["payload"], b"header;value\r\n" + b"1;?\r\n" * 3)
        self.assertEqual(rows[1]["payload"], b"1;?\r\n" * 5)
        self.assertEqual(rows[2]["payload"], b"1;?\r\n" * 6)
        for row in rows:
            self.assertEqual(row["payload"], member[row["start"] : row["end"]])
            self.assertEqual(row["sha256"], hashlib.sha256(row["payload"]).hexdigest())
            self.assertEqual(row["line_count"], row["payload"].count(b"\n"))
        with self.assertRaisesRegex(ValueError, "no complete rows"):
            acquisition.trim_rows(bytearray(b"partial"), {"offset": 0})

    # Purpose: Reject malformed or overlapping geometry before source download or allocation.
    # Inputs: Owned source pins with one controlled size, type, name, endpoint or label mutation.
    # Outputs: Every invalid source fails its shared permission and resource admission.
    def test_source_admission(self) -> None:
        spec, _, _ = fixture_source()
        permission = {"evidence": [spec["source_url"], spec["download_url"]]}
        with patch.object(acquisition, "require_permission", return_value=permission):
            self.assertEqual(acquisition.validate_source(spec), spec)
            for field, value in (
                ("schema_version", True),
                ("archive_bytes", True),
                ("archive_sha256", "invalid"),
                ("download_url", "https://unreviewed.invalid/archive.zip"),
            ):
                invalid = copy.deepcopy(spec)
                invalid[field] = value
                with self.subTest(field=field), self.assertRaises(ValueError):
                    acquisition.validate_source(invalid)
            for field, value in (("offset", True), ("offset", -1), ("offset", 15), ("bytes", 0), ("label", "FIRST")):
                invalid = copy.deepcopy(spec)
                invalid["windows"][1][field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    acquisition.validate_source(invalid)
            invalid = copy.deepcopy(spec)
            invalid["member"]["bytes"] = 128 * 1024 * 1024
            invalid["windows"] = [
                {"label": f"row{index}", "offset": index * 32 * 1024 * 1024, "bytes": 32 * 1024 * 1024}
                for index in range(3)
            ]
            with self.assertRaisesRegex(ValueError, "aggregate 64 MiB"):
                acquisition.validate_source(invalid)

    # Purpose: Authenticate both container and complete member, including bytes outside retained excerpts.
    # Inputs: Valid generated ZIP and independent mismatches in each expected hash.
    # Outputs: Wrong container or complete-member identity fails rather than admitting matching excerpts alone.
    def test_container_and_full_member_identity(self) -> None:
        spec, payload, _ = fixture_source()
        for target in (spec, spec["member"]):
            key = "archive_sha256" if target is spec else "sha256"
            original = target[key]
            target[key] = "0" * 64
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                acquisition.decode_windows(payload, spec)
            target[key] = original
        with self.assertRaisesRegex(ValueError, "archive bytes"):
            acquisition.decode_windows(payload[:-1], spec)

    # Purpose: Admit chunked responses and reject overflow, truncation or response metadata contradictions.
    # Inputs: Owned in-memory HTTP doubles; no network endpoint is contacted.
    # Outputs: Exact chunked bytes pass; invalid status/encoding/length and excess/incomplete body fail.
    def test_bounded_chunked_download(self) -> None:
        spec, payload, _ = fixture_source()
        for body, headers, status, accepted in (
            (payload, {}, 200, True),
            (payload, {"Content-Length": str(len(payload))}, 200, True),
            (payload + b"x", {}, 200, False),
            (payload[:-1], {}, 200, False),
            (payload, {"Content-Length": "1"}, 200, False),
            (payload, {"Content-Encoding": "gzip"}, 200, False),
            (payload, {}, 404, False),
        ):
            response = io.BytesIO(body)
            response.headers, response.status = headers, status
            opener = SimpleNamespace(open=lambda request, timeout, response=response: response)
            with patch.object(acquisition, "build_opener", return_value=opener):
                if accepted:
                    self.assertEqual(acquisition.download_archive(spec), payload)
                else:
                    with self.assertRaises(ValueError):
                        acquisition.download_archive(spec)
        response = io.BytesIO(payload)
        response.headers, response.status = {}, 200
        opener = SimpleNamespace(open=lambda request, timeout: response)
        with (
            patch.object(acquisition, "build_opener", return_value=opener),
            patch.object(acquisition.time, "monotonic", side_effect=[0, 1, 181]),
            self.assertRaisesRegex(ValueError, "lifetime boundary at EOF"),
        ):
            acquisition.download_archive(spec)
        with (
            patch.object(acquisition.time, "monotonic", side_effect=[0, 1, 121]),
            self.assertRaisesRegex(ValueError, "lifetime boundary at EOF"),
        ):
            acquisition.decode_windows(payload, spec)

    # Purpose: Exercise publication through the canonical manifest boundary with no overwrite or early network access.
    # Inputs: Owned source pins, mocked downloaded bytes/permissions and one new temporary destination.
    # Outputs: Exact manifest/payload evidence passes; prior destinations, denied rights and changed pins fail.
    def test_publication_and_preflight(self) -> None:
        spec, payload, member = fixture_source()
        permission = {"evidence": [spec["source_url"], spec["download_url"]]}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, destination = root / "source.json", root / "raw"
            source.write_text(json.dumps(spec), encoding="utf-8")
            with (
                patch.object(acquisition, "require_permission", return_value=permission),
                patch.object(comparison, "require_permission", return_value=permission),
                patch.object(acquisition, "download_archive", return_value=payload) as download,
            ):
                record = acquisition.acquire(source, destination)
                self.assertTrue(record["complete_member_crc_verified"])
                for file, selection in zip(record["corpus"]["files"], record["selection"], strict=True):
                    self.assertEqual(
                        (destination / file["name"]).read_bytes(), member[selection["start"] : selection["end"]]
                    )
                with self.assertRaisesRegex(ValueError, "existing or linked"):
                    acquisition.acquire(source, destination)
                download.assert_called_once()
                other = root / "other"
                with (
                    patch.object(acquisition, "metadata_digest", side_effect=["a" * 64, "a" * 64, "b" * 64]),
                    self.assertRaisesRegex(ValueError, "changed during acquisition"),
                ):
                    acquisition.acquire(source, other)
                self.assertFalse(other.exists())
            with (
                patch.object(acquisition, "require_permission", side_effect=ValueError("permission denied")),
                patch.object(acquisition, "download_archive") as forbidden,
                self.assertRaisesRegex(ValueError, "permission denied"),
            ):
                acquisition.acquire(source, root / "denied")
            forbidden.assert_not_called()


if __name__ == "__main__":
    unittest.main()
