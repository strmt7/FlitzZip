"""Offline provenance, integrity and code-parity tests for the scanner packaging patch."""

import base64
import copy
import csv
import hashlib
import io
import json
import re
import tempfile
import unittest
import zipfile
from email.parser import BytesParser
from pathlib import Path

from tools import prepare_semgrep_wheel as packaging


# Purpose: Create a tiny upstream wheel with independent RECORD hashes.
# Inputs: new destination and optional deliberate RECORD/name corruption. Outputs: a valid or malformed ZIP fixture.
def make_wheel(path: Path, *, corrupt=False, duplicate=False) -> dict:
    manifest = json.loads(packaging.MANIFEST.read_text())
    prefix = "semgrep-1.178.0.dist-info/"
    files = {
        "semgrep/tool.py": b"value = 42\n",
        prefix + "METADATA": (
            b"Metadata-Version: 2.4\nName: semgrep\nVersion: 1.178.0\n"
            b"Requires-Dist: attrs>=21.3\nRequires-Dist: pyjwt[crypto]~=2.13.0\n"
            b"Requires-Dist: mcp==1.29.0\n\nOriginal upstream description. UTF-8: \xc3\xa9.\n"
        ),
        prefix + "WHEEL": b"Wheel-Version: 1.0\nRoot-Is-Purelib: false\nTag: py3-none-any\n",
        prefix + "licenses/LICENSE": b"Original license notice\n",
    }
    rows = []
    with zipfile.ZipFile(path, "x") as archive:
        for name, data in files.items():
            archive.writestr(name, data)
            encoded = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
            rows.append((name, "sha256=" + encoded, str(len(data))))
        if corrupt:
            rows[0] = (rows[0][0], "sha256=" + "a" * 43, rows[0][2])
        if duplicate:
            archive.writestr("semgrep/tool.py", b"different content")
        record = io.StringIO(newline="")
        csv.writer(record, lineterminator="\n").writerows(rows + [(prefix + "RECORD", "", "")])
        archive.writestr(prefix + "RECORD", record.getvalue())
    manifest["sha256"] = packaging.file_digest(path)
    return manifest


