"""Offline safety and repeatability checks for whole-distribution provisioning."""

import hashlib
import io
import json
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.bootstrap_rocm_sdk import NoRedirect, download_archive, inspect_archive, provision, validate_member


class RocmBootstrapTests(unittest.TestCase):
    """Exercise actual archive boundaries and installation preservation, without installing software."""

    def test_rejects_windows_escape_collision_and_special_members(self):
        """Purpose: Prove untrusted names cannot escape the SDK or collide under Windows naming rules.
        Inputs: Host-independent archive metadata covers traversal, streams, invalid names, links and devices.
        Outputs: Requires rejection before extraction while ordinary SDK paths remain accepted.
        """
        for name in (
            "../outside",
            "/absolute",
            "C:/escape",
            "a\\b",
            "file:stream",
            "NUL.txt",
            "COM1",
            "a./file",
            "a /file",
            "file?",
            'file"',
            "bad\nname",
            ".",
        ):
            with self.subTest(name=name), self.assertRaises(ValueError):
                validate_member(tarfile.TarInfo(name), set())
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.FIFOTYPE, tarfile.CHRTYPE):
            entry = tarfile.TarInfo("unsafe")
            entry.type = kind
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                validate_member(entry, set())
        seen = set()
        validate_member(tarfile.TarInfo("./include/Hip.h"), seen)
        with self.assertRaises(ValueError):
            validate_member(tarfile.TarInfo("include/hip.h"), seen)
        entry = tarfile.TarInfo("huge")
        entry.size = 8 * 1024**3 + 1
        with self.assertRaises(ValueError):
            validate_member(entry, set())

    def test_adopts_byte_exact_sdk_and_rejects_changed_cache_without_overwrite(self):
        """Purpose: Verify complete manual-install adoption, repeatable cache use and fail-closed corruption handling.
        Inputs: A tiny real tar archive and file tree serve only as a provisioning fixture, never a benchmark.
        Outputs: Requires exact verification, corruption rejection and no cache-hit extraction or download.
        """
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk = root / "sdk"
            files = {
                "share/therock/therock_manifest.json": b'{"rocm_version":"test-version"}',
                "bin/hipcc.exe": b"fixture compiler; never executed",
                "lib/amdhip64.lib": b"fixture import library",
                "include/hip/hip_version.h": b"fixture header",
                "lib/llvm/bin/clang.exe": b"fixture compiler; never executed",
                "lib/llvm/amdgcn/bitcode/ocml.bc": b"fixture bitcode",
            }
            archive = root / "distribution.tar.gz"
            with tarfile.open(archive, "w:gz") as stream:
                for name, data in files.items():
                    member = tarfile.TarInfo(name)
                    member.size = len(data)
                    stream.addfile(member, io.BytesIO(data))
                    target = sdk / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(data)
            lock = {
                "archive_bytes": archive.stat().st_size,
                "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                "members": len(files),
                "unpacked_bytes": sum(map(len, files.values())),
                "version": "test-version",
                "archive_name": archive.name,
                "url": "https://example.invalid/distribution",
                "digest_origin": "fixture",
            }
            with patch("tools.bootstrap_rocm_sdk.bounded_extract") as extract:
                self.assertEqual(provision(lock, root, archive), sdk)
                self.assertEqual(provision(lock, root), sdk)
                extract.assert_not_called()
            self.assertEqual(inspect_archive(archive, lock)["members"], len(files))
            portable = root / "portable installation"

            def extract_fixture(source, destination):
                """Purpose: Stand in for native extraction using the independently declared fixture files.
                Inputs: Source must be this test's admitted archive; destination is a fresh test-owned directory.
                Outputs: Writes only predefined fixture paths and bytes; never extracts archive-controlled names.
                """
                self.assertEqual(source, archive)
                for name, data in files.items():
                    target = destination / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with target.open("xb") as output:
                        output.write(data)

            with patch("tools.bootstrap_rocm_sdk.bounded_extract", side_effect=extract_fixture) as extract:
                self.assertEqual(provision(lock, portable, archive), portable / "sdk")
                self.assertEqual(provision(lock, portable), portable / "sdk")
                self.assertEqual(extract.call_count, 1)
            changed = sdk / "lib/amdhip64.lib"
            changed_bytes = b"x" * len(files["lib/amdhip64.lib"])
            changed.write_bytes(changed_bytes)
            with self.assertRaisesRegex(ValueError, "critical build inputs changed"):
                provision(lock, root)
            self.assertEqual(changed.read_bytes(), changed_bytes)
            (root / "verified-distribution.json").unlink()
            with self.assertRaisesRegex(ValueError, "bytes differ"):
                provision(lock, root, archive)
            self.assertEqual(changed.read_bytes(), changed_bytes)
            wrong = {**lock, "sha256": "0" * 64}
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                inspect_archive(archive, wrong)
            wrong = {**lock, "unpacked_bytes": lock["unpacked_bytes"] + 1}
            with self.assertRaisesRegex(ValueError, "inventory differs"):
                inspect_archive(archive, wrong)
            with (
                patch("tools.bootstrap_rocm_sdk.time.monotonic", side_effect=(0, 901)),
                self.assertRaisesRegex(ValueError, "lifetime bounds"),
            ):
                inspect_archive(archive, lock)

    def test_redirects_are_not_silently_followed(self):
        """Purpose: Preserve the exact HTTPS provenance boundary across redirects.
        Inputs: Redirect metadata points outside the original source.
        Outputs: Requires rejection before the redirected request.
        """
        with self.assertRaisesRegex(ValueError, "redirects"):
            NoRedirect().redirect_request(None, None, 302, "Found", {}, "http://example.invalid/")

    def test_download_rejects_timeout_excess_short_bytes_and_bad_hash(self):
        """Purpose: Exercise real streaming admission and timeout failures through an offline transport.
        Inputs: Tiny byte streams simulate valid HTTPS data and transport failures; no network request is made.
        Outputs: Requires byte/hash/time rejection and preserves existing archives without overwriting.
        """
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock = {
                "url": "https://example.invalid/sdk",
                "archive_bytes": 4,
                "sha256": hashlib.sha256(b"data").hexdigest(),
            }
            for mode, data in (
                ("valid", b"data"),
                ("timeout", b"data"),
                ("excess", b"excess"),
                ("short", b"dat"),
                ("hash", b"xxxx"),
            ):
                response = io.BytesIO(data)
                response.headers = {"Content-Length": "4"}
                destination = root / mode
                with patch("tools.bootstrap_rocm_sdk.urllib.request.build_opener") as opener:
                    opener.return_value.open.return_value = response
                    if mode == "timeout":
                        with (
                            patch("tools.bootstrap_rocm_sdk.time.monotonic", side_effect=(0, 1201)),
                            self.assertRaisesRegex(ValueError, "lifetime bounds"),
                        ):
                            download_archive(lock, destination)
                    elif mode == "valid":
                        download_archive(lock, destination)
                        self.assertEqual(destination.read_bytes(), b"data")
                    else:
                        with self.assertRaises(ValueError):
                            download_archive(lock, destination)
                    if mode != "valid":
                        self.assertFalse(destination.exists())
                    self.assertEqual(opener.return_value.open.call_args.kwargs["timeout"], 60)
            with self.assertRaisesRegex(ValueError, "overwrite"):
                download_archive(lock, root / "valid")
            self.assertEqual((root / "valid").read_bytes(), b"data")

    def test_repository_pin_is_complete_distribution_metadata(self):
        """Purpose: Keep version and provenance shared instead of host-specific installer configuration.
        Inputs: Reads the repository-owned lock without downloading or executing SDK components.
        Outputs: Requires complete archive dimensions and explicit local digest origin.
        """
        lock = json.loads((Path(__file__).parent / "rocm-sdk-lock.json").read_text(encoding="utf-8"))
        self.assertTrue(lock["url"].startswith("https://stable.repo.amd.com/"))
        self.assertRegex(lock["sha256"], r"^[a-f0-9]{64}$")
        self.assertGreater(lock["unpacked_bytes"], lock["archive_bytes"])
        self.assertIn("not an independently published AMD checksum", lock["digest_origin"])


if __name__ == "__main__":
    unittest.main()
