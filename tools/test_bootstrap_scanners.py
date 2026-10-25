"""Offline provenance, cache and local-feed contracts for scanner provisioning."""

from __future__ import annotations

import hashlib
import io
import json
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

from tools import bootstrap_scanners as bootstrap


class ScannerBootstrapTests(unittest.TestCase):
    # Purpose: Own every fixture file outside the published tree.
    # Inputs: One test method. Outputs: A disposable temporary root.
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="scanner-bootstrap-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    # Purpose: Reject duplicate, malformed and hosted-version-divergent package pins before installation.
    # Inputs: Current manifest plus controlled invalid copies and workflow text.
    # Outputs: Exact current pin admitted; each invalid control raises.
    def test_gitleaks_pin_contract(self):
        pin = bootstrap.read_gitleaks_pin()
        self.assertEqual(pin["version"], "8.30.1")
        manifest = self.root / "pin.json"
        manifest.write_text('{"version":"8.30.1","version":"8.30.1"}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            bootstrap.read_gitleaks_pin(manifest)
        bad = dict(pin, binary_bytes=True)
        manifest.write_text(json.dumps(bad), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "size pin"):
            bootstrap.read_gitleaks_pin(manifest)
        bad = dict(pin, version="8.30.2")
        manifest.write_text(json.dumps(bad), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "differs from the hosted scanner"):
            bootstrap.read_gitleaks_pin(manifest)

    # Purpose: Admit complete HTTPS bytes and reject oversize or changed data independently of network state.
    # Inputs: Controlled in-memory response and exact digest/size pins. Outputs: Verified output or a closed error.
    def test_download_verified_rejects_wrong_bytes_and_transport(self):
        good = b"verified scanner asset"
        digest = hashlib.sha256(good).hexdigest().upper()
        output = self.root / "good.bin"
        with patch.object(bootstrap.urllib.request, "urlopen", return_value=io.BytesIO(good)):
            bootstrap.download_verified("https://github.com/asset", output, len(good), digest)
        self.assertEqual(output.read_bytes(), good)
        with (
            patch.object(bootstrap.urllib.request, "urlopen", return_value=io.BytesIO(good + b"x")),
            self.assertRaisesRegex(ValueError, "exceeds its pinned size"),
        ):
            bootstrap.download_verified("https://github.com/asset", self.root / "extra.bin", len(good), digest)
        with (
            patch.object(bootstrap.urllib.request, "urlopen", return_value=io.BytesIO(good[:-1] + b"x")),
            self.assertRaisesRegex(ValueError, "differs from pinned provenance"),
        ):
            bootstrap.download_verified("https://github.com/asset", self.root / "wrong.bin", len(good), digest)
        with self.assertRaisesRegex(ValueError, "URL or byte budget"):
            bootstrap.download_verified("file:///asset", self.root / "local.bin", len(good), digest)
        with self.assertRaisesRegex(ValueError, "URL or byte budget"):
            bootstrap.download_verified("https://untrusted.invalid/asset", self.root / "other.bin", len(good), digest)

    # Purpose: Extract only the exact executable from a verified ZIP without trusting other member names.
    # Inputs: Valid executable beside a traversal name, then altered executable bytes.
    # Outputs: Pinned binary accepted and altered binary rejected.
    def test_gitleaks_extraction_checks_complete_binary(self):
        archive = self.root / "scanner.zip"
        payload = b"scanner binary"
        pin = {"binary_bytes": len(payload), "binary_sha256": hashlib.sha256(payload).hexdigest().upper()}
        with zipfile.ZipFile(archive, "w") as package:
            package.writestr("../untrusted.exe", b"untrusted")
            package.writestr("gitleaks.exe", payload)
        bootstrap.extract_gitleaks(archive, self.root / "gitleaks.exe", pin)
        self.assertEqual((self.root / "gitleaks.exe").read_bytes(), payload)
        self.assertFalse((self.root.parent / "untrusted.exe").exists())
        bad = dict(pin, binary_sha256="0" * 64)
        with self.assertRaisesRegex(ValueError, "differs from pinned provenance"):
            bootstrap.extract_gitleaks(archive, self.root / "bad.exe", bad)

    # Purpose: Reuse an exact cached binary and repair a corrupted one using the verified archive path.
    # Inputs: Mocked exact release payload and an owned cache with a tampered executable.
    # Outputs: One download for first install, none for reuse, one for repair.
    def test_gitleaks_cache_reuse_and_repair(self):
        cache = self.root / "cache"
        cache.mkdir()
        payload = b"owned gitleaks"
        archive = self.root / "release.zip"
        with zipfile.ZipFile(archive, "w") as package:
            package.writestr("gitleaks.exe", payload)
        package_bytes = archive.read_bytes()
        pin = {
            "version": "8.30.1",
            "archive_bytes": len(package_bytes),
            "archive_sha256": hashlib.sha256(package_bytes).hexdigest().upper(),
            "binary_bytes": len(payload),
            "binary_sha256": hashlib.sha256(payload).hexdigest().upper(),
        }

        # The real downloader is covered above; this test controls its bytes at the cache boundary.
        def copy_release(url, destination, size, digest):
            self.assertTrue(url.endswith("/gitleaks_8.30.1_windows_x64.zip"))
            self.assertEqual(size, len(package_bytes))
            self.assertEqual(digest, pin["archive_sha256"])
            destination.write_bytes(package_bytes)

        with patch.object(bootstrap, "download_verified", side_effect=copy_release) as download:
            executable = bootstrap.provision_gitleaks(cache, pin)
            self.assertEqual(download.call_count, 1)
            self.assertEqual(bootstrap.provision_gitleaks(cache, pin), executable)
            self.assertEqual(download.call_count, 1)
            executable.write_bytes(b"tampered")
            self.assertEqual(bootstrap.provision_gitleaks(cache, pin), executable)
            self.assertEqual(download.call_count, 2)
        self.assertEqual(executable.read_bytes(), payload)

    # Purpose: Make the verified NuGet feed the only installer source and isolate SDK caches.
    # Inputs: Pinned in-memory package and controlled .NET subprocess returning a real test shim.
    # Outputs: Correct local source/configuration, exact package and version-checked shim.
    def test_devskim_installer_uses_verified_local_feed_only(self):
        stage = self.root / "stage"
        stage.mkdir()
        package_bytes = b"pinned nuget package fixture"
        pin = {
            "version": "1.0.100",
            "nupkg_bytes": len(package_bytes),
            "nupkg_sha256": hashlib.sha256(package_bytes).hexdigest().upper(),
        }

        def download(url, destination, size, digest):
            self.assertTrue(url.startswith("https://api.nuget.org/v3-flatcontainer/"))
            self.assertEqual((size, digest), (pin["nupkg_bytes"], pin["nupkg_sha256"]))
            destination.write_bytes(package_bytes)

        def run(command, **kwargs):
            if command[1:3] == ["tool", "install"]:
                self.assertEqual(kwargs["env"]["NUGET_PACKAGES"], str(stage / "packages"))
                config = Path(command[command.index("--configfile") + 1]).read_text(encoding="utf-8")
                self.assertIn("<clear", config)
                self.assertIn(str(stage / "feed"), config)
                (stage / "install").mkdir()
                (stage / "install/devskim.exe").write_bytes(b"shim")
                return subprocess.CompletedProcess(command, 0, "installed", "")
            return subprocess.CompletedProcess(command, 0, "Microsoft.DevSkim.CLI 1.0.100", "")

        with (
            patch.object(bootstrap, "download_verified", side_effect=download),
            patch.object(bootstrap.subprocess, "run", side_effect=run),
        ):
            installed = bootstrap.install_verified_devskim("dotnet", stage, pin)
        self.assertEqual(installed.read_bytes(), b"shim")

    # Purpose: Reinstall after a damaged active shim without confusing old and new cache identities.
    # Inputs: Two controlled installer outcomes and a tampered first shim.
    # Outputs: Atomic receipt points to the current complete install; a valid cache is reused.
    def test_devskim_active_receipt_reuse_and_repair(self):
        cache = self.root / "cache"
        cache.mkdir()
        pin = {"version": "1.0.100", "nupkg_sha256": "A" * 64}

        def install(dotnet, stage, selected):
            self.assertEqual((dotnet, selected), ("dotnet", pin))
            destination = stage / "install"
            destination.mkdir()
            executable = destination / "devskim.exe"
            executable.write_bytes(b"verified scanner")
            return executable

        with (
            patch.object(bootstrap, "find_dotnet", return_value="dotnet"),
            patch.object(bootstrap, "install_verified_devskim", side_effect=install) as installer,
        ):
            first = bootstrap.provision_devskim(cache, pin)
            self.assertEqual(bootstrap.provision_devskim(cache, pin), first)
            self.assertEqual(installer.call_count, 1)
            first.write_bytes(b"tampered")
            second = bootstrap.provision_devskim(cache, pin)
            self.assertNotEqual(first, second)
            self.assertEqual(installer.call_count, 2)
            self.assertEqual(bootstrap.provision_devskim(cache, pin), second)


if __name__ == "__main__":
    unittest.main()
