"""Integrity and dependency-boundary regressions for the Semgrep packaging revision."""

import base64
import copy
import csv
import hashlib
import io
import json
import runpy
import sys
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
                "Requires-Dist: opentelemetry-api~=1.37.0",
                "Requires-Dist: opentelemetry-sdk~=1.37.0",
                "Requires-Dist: opentelemetry-exporter-otlp-proto-http~=1.37.0",
                "Requires-Dist: opentelemetry-instrumentation-requests~=0.58b0",
                "Requires-Dist: opentelemetry-instrumentation-threading~=0.58b0",
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
    def test_recipe_rejects_ambiguous_or_incomplete_metadata(self):
        """Purpose: Convert every malformed packaging record into controlled refusal.
        Inputs: Wrong top-level and wheel-field shapes. Outputs: ValueError without field lookup exceptions.
        """
        valid = distribution.recipe()
        variants = (
            valid | {"unreviewed": True},
            valid | {"advisory": "https://example.invalid/advisory"},
            valid | {"wheels": ["linux", "windows"]},
            valid | {"wheels": valid["wheels"] | {"linux": []}},
            valid | {"wheels": valid["wheels"] | {"linux": valid["wheels"]["linux"] | {"sha256": None}}},
            valid | {"wheels": valid["wheels"] | {"linux": valid["wheels"]["linux"] | {"bytes": True}}},
        )
        for candidate in variants:
            with (
                self.subTest(candidate=candidate),
                patch.object(distribution, "read_recipe", return_value=candidate),
                self.assertRaises(ValueError),
            ):
                distribution.recipe()

    # Purpose: Reject a transport that returns all expected bytes but reaches EOF after the acquisition deadline.
    # Inputs: Tiny inert response bytes and a controlled monotonic clock; no remote server or waiting is used.
    # Outputs: Late EOF fails instead of admitting the wheel; an otherwise identical timely response succeeds.
    def test_download_deadline_includes_final_eof(self):
        payload = b"owned wheel transport fixture"
        pin = {"url": "https://example.invalid/wheel", "bytes": len(payload), "sha256": distribution.sha256(payload)}
        for elapsed, accepted in ((2, True), (181, False)):
            response = io.BytesIO(payload)
            response.headers, response.status = {"Content-Length": str(len(payload))}, 200
            with (
                self.subTest(elapsed=elapsed),
                patch.object(distribution.urllib.request, "build_opener") as opener,
                patch.object(distribution.time, "monotonic", side_effect=[0, 1, elapsed]),
            ):
                opener.return_value.open.return_value = response
                if accepted:
                    self.assertEqual(distribution.download(pin), payload)
                else:
                    with self.assertRaisesRegex(ValueError, "lifetime"):
                        distribution.download(pin)

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
                    renamed = name.replace(
                        "semgrep-1.181.0.dist-info/", "semgrep-1.181.0+flitzzip.security2.dist-info/"
                    )
                    if not name.endswith(("/METADATA", "/RECORD")):
                        self.assertEqual(after[renamed], value)
                info = "semgrep-1.181.0+flitzzip.security2.dist-info/"
                expected = before["semgrep-1.181.0.dist-info/METADATA"][0]
                expected = expected.replace(b"Version: 1.181.0", b"Version: 1.181.0+flitzzip.security2")
                expected = expected.replace(b"Requires-Dist: mcp==1.29.0", b"Requires-Dist: mcp==1.30.0")
                for package in ("api", "sdk", "exporter-otlp-proto-http"):
                    name = f"Requires-Dist: opentelemetry-{package}".encode()
                    expected = expected.replace(name + b"~=1.37.0", name + b"==1.45.1")
                for package in ("requests", "threading"):
                    name = f"Requires-Dist: opentelemetry-instrumentation-{package}".encode()
                    expected = expected.replace(name + b"~=0.58b0", name + b"==0.66b1")
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
        rows = list(csv.reader(io.StringIO(files[record][0].decode("utf-8"))))
        rows[1] = rows[0]
        duplicate = io.StringIO()
        writer = csv.writer(duplicate, lineterminator="\n")
        writer.writerows(rows)
        files[record] = (duplicate.getvalue().encode(), 0o100644)
        with self.assertRaisesRegex(ValueError, "unknown or duplicate"):
            distribution.validate_record(files, record)
        del files[record]
        with self.assertRaisesRegex(ValueError, "RECORD is missing"):
            distribution.validate_record(files, record)
        files[record] = (b"\xff", 0o100644)
        with self.assertRaisesRegex(ValueError, "RECORD is malformed"):
            distribution.validate_record(files, record)

    # Purpose: Prevent path aliases or hostile extraction names from entering a derived wheel.
    # Inputs: Original wheel members outside canonical relative paths. Outputs: Rejection without filesystem writes.
    def test_unsafe_members_are_rejected(self):
        for name in ("../escape", "/absolute", "C:/drive", "a\\b", "a//b", "a/./b", "name. "):
            with self.subTest(name=name), self.assertRaises(ValueError):
                distribution.read_wheel(fixture(extra={name: b"payload"}))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            distribution.read_wheel(fixture(extra={"Case/Name": b"first", "case/name": b"second"}))
        with patch.object(distribution, "MAX_BYTES", 1), self.assertRaisesRegex(ValueError, "extent"):
            distribution.read_wheel(fixture())

    def test_distribution_identity_drift_is_rejected(self):
        """Purpose: Bind the upstream wheel identity before changing metadata.
        Inputs: A correctly recorded wheel with the wrong project identity. Outputs: Refusal before patching.
        """
        info = "semgrep-1.181.0.dist-info/"
        metadata = b"Name: other\nVersion: 1.181.0\nRequires-Dist: mcp==1.29.0\n\n"
        with self.assertRaisesRegex(ValueError, "metadata differs"):
            distribution.derive_wheel(fixture(extra={info + "METADATA": metadata}), distribution.recipe())

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

    def test_successful_build_records_exact_artifacts_and_cleans_receipt_failure(self):
        """Purpose: Exercise complete publication and transactional failure cleanup.
        Inputs: Inert source wheel, reviewed digest and receipt failure.
        Outputs: Exact receipt or empty output.
        """
        source = fixture()
        data = copy.deepcopy(distribution.recipe())
        derived = distribution.derive_wheel(source, data)
        data["wheels"]["windows"]["derived_sha256"] = distribution.sha256(derived)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with (
                patch.object(distribution, "recipe", return_value=data),
                patch.object(distribution, "download", return_value=source),
            ):
                wheel = distribution.build(root / "complete", "windows")
            receipt = json.loads((wheel.parent / "receipt.json").read_text(encoding="utf-8"))
            self.assertEqual(receipt["output"], wheel.name)
            self.assertEqual(receipt["sha256"], distribution.sha256(wheel.read_bytes()))

            original_open = Path.open

            def fail_receipt(path, *args, **kwargs):
                """Purpose: Inject publication failure. Inputs: Output path/open options. Outputs: File or error."""
                if path.name == "receipt.json":
                    raise OSError("receipt failure")
                return original_open(path, *args, **kwargs)

            with (
                patch.object(distribution, "recipe", return_value=data),
                patch.object(distribution, "download", return_value=source),
                patch.object(Path, "open", side_effect=fail_receipt, autospec=True),
                self.assertRaisesRegex(OSError, "receipt failure"),
            ):
                distribution.build(root / "failed", "windows")
            self.assertEqual(list((root / "failed").iterdir()), [])
            with patch.object(distribution, "recipe", return_value=data), self.assertRaisesRegex(ValueError, "target"):
                distribution.build(root / "unsupported", "other")

    def test_cli_selects_explicit_and_native_targets_and_entrypoint_status(self):
        """Purpose: Exercise supported CLI routing and platform refusal.
        Inputs: Explicit/native target arguments and unsupported host facts.
        Outputs: Exact builder calls or parser exit.
        """
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "wheel"
            for arguments, system, machine, target in (
                (["--output", str(output), "--target", "linux"], "Other", "other", "linux"),
                (["--output", str(output)], "Windows", "AMD64", "windows"),
            ):
                with (
                    self.subTest(arguments=arguments),
                    patch.object(sys, "argv", ["semgrep_distribution.py", *arguments]),
                    patch.object(distribution.platform, "system", return_value=system),
                    patch.object(distribution.platform, "machine", return_value=machine),
                    patch.object(distribution, "build", return_value=output / "result.whl") as build,
                    patch("builtins.print"),
                ):
                    self.assertEqual(distribution.main(), 0)
                build.assert_called_once_with(output, target)
            for system, machine in (("Other", "x86_64"), ("Windows", "arm64")):
                with (
                    patch.object(sys, "argv", ["semgrep_distribution.py", "--output", str(output)]),
                    patch.object(distribution.platform, "system", return_value=system),
                    patch.object(distribution.platform, "machine", return_value=machine),
                    patch.object(sys, "stderr", io.StringIO()),
                    self.assertRaises(SystemExit),
                ):
                    distribution.main()
            with (
                patch.object(sys, "argv", ["semgrep_distribution.py", "--output", str(output), "--target", "other"]),
                patch.object(sys, "stderr", io.StringIO()),
                self.assertRaises(SystemExit) as status,
            ):
                runpy.run_path(str(Path(distribution.__file__).resolve()), run_name="__main__")
            self.assertEqual(status.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
