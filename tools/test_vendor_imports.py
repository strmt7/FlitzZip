"""Exact upstream-import proofs and adaptation refusal; no network or native build."""

from __future__ import annotations

import gzip
import hashlib
import io
import json
import os
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from urllib.parse import urlsplit

from tools import vendor_imports as imports
from tools.executable_path import executable_path


class VendorImportFixture(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.members = {
            "lz4.c": b"int independent_fixture(void) { return 7; }\n",
            "lz4.h": b"#pragma once\n",
            "LICENSE": b"Independent license fixture\n",
        }
        self.manifest = {
            "schema_version": 1,
            "component": "lz4",
            "version": "1.11.0",
            "url": "https://github.com/lz4/lz4/releases/download/v1.11.0/lz4-1.11.0.tar.gz",
            "archive": "third_party/upstream/lz4/1.11.0/lz4-1.11.0.tar.gz",
            "files": ["LICENSE", "lz4.c", "lz4.h"],
        }
        for name, data in self.members.items():
            path = self.root / imports.SOURCE_ROOT / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        self.save_manifest()
        self.save_archive()

    def save_manifest(self) -> None:
        (self.root / imports.MANIFEST).write_text(json.dumps(self.manifest), encoding="utf-8")

    def save_archive(self, role: str = "valid") -> None:
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode="w:") as archive:
            members = list(self.members.items())
            if role == "duplicate":
                members.append(members[0])
            if role == "missing":
                members.pop()
            for index, (name, data) in enumerate(members):
                member = tarfile.TarInfo("../escape" if role == "unsafe" and index == 0 else f"lz4-1.11.0/lib/{name}")
                if role == "link" and index == 0:
                    member.type, member.linkname = tarfile.SYMTYPE, "other"
                    archive.addfile(member)
                else:
                    member.size = len(data)
                    archive.addfile(member, io.BytesIO(data))
        compressed = gzip.compress(buffer.getvalue())
        path = self.root / self.manifest["archive"]
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(compressed)
        self.checksum = path.with_name("SHA256SUMS")
        self.checksum.write_text(hashlib.sha256(compressed).hexdigest() + "  " + path.name + "\n", encoding="ascii")