class PackagingTests(unittest.TestCase):
    # Purpose: Verify exact payload parity, explicit local identity and reproducible output.
    # Inputs: independent miniature wheel. Outputs: matching code/notices and deterministic SHA-256 assertions.
    def test_code_parity_and_repeatability(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = make_wheel(root / "source.whl")
            first = packaging.build_wheel(root / "source.whl", root / "first.whl", manifest)
            second = packaging.build_wheel(root / "source.whl", root / "second.whl", manifest)
            self.assertEqual(first, second)
            with zipfile.ZipFile(root / "source.whl") as original, zipfile.ZipFile(root / "first.whl") as revised:
                old_prefix = "semgrep-1.178.0.dist-info"
                new_prefix = "semgrep-1.178.0+superzip.1.dist-info"
                members, records = packaging.wheel_inventory(revised, new_prefix)
                self.assertEqual(len(members), len(original.infolist()))
                for member in members:
                    if member.filename.endswith("/RECORD"):
                        continue
                    data = revised.read(member)
                    encoded = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
                    self.assertEqual(records[member.filename], ("sha256=" + encoded, str(len(data))))
                for name in original.namelist():
                    if name.endswith(("/METADATA", "/RECORD")):
                        continue
                    new_name = new_prefix + name[len(old_prefix) :] if name.startswith(old_prefix + "/") else name
                    self.assertEqual(original.read(name), revised.read(new_name))
                metadata = BytesParser(policy=packaging.METADATA_POLICY).parsebytes(
                    revised.read(new_prefix + "/METADATA")
                )
                self.assertEqual(metadata["Version"], manifest["local_version"])
                self.assertEqual(
                    metadata.get_all("Requires-Dist"), ["attrs>=21.3", "pyjwt[crypto]==2.15.1", "mcp==1.29.0"]
                )
                self.assertEqual(
                    metadata.get_payload(decode=True), b"Original upstream description. UTF-8: \xc3\xa9.\n"
                )

    # Purpose: Reject a manifest capable of escaping staging or selecting an unrelated origin.
    # Inputs: one-field changes to the trusted manifest. Outputs: identity validation failures.
    def test_manifest_identity(self):
        original = packaging.read_manifest(packaging.MANIFEST)
        for field, value in (
            ("filename", "../escape.whl"),
            ("local_version", "1.178.0/escape"),
            ("url", "https://files.pythonhosted.org.attacker.invalid/packages/x"),
            ("schema", True),
            ("tested_requirement", "pyjwt[crypto]>=2.15.1"),
        ):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as directory:
                manifest = {**original, field: value}
                path = Path(directory) / "manifest.json"
                path.write_text(json.dumps(manifest))
                with self.assertRaises(ValueError):
                    packaging.read_manifest(path)

    # Purpose: Keep dependency floors and the production install path aligned with the tested patch.
    # Inputs: tracked manifest, requirement files and workflow. Outputs: fail-closed parity and no-bypass assertions.
    def test_production_configuration_parity(self):
        manifest = packaging.read_manifest(packaging.MANIFEST)
        directory = packaging.MANIFEST.parent
        source = (directory / "requirements-semgrep-linux.in").read_text()
        locked = (directory / "requirements-semgrep-linux.txt").read_text()
        for contents in (source, locked):
            self.assertEqual(re.findall(r"(?m)^semgrep==([^\s]+)", contents), [manifest["local_version"]])
            self.assertIn("--find-links ../../out/scanner-wheels\n", contents)
        self.assertIn(manifest["tested_requirement"], source)
        jwt_version = manifest["tested_requirement"].split("==")[1]
        self.assertEqual(re.findall(r"(?m)^pyjwt==([^\s]+)", locked), [jwt_version])
        workflow = (packaging.ROOT / ".github/workflows/security-code-scanning.yml").read_text()
        for command in (
            "python -m unittest tools.test_prepare_semgrep_wheel",
            "python tools/prepare_semgrep_wheel.py",
            '"$RUNNER_TEMP/semgrep/bin/python" -m pip check',
            '"$RUNNER_TEMP/semgrep/bin/python" -m unittest tools.test_semgrep_runtime',
        ):
            self.assertIn(command, workflow)
        self.assertNotIn("--no-deps", workflow)

    # Purpose: Refuse unreviewed artifact changes even if the ZIP still parses.
    # Inputs: wrong manifest digest. Outputs: SHA-256 admission failure before output creation.
    def test_upstream_hash_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = make_wheel(root / "source.whl")
            manifest["sha256"] = "0" * 64
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                packaging.build_wheel(root / "source.whl", root / "result.whl", manifest)
            self.assertFalse((root / "result.whl").exists())

    # Purpose: Independently verify upstream RECORD rather than trust only ZIP CRCs.
    # Inputs: wheel with a deliberately wrong member digest. Outputs: integrity rejection.
    def test_record_corruption(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = make_wheel(root / "source.whl", corrupt=True)
            with self.assertRaisesRegex(ValueError, "integrity"):
                packaging.build_wheel(root / "source.whl", root / "result.whl", manifest)

    # Purpose: Prevent ambiguous repeated ZIP members from changing scanner code.
    # Inputs: a repeated code entry. Outputs: duplicate-member rejection.
    def test_duplicate_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertWarns(UserWarning):
                manifest = make_wheel(root / "source.whl", duplicate=True)
            with self.assertRaisesRegex(ValueError, "duplicate"):
                packaging.build_wheel(root / "source.whl", root / "result.whl", manifest)

    # Purpose: Require a fresh review when upstream changes the dependency declaration.
    # Inputs: unexpected upstream version, requirement or repeated dependency. Outputs: fail-closed metadata validation.
    def test_metadata_drift(self):
        manifest = json.loads(packaging.MANIFEST.read_text())
        metadata = b"Name: semgrep\nVersion: 1.178.0\nRequires-Dist: pyjwt[crypto]~=2.13.0\n\n"
        for change in (
            metadata.replace(b"1.178.0", b"1.179.0"),
            metadata.replace(b"2.13.0", b"2.14.0"),
            metadata.replace(b"\n\n", b"\nRequires-Dist: pyjwt>=2\n\n"),
        ):
            with self.subTest(metadata=change), self.assertRaisesRegex(ValueError, "reviewed patch"):
                packaging.revise_metadata(change, manifest)

    # Purpose: Refuse overwriting an existing owned or unrelated output.
    # Inputs: pre-existing wheel path. Outputs: collision refusal and unchanged bytes.
    def test_existing_output_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = make_wheel(root / "source.whl")
            target = root / "result.whl"
            target.write_bytes(b"must remain intact")
            with self.assertRaises(FileExistsError):
                packaging.build_wheel(root / "source.whl", target, copy.deepcopy(manifest))
            self.assertEqual(target.read_bytes(), b"must remain intact")


if __name__ == "__main__":
    unittest.main()
