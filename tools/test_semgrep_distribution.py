"""Integrity and dependency-boundary regressions for the Semgrep packaging revision."""

import base64
import csv
import hashlib
import io
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

from tools import semgrep_distribution as distribution


def fixture(*, newline="\n", dependency="1.29.0", extra=None) -> bytes:
    """Purpose: Create an independent standard-wheel fixture. Inputs: Metadata variant. Outputs: Recorded ZIP bytes."""
    info = "semgrep-1.181.0.dist-info/"
    files = {
        "semgrep/scan.py": b"print('scanner fixture')\n",
        "semgrep/bin/semgrep-core": b"executable fixture\x00\xff",
        info + "licenses/LICENSE": b"preserved license and attribution\n",
        info + "WHEEL": b"Wheel-Version: 1.0\nRoot-Is-Purelib: false\nTag: py3-none-any\n",
        info + "METADATA": newline.join(
            [
                "Metadata-Version: 2.4",
                "Name: semgrep",
                "Version: 1.181.0",
                f"Requires-Dist: mcp=={dependency}",
                "Requires-Dist: pyjwt[crypto]<3,>=2.15.0",
                "",
                "Description",
                "",
            ]
        ).encode(),
    }
    files.update(extra or {})
    record = io.StringIO()
    writer = csv.writer(record, lineterminator="\n")
    for name, data in files.items():
        digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
        writer.writerow([name, "sha256=" + digest, str(len(data))])
    writer.writerow([info + "RECORD", "", ""])
    files[info + "RECORD"] = record.getvalue().encode()
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as archive:
        for name, data in files.items():
            member = zipfile.ZipInfo(name)
            # Preserve the raw test spelling: ZipInfo otherwise normalizes Windows separators.
            member.filename = name
            member.orig_filename = name
            member.create_system = 3
            member.external_attr = (0o100755 if name.endswith("semgrep-core") else 0o100644) << 16
            archive.writestr(member, data)
    return output.getvalue()


class DistributionTests(unittest.TestCase):
    # Purpose: Preserve executable/source/license bytes across platform metadata line endings.
    # Inputs: Independently recorded LF and CRLF wheels. Outputs: Only identity, dependency and RECORD change.
    def test_exact_patch_preserves_payloads_and_permissions(self):
        recipe = distribution.recipe()
        for newline in ("\n", "\r\n"):
            with self.subTest(newline=repr(newline)):
                original = fixture(newline=newline)
                result = distribution.derive_wheel(original, recipe)
                self.assertEqual(result, distribution.derive_wheel(original, recipe))
                before, after = distribution.read_wheel(original), distribution.read_wheel(result)
                self.assertEqual(len(before), len(after))
                for name, value in before.items():
                    renamed = name.replace("semgrep-1.181.0.dist-info/", "semgrep-1.181.0+flitzzip.mcp1.dist-info/")
                    if not name.endswith(("/METADATA", "/RECORD")):
                        self.assertEqual(after[renamed], value)
                info = "semgrep-1.181.0+flitzzip.mcp1.dist-info/"
                expected = before["semgrep-1.181.0.dist-info/METADATA"][0]
                expected = expected.replace(b"Version: 1.181.0", b"Version: 1.181.0+flitzzip.mcp1")
                expected = expected.replace(b"Requires-Dist: mcp==1.29.0", b"Requires-Dist: mcp==1.30.0")
                self.assertEqual(after[info + "METADATA"][0], expected)
                distribution.validate_record(after, info + "RECORD")
                self.assertEqual(after["semgrep/bin/semgrep-core"][1], 0o100755)

    # Purpose: Refuse metadata drift instead of broadening a dependency patch to unknown inputs.
    # Inputs: Freshly recorded wheels with an unexpected requirement. Outputs: Explicit patch failure.
    def test_metadata_drift_is_rejected(self):
        for requirement in ("1.28.0", "1.30.0", "1.29.0,<2"):
            with self.subTest(requirement=requirement), self.assertRaises(ValueError):
                distribution.derive_wheel(fixture(dependency=requirement), distribution.recipe())

    # Purpose: Detect missing, duplicated or altered integrity records independently of the outer source hash.
    # Inputs: A valid file map with corrupted RECORD or payload. Outputs: Refusal for every incomplete record.
    def test_record_corruption_is_rejected(self):
        files = distribution.read_wheel(fixture())
        record = "semgrep-1.181.0.dist-info/RECORD"
        variants = [
            {**files, "semgrep/scan.py": (b"altered", 0o100644)},
            {**files, record: (files[record][0].split(b"\n", 1)[1], 0o100644)},
            {**files, record: (files[record][0] + b"missing,sha256=invalid,1\n", 0o100644)},
        ]
        for variant in variants:
            with self.subTest(record=variant[record][0]), self.assertRaises(ValueError):
                distribution.validate_record(variant, record)

    # Purpose: Prevent path aliases or hostile extraction names from entering a derived wheel.
    # Inputs: Original wheel members outside canonical relative paths. Outputs: Rejection without filesystem writes.
    def test_unsafe_members_are_rejected(self):
        for name in ("../escape", "/absolute", "C:/drive", "a\\b", "a//b", "a/./b"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                distribution.read_wheel(fixture(extra={name: b"payload"}))

    # Purpose: Require exact response extent and digest before metadata parsing or publication.
    # Inputs: Bounded mock HTTPS bodies. Outputs: Successful exact body or rejection of altered/truncated bytes.
    def test_download_integrity_and_extent(self):
        contents = b"official wheel fixture"
        pin = {
            "url": "https://files.pythonhosted.org/fixture.whl",
            "bytes": len(contents),
            "sha256": hashlib.sha256(contents).hexdigest(),
        }
        for body, length, expected in (
            (contents, len(contents), True),
            (contents[:-1], len(contents), False),
            (contents + b"x", len(contents), False),
            (b"x" * len(contents), len(contents), False),
            (contents, len(contents) + 1, False),
        ):
            response = io.BytesIO(body)
            response.status = 200
            response.headers = {"Content-Length": str(length)}
            with (
                self.subTest(body=body, length=length),
                patch.object(distribution.urllib.request, "build_opener") as opener,
            ):
                opener.return_value.open.return_value = response
                if expected:
                    self.assertEqual(distribution.download(pin), contents)
                else:
                    with self.assertRaises(ValueError):
                        distribution.download(pin)
        with self.assertRaises(ValueError):
            distribution.NoRedirect().redirect_request(None, None, 302, "", {}, "https://untrusted.invalid")

    # Purpose: Refuse output collisions before network work and reject an unreviewed derived digest.
    # Inputs: Owned directory and synthetic source wheel. Outputs: No clobbering or wheel publication on failure.
    def test_build_never_overwrites_or_publishes_wrong_digest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.object(distribution, "download", return_value=fixture()) as download:
                with self.assertRaises(FileExistsError):
                    distribution.build(root, "linux")
                download.assert_not_called()
                with self.assertRaisesRegex(ValueError, "derived|Derived"):
                    distribution.build(root / "new", "linux")
            self.assertEqual(list((root / "new").iterdir()), [])


if __name__ == "__main__":
    unittest.main()
