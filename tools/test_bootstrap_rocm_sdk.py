"""Offline safety and repeatability checks for whole-distribution provisioning."""

import hashlib
import io
import json
import os
import runpy
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from tools import bootstrap_rocm_sdk as bootstrap
from tools.bootstrap_rocm_sdk import NoRedirect, download_archive, inspect_archive, provision, validate_member


class RocmBootstrapTests(unittest.TestCase):
    """Exercise actual archive boundaries and installation preservation, without installing software."""

    def test_lock_admission_rejects_ambiguous_types_fields_and_urls(self):
        """Purpose: Admit one complete typed ROCm distribution record.
        Inputs: Actual lock and controlled schema/provenance mutations. Outputs: Exact metadata or ValueError.
        """
        valid = bootstrap.read_lock()
        self.assertEqual(valid["version"], "10.1.0")
        variants = (
            valid | {"version": True},
            valid | {"archive_bytes": True},
            valid | {"unreviewed": "field"},
            valid | {"url": "https://example.invalid/sdk.tar.gz"},
            valid | {"release_notes": "https://example.invalid/notes"},
        )
        with tempfile.TemporaryDirectory() as owned:
            path = Path(owned) / "lock.json"
            for candidate in variants:
                path.write_text(json.dumps(candidate), encoding="utf-8")
                with self.subTest(candidate=candidate), self.assertRaisesRegex(ValueError, "ROCm SDK"):
                    bootstrap.read_lock(path)
            duplicate = json.dumps(valid).replace('{"version":', '{"version":"10.1.0","version":', 1)
            path.write_text(duplicate, encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate"):
                bootstrap.read_lock(path)

    def test_linked_provisioning_path_is_rejected(self):
        """Purpose: Refuse static redirection before provisioning reads or writes.
        Inputs: Owned Windows junction or POSIX directory symlink. Outputs: Error without target changes.
        """
        with tempfile.TemporaryDirectory() as owned:
            root = Path(owned)
            target = root / "target"
            target.mkdir()
            linked = root / "linked"
            if os.name == "nt":
                import subprocess

                subprocess.run(
                    ["cmd.exe", "/d", "/c", "mklink", "/J", str(linked), str(target)],
                    check=True,
                    capture_output=True,
                    timeout=30,
                )
            else:
                linked.symlink_to(target, target_is_directory=True)
            try:
                with self.assertRaisesRegex(ValueError, "linked path"):
                    bootstrap.reject_link_chain(linked / "child")
            finally:
                if os.name == "nt":
                    linked.rmdir()
                else:
                    linked.unlink()

    def test_existing_archive_tree_requires_every_directory_and_file(self):
        """Purpose: Bind adoption to the complete archive shape.
        Inputs: Tiny archive with explicit directory and file members. Outputs: Missing entries fail before hashing.
        """
        with tempfile.TemporaryDirectory() as owned:
            root = Path(owned)
            archive = root / "shape.tar.gz"
            with tarfile.open(archive, "w:gz") as output:
                directory = tarfile.TarInfo("include")
                directory.type = tarfile.DIRTYPE
                output.addfile(directory)
                item = tarfile.TarInfo("include/file.h")
                item.size = 1
                output.addfile(item, io.BytesIO(b"x"))
            lock = {
                "archive_bytes": archive.stat().st_size,
                "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                "members": 2,
                "unpacked_bytes": 1,
            }
            existing = root / "existing"
            existing.mkdir()
            with self.assertRaisesRegex(ValueError, "missing a directory"):
                inspect_archive(archive, lock, existing)
            (existing / "include").mkdir()
            with self.assertRaisesRegex(ValueError, "missing or changed files"):
                inspect_archive(archive, lock, existing)
            (existing / "include/file.h").write_bytes(b"too long")
            with self.assertRaisesRegex(ValueError, "missing or changed files"):
                inspect_archive(archive, lock, existing)

    def test_sdk_identity_rejects_large_or_wrong_version_manifest(self):
        """Purpose: Bound and identify critical SDK inputs.
        Inputs: Complete inert critical-file tree with oversized or wrong manifest. Outputs: Refusal.
        """
        files = {
            "share/therock/therock_manifest.json": b'{"rocm_version":"other"}',
            "bin/hipcc.exe": b"compiler",
            "lib/amdhip64.lib": b"library",
            "include/hip/hip_version.h": b"header",
            "lib/llvm/bin/clang.exe": b"compiler",
            "lib/llvm/amdgcn/bitcode/ocml.bc": b"bitcode",
        }
        with tempfile.TemporaryDirectory() as owned:
            root = Path(owned)
            for name, contents in files.items():
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(contents)
            with self.assertRaisesRegex(ValueError, "version differs"):
                bootstrap.sdk_identity(root, "expected")
            with patch.object(bootstrap, "CHUNK", 1), self.assertRaisesRegex(ValueError, "metadata bound"):
                bootstrap.sdk_identity(root, "other")

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
            wrong_receipt = root / "wrong-receipt"
            (wrong_receipt / "sdk").mkdir(parents=True)
            (wrong_receipt / "verified-distribution.json").write_text('{"sha256":"wrong"}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "receipt does not match"):
                provision(lock, wrong_receipt)
            with self.assertRaisesRegex(ValueError, "explicitly supplied"):
                provision(lock, root / "missing-explicit", root / "absent.tar.gz")
            with (
                patch.object(bootstrap.shutil, "disk_usage", return_value=SimpleNamespace(free=0)),
                self.assertRaisesRegex(ValueError, "headroom"),
            ):
                provision(lock, root / "no-download-space")
            with (
                patch.object(bootstrap.shutil, "disk_usage", return_value=SimpleNamespace(free=0)),
                self.assertRaisesRegex(ValueError, "complete ROCm SDK"),
            ):
                provision(lock, root / "no-extract-space", archive)

            def populate(_source, destination):
                """Purpose: Model admitted extraction. Inputs: Owned stage. Outputs: Exact critical fixture files."""
                for name, data in files.items():
                    target = destination / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(data)

            downloaded = root / "downloaded"

            def acquire(_lock, destination):
                """Purpose: Model an exact download. Inputs: Destination. Outputs: Copied admitted archive bytes."""
                destination.write_bytes(archive.read_bytes())

            with (
                patch.object(bootstrap.shutil, "disk_usage", return_value=SimpleNamespace(free=10**15)),
                patch.object(bootstrap, "download_archive", side_effect=acquire) as download,
                patch.object(bootstrap, "bounded_extract", side_effect=populate),
            ):
                self.assertEqual(provision(lock, downloaded), downloaded / "sdk")
            download.assert_called_once()
            failed_stage = root / "failed-stage"

            def fail_extract(_source, destination):
                """Purpose: Inject extraction failure. Inputs: Owned stage. Outputs: Debris then error."""
                (destination / "debris").write_bytes(b"fixture")
                raise RuntimeError("extract fixture failed")

            with (
                patch.object(bootstrap.shutil, "disk_usage", return_value=SimpleNamespace(free=10**15)),
                patch.object(bootstrap, "bounded_extract", side_effect=fail_extract),
                self.assertRaisesRegex(RuntimeError, "extract fixture failed"),
            ):
                provision(lock, failed_stage, archive)
            self.assertEqual(list(failed_stage.glob("stage-*")), [])
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
            redirected = "http://example.invalid/"
            NoRedirect().redirect_request(None, None, 302, "Found", {}, redirected)

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
            for mode, data, length in (
                ("valid", b"data", 4),
                ("timeout", b"data", 4),
                ("excess", b"excess", 4),
                ("short", b"dat", 4),
                ("hash", b"xxxx", 4),
                ("length", b"data", 5),
            ):
                response = io.BytesIO(data)
                response.headers = {"Content-Length": str(length)}
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
            insecure = "https://example.invalid/sdk".replace("https", "http", 1)
            for url in (insecure, "https://user@example.invalid/sdk#fragment"):
                with self.subTest(url=url), self.assertRaisesRegex(ValueError, "must be HTTPS"):
                    download_archive(lock | {"url": url}, root / "invalid-url")

    def test_repository_pin_is_complete_distribution_metadata(self):
        """Purpose: Keep version and provenance shared instead of host-specific installer configuration.
        Inputs: Reads the repository-owned lock without downloading or executing SDK components.
        Outputs: Requires complete archive dimensions and explicit local digest origin.
        """
        lock = bootstrap.read_lock()
        self.assertTrue(lock["url"].startswith("https://stable.repo.amd.com/"))
        self.assertRegex(lock["sha256"], r"^[a-f0-9]{64}$")
        self.assertGreater(lock["unpacked_bytes"], lock["archive_bytes"])
        self.assertIn("not an independently published AMD checksum", lock["digest_origin"])

    # Purpose: Exercise the actual Windows tar consumer and process-failure propagation on admitted tiny source.
    # Inputs: One owned header in a real TAR.GZ, then deletion of that admitted archive before extraction.
    # Outputs: Exact extraction succeeds; the missing-source failure cannot be reported as successful extraction.
    def test_actual_contained_extraction_and_missing_source_failure(self):
        import os

        from tools import bootstrap_rocm_sdk as bootstrap

        if os.name != "nt":
            self.skipTest("Native Windows provisioning consumer")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "owned source.tar.gz"
            payload = b"/* inert SDK extraction correctness fixture */\n"
            with tarfile.open(archive, "w:gz") as output:
                item = tarfile.TarInfo("include/fixture.h")
                item.size = len(payload)
                output.addfile(item, io.BytesIO(payload))
            lock = {
                "archive_bytes": archive.stat().st_size,
                "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                "members": 1,
                "unpacked_bytes": len(payload),
            }
            self.assertEqual(inspect_archive(archive, lock)["members"], 1)
            destination = root / "owned extraction"
            destination.mkdir()
            bootstrap.bounded_extract(archive, destination)
            self.assertEqual((destination / "include/fixture.h").read_bytes(), payload)
            archive.unlink()
            refused = root / "refused"
            refused.mkdir()
            with self.assertRaisesRegex(RuntimeError, "extraction failed"):
                bootstrap.bounded_extract(archive, refused)
            self.assertEqual(list(refused.iterdir()), [])

    def test_containment_loader_and_cli_failure_routes(self):
        """Purpose: Fail closed when containment or CLI provisioning is unavailable.
        Inputs: Missing loader, supported/unsupported host routes and injected provision failure. Outputs: Exact status.
        """
        with (
            patch.object(bootstrap.importlib.util, "spec_from_file_location", return_value=None),
            self.assertRaisesRegex(RuntimeError, "containment is unavailable"),
        ):
            bootstrap.bounded_extract(Path("archive"), Path("destination"))
        with (
            patch.object(sys, "argv", ["bootstrap_rocm_sdk.py"]),
            patch.object(bootstrap, "os", SimpleNamespace(name="posix")),
            patch.object(sys, "stderr", io.StringIO()),
            self.assertRaises(SystemExit),
        ):
            bootstrap.main()
        lock = bootstrap.read_lock()
        with tempfile.TemporaryDirectory() as owned:
            parent = Path(owned) / "sdk"
            with (
                patch.object(sys, "argv", ["bootstrap_rocm_sdk.py", "--parent", str(parent)]),
                patch.object(bootstrap, "os", SimpleNamespace(name="nt")),
                patch.object(bootstrap, "read_lock", return_value=lock),
                patch.object(bootstrap, "provision", return_value=parent / "sdk") as provisioned,
                patch("builtins.print"),
            ):
                self.assertEqual(bootstrap.main(), 0)
            provisioned.assert_called_once_with(lock, parent, None)
            with (
                patch.object(sys, "argv", ["bootstrap_rocm_sdk.py", "--parent", str(parent)]),
                patch.object(bootstrap, "os", SimpleNamespace(name="nt")),
                patch.object(bootstrap, "read_lock", return_value=lock),
                patch.object(bootstrap, "provision", side_effect=ValueError("fixture failure")),
                patch.object(sys, "stderr", io.StringIO()) as error,
            ):
                self.assertEqual(bootstrap.main(), 1)
            self.assertIn("fixture failure", error.getvalue())
        original_path = list(sys.path)
        try:
            with (
                patch.object(sys, "argv", ["bootstrap_rocm_sdk.py", "--help"]),
                patch.object(sys, "stdout", io.StringIO()),
                self.assertRaises(SystemExit) as status,
            ):
                runpy.run_path(str(Path(bootstrap.__file__).resolve()), run_name="__main__")
            self.assertEqual(status.exception.code, 0)
        finally:
            sys.path[:] = original_path

    # Purpose: Keep the whole SDK acquisition deadline active through the terminal response read.
    # Inputs: Four inert bytes, an offline response and a clock that advances only when EOF is observed.
    # Outputs: A late response never publishes an archive or leaves multi-gigabyte partial debris.
    def test_download_deadline_includes_terminal_read(self):
        from tools import bootstrap_rocm_sdk as bootstrap

        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "sdk.tar.gz"
            payload = b"data"
            lock = {
                "url": "https://example.invalid/sdk",
                "archive_bytes": len(payload),
                "sha256": hashlib.sha256(payload).hexdigest(),
            }
            response = io.BytesIO(payload)
            response.headers = {"Content-Length": str(len(payload))}
            with (
                patch.object(bootstrap.urllib.request, "build_opener") as opener,
                patch.object(bootstrap.time, "monotonic", side_effect=[0, 1, 1201]),
            ):
                opener.return_value.open.return_value = response
                with self.assertRaisesRegex(ValueError, "lifetime"):
                    download_archive(lock, destination)
            self.assertFalse(destination.exists())
            partials = list(destination.parent.glob("*.partial"))
            self.assertEqual(partials, [])


if __name__ == "__main__":
    unittest.main()
