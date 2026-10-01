"""Offline regression tests for package identity and download-budget consistency."""

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from tools import devskim_provenance as provenance


# Purpose: Validate metadata admission and exact identity without network or SDK installation.
# Inputs: Tiny temporary packages and copied canonical metadata.
# Outputs: unittest assertions; no downloaded or executable files are needed.
class ProvenanceTests(unittest.TestCase):
    # Purpose: Reject malformed/injectable versions, digests, sizes, schemas, and extra fields.
    # Inputs: One mutated canonical field per case. Outputs: ValueError for each invalid pin.
    def test_invalid_manifest(self):
        baseline = provenance.read_manifest()
        cases = [
            {"version": "1.0.100\nINJECT=value"},
            {"version": "1.0.100-beta"},
            {"version": "../1.0.100"},
            {"nupkg_sha256": "a" * 64},
            {"nupkg_sha256": "G" * 64},
            {"nupkg_sha256": None},
            {"nupkg_bytes": True},
            {"nupkg_bytes": 0},
            {"nupkg_bytes": provenance.MAX_PACKAGE_BYTES + 1},
            {"schema_version": True},
            {"schema_version": 2},
            {"extra": "unexpected"},
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "pin.json"
            for change in cases:
                with self.subTest(change=change):
                    path.write_text(json.dumps(baseline | change))
                    with self.assertRaises(ValueError):
                        provenance.read_manifest(path)

    # Purpose: Prevent an unbounded or non-object metadata file from reaching CI outputs.
    # Inputs: Oversized, truncated, or wrong-shaped JSON. Outputs: Rejected metadata.
    def test_manifest_input_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "pin.json"
            duplicate = (
                json.dumps(provenance.read_manifest())
                .replace('"schema_version": 1', '"schema_version": 2, "schema_version": 1')
                .encode()
            )
            for payload in [b" " * (provenance.MAX_MANIFEST_BYTES + 1), b"{", b"[]", b"null", duplicate]:
                path.write_bytes(payload)
                with self.assertRaises(ValueError):
                    provenance.read_manifest(path)

    # Purpose: Keep curl's limit identical to the official verified package's exact size.
    # Inputs: Canonical manifest and production workflow. Outputs: Pin-to-command consistency.
    def test_production_download_contract(self):
        pin = provenance.read_manifest()
        self.assertEqual(pin["version"], "1.0.100")
        self.assertEqual(pin["nupkg_bytes"], 58204542)
        output = provenance.environment_lines(pin)
        self.assertEqual(len(output.splitlines()), 3)
        self.assertIn("DEVSKIM_NUPKG_BYTES=58204542\n", output)
        workflow = (provenance.MANIFEST.parents[1] / "workflows" / "security-code-scanning.yml").read_text()
        for required in [
            'python -m tools.devskim_provenance --emit-env >> "$GITHUB_ENV"',
            '--max-filesize "$DEVSKIM_NUPKG_BYTES"',
            'python -m tools.devskim_provenance --package "$package"',
            '--configfile "$feed/NuGet.Config"',
            'ET.SubElement(ET.SubElement(config, "fallbackPackageFolders"), "clear")',
            'NUGET_PACKAGES="$cache" dotnet tool install',
            'mkdir "$cache"',
            "--disable-supression --skip-excerpts",
        ]:
            self.assertIn(required, workflow)

    # Purpose: Detect truncation, growth and same-size corruption in the exact install package.
    # Inputs: Tiny known payload and matching digest. Outputs: Success only for original bytes.
    def test_package_identity(self):
        payload = b"synthetic package identity control"
        pin = {"nupkg_bytes": len(payload), "nupkg_sha256": hashlib.sha256(payload).hexdigest().upper()}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "package.nupkg"
            path.write_bytes(payload)
            provenance.verify_package(path, pin)
            for changed in [payload[:-1], payload + b"x", b"x" + payload[1:]]:
                path.write_bytes(changed)
                with self.assertRaises(ValueError):
                    provenance.verify_package(path, pin)

    # Purpose: Exercise the streaming boundary around the hash reader's one-MiB chunk.
    # Inputs: Generated payload sizes immediately below, at and above one MiB.
    # Outputs: Exact valid packages pass and an additional byte always fails.
    def test_stream_chunk_boundary(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "package.nupkg"
            for size in [1024 * 1024 - 1, 1024 * 1024, 1024 * 1024 + 1]:
                payload = b"x" * size
                pin = {"nupkg_bytes": size, "nupkg_sha256": hashlib.sha256(payload).hexdigest().upper()}
                path.write_bytes(payload)
                provenance.verify_package(path, pin)
                path.write_bytes(payload + b"x")
                with self.assertRaises(ValueError):
                    provenance.verify_package(path, pin)


if __name__ == "__main__":
    unittest.main()