class VendorImportTests(VendorImportFixture):
    def test_official_tag_archive_preserves_complete_member_and_adaptation_proof(self) -> None:
        self.manifest["url"] = "https://github.com/lz4/lz4/archive/refs/tags/v1.11.0.tar.gz"
        self.save_manifest()
        expected = sorted(imports.SOURCE_ROOT + name for name in self.members)
        self.assertEqual(imports.classify(self.root)["canonical_paths"], expected)
        (self.root / "third_party/lz4/lz4.c").write_bytes(b"local adaptation\n")
        self.assertEqual(imports.classify(self.root)["canonical_paths"], [])

    def test_tag_archive_origin_cannot_change_publisher_ref_or_url_components(self) -> None:
        origin = urlsplit("https://github.com/lz4/lz4/archive/refs/tags/v1.11.0.tar.gz")
        for field, value in (
            ("scheme", "http"),
            ("netloc", "github.com.example.invalid"),
            ("netloc", "github.com@example.invalid"),
            ("path", "/other/lz4/archive/refs/tags/v1.11.0.tar.gz"),
            ("path", "/lz4/other/archive/refs/tags/v1.11.0.tar.gz"),
            ("path", "/lz4/lz4/archive/refs/heads/v1.11.0.tar.gz"),
            ("path", "/lz4/lz4/archive/refs/tags/v1.11.1.tar.gz"),
            ("query", "extra=1"),
            ("fragment", "fragment"),
            ("path", "/lz4/lz4/archive/refs/tags/../v1.11.0.tar.gz"),
        ):
            url = origin._replace(**{field: value}).geturl()
            with self.subTest(url=url):
                self.manifest["url"] = url
                self.save_manifest()
                with self.assertRaisesRegex(ValueError, "publisher"):
                    imports.classify(self.root)

    def test_complete_import_and_git_line_endings(self) -> None:
        expected = sorted(imports.SOURCE_ROOT + name for name in self.members)
        self.assertEqual(imports.classify(self.root)["canonical_paths"], expected)
        for name, data in self.members.items():
            (self.root / imports.SOURCE_ROOT / name).write_bytes(data.replace(b"\n", b"\r\n"))
        result = imports.classify(self.root)
        self.assertEqual(result["canonical_paths"], expected)
        self.assertEqual(result["adapted_paths"], [])
        self.assertEqual(
            result["source_sha256"],
            {imports.SOURCE_ROOT + name: hashlib.sha256(data).hexdigest() for name, data in self.members.items()},
        )

    def test_any_local_adaptation_removes_the_complete_component_classification(self) -> None:
        for name, data in self.members.items():
            with self.subTest(name=name):
                path = self.root / imports.SOURCE_ROOT / name
                path.write_bytes(data + b"local adaptation")
                result = imports.classify(self.root)
                self.assertEqual(result["canonical_paths"], [])
                self.assertEqual(result["adapted_paths"], [imports.SOURCE_ROOT + name])
                path.write_bytes(data)

    def test_archive_identity_and_member_failures_are_closed(self) -> None:
        for role in ("duplicate", "missing", "link", "unsafe"):
            with self.subTest(role=role):
                self.save_archive(role)
                with self.assertRaises(ValueError):
                    imports.classify(self.root)
        self.save_archive()
        path = self.root / self.manifest["archive"]
        path.write_bytes(path.read_bytes() + b"changed archive")
        with self.assertRaisesRegex(ValueError, "archive digest mismatch"):
            imports.classify(self.root)

    def test_manifest_cannot_broaden_the_policy(self) -> None:
        for key, value in (
            ("schema_version", True),
            ("component", "application"),
            ("version", "../1.11.0"),
            ("archive", "../escape"),
            ("url", "https://example.test/source"),
            ("url", []),
            ("url", {}),
            ("url", None),
            ("url", True),
            ("url", 7),
            ("files", []),
            ("files", ["lz4.c", "lz4.c", "LICENSE"]),
            ("files", [True, "lz4.h", "LICENSE"]),
        ):
            previous = self.manifest[key]
            with self.subTest(key=key, value=value):
                self.manifest[key] = value
                self.save_manifest()
                with self.assertRaises(ValueError):
                    imports.classify(self.root)
            self.manifest[key] = previous
        self.save_manifest()
        path = self.root / imports.MANIFEST
        payload = path.read_text().replace('"schema_version": 1', '"schema_version": 1, "schema_version": 1')
        path.write_text(payload, encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate properties"):
            imports.classify(self.root)

    def test_pinned_checksum_inventory_is_exact(self) -> None:
        original = self.checksum.read_text()
        for text in (original * 2, original.replace("  ", " "), original.replace("lz4-1.11.0", "other")):
            with self.subTest(text=text):
                self.checksum.write_text(text, encoding="ascii")
                with self.assertRaisesRegex(ValueError, "checksum inventory"):
                    imports.classify(self.root)
        self.checksum.write_text(original, encoding="ascii")
        path = self.root / imports.SOURCE_ROOT / "lz4.c"
        path.unlink()
        with self.assertRaises(FileNotFoundError):
            imports.classify(self.root)

    def test_limits_apply_before_unbounded_archive_or_source_reads(self) -> None:
        for name, limit in (
            ("MAX_ARCHIVE_BYTES", 1),
            ("MAX_EXPANDED_BYTES", 1),
            ("MAX_MEMBERS", 1),
            ("MAX_SOURCE_BYTES", 1),
        ):
            with self.subTest(limit=name), patch.object(imports, name, limit), self.assertRaises(ValueError):
                imports.classify(self.root)

    def test_cli_import_qualification_rejects_adaptations(self) -> None:
        args = ["vendor-imports", "--root", str(self.root), "--require-canonical"]
        with patch("sys.argv", args), patch("sys.stdout", new_callable=io.StringIO) as output:
            self.assertEqual(imports.main(), 0)
            self.assertEqual(json.loads(output.getvalue())["adapted_paths"], [])
        path = self.root / imports.SOURCE_ROOT / "lz4.c"
        path.write_bytes(path.read_bytes() + b"adapted")
        with patch("sys.argv", args), self.assertRaisesRegex(ValueError, "ordinary function gates"):
            imports.main()


@unittest.skipUnless(os.name == "nt", "The refactor audit uses Windows PowerShell")
class VendorAuditIntegrationTests(VendorImportFixture):
    def setUp(self) -> None:
        super().setUp()
        self.members["lz4.c"] = b"int fixture(void) {\n" + b"    operation();\n" * 125 + b"    return 0;\n}\n"
        (self.root / imports.SOURCE_ROOT / "lz4.c").write_bytes(self.members["lz4.c"])
        self.save_archive()
        tools = self.root / "tools"
        tools.mkdir()
        for name in ("refactor_audit.ps1", "vendor_imports.py", "json_input.py", "native_build_provenance.py"):
            (tools / name).write_bytes((imports.ROOT / "tools" / name).read_bytes())
        git = executable_path("git")
        for arguments in (
            ("init", "-q", "--initial-branch=main"),
            ("add", "tools"),
            ("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture"),
        ):
            subprocess.run([git, "-C", str(self.root), *arguments], check=True, capture_output=True, timeout=20)

    def audit(self) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                executable_path("powershell"),
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(self.root / "tools/refactor_audit.ps1"),
                "-ChangedOnly",
                "-MaxFunctionLines",
                "120",
                "-MaxComplexityMarkers",
                "35",
                "-FailOnFindings",
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )

    def test_only_the_archive_proven_import_receives_numeric_classification(self) -> None:
        canonical = self.audit()
        self.assertEqual(canonical.returncode, 0, canonical.stderr)
        self.assertIn("upstream_refactor_lead category=large-function", canonical.stdout)
        path = self.root / imports.SOURCE_ROOT / "lz4.c"
        path.write_bytes(path.read_bytes().replace(b"return 0;", b"operation();\n    return 0;"))
        adapted = self.audit()
        self.assertNotEqual(adapted.returncode, 0)
        self.assertIn("refactor_finding category=large-function", adapted.stdout)
        self.assertNotIn("upstream_refactor_lead", adapted.stdout)

    def test_first_party_code_and_invalid_provenance_still_block(self) -> None:
        path = self.root / "src/owned.c"
        path.parent.mkdir()
        path.write_bytes(self.members["lz4.c"])
        owned = self.audit()
        self.assertNotEqual(owned.returncode, 0)
        self.assertIn("refactor_finding category=large-function", owned.stdout)
        self.assertIn("src", owned.stdout)
        path.unlink()
        archive = self.root / self.manifest["archive"]
        archive.write_bytes(archive.read_bytes() + b"changed archive")
        invalid = self.audit()
        self.assertNotEqual(invalid.returncode, 0)
        self.assertNotIn("status=clean", invalid.stdout)
        self.assertIn("proof failed", invalid.stderr)


if __name__ == "__main__":
    unittest.main()
