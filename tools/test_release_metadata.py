"""Independent version and publication-boundary regressions for release metadata."""

import contextlib
import io
import itertools
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import release_metadata as metadata


class ReleaseMetadataTests(unittest.TestCase):
    # Purpose: Own release inputs and output sentinels without modifying repository or Actions state.
    # Inputs: None. Outputs: A cleanup-bound project root with an independently declared default version.
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\nproject(\n FlitzZip VERSION 4.5.6 LANGUAGES CXX)\n",
            encoding="utf-8",
        )

    # Purpose: Accept every SemVer prerelease identifier class, including digit-leading nonnumeric identifiers.
    # Inputs: Independently specified core/numeric/alphanumeric/hyphen cases from SemVer 2.0.0 section nine.
    # Outputs: Exact numeric components and unchanged suffix; no packaging or publication is performed.
    def test_semver_identifier_classes(self):
        for suffix in ("0", "9", "alpha", "alpha.1", "1beta", "01beta", "001a.2-z", "-", "x-y-z.--"):
            with self.subTest(suffix=suffix):
                self.assertEqual(metadata.parse_release_version("1.2.3-" + suffix), (1, 2, 3, suffix))
        self.assertEqual(metadata.parse_release_version("0.0.0"), (0, 0, 0, None))
        self.assertEqual(metadata.parse_release_version("12.34.56"), (12, 34, 56, None))

    # Purpose: Prevent the digit-leading defect family from recurring in any prerelease identifier position.
    # Inputs: All one-to-three-character combinations of representative digits, letters and a hyphen.
    # Outputs: An independent numeric-identifier rule determines acceptance in first and subsequent positions.
    def test_short_prerelease_identifier_grammar(self):
        for length in range(1, 4):
            for characters in itertools.product("019aZ-", repeat=length):
                identifier = "".join(characters)
                numeric_leading_zero = identifier.isdecimal() and length > 1 and identifier[0] == "0"
                for suffix in (identifier, "alpha." + identifier):
                    version = "1.2.3-" + suffix
                    with self.subTest(version=version):
                        if numeric_leading_zero:
                            with self.assertRaises(ValueError):
                                metadata.parse_release_version(version)
                        else:
                            self.assertEqual(metadata.parse_release_version(version), (1, 2, 3, suffix))

    # Purpose: Preserve strict numeric and character boundaries while accepting valid mixed identifiers.
    # Inputs: Leading-zero numbers, empty identifiers, Unicode, export injection and unsupported naming forms.
    # Outputs: Every invalid version fails through the public normalizer without producing metadata.
    def test_invalid_versions_and_normalization(self):
        for value in (
            "",
            " ",
            "latest",
            "LATEST",
            "v1.2.3",
            "V1.2.3",
            "01.2.3",
            "1.02.3",
            "1.2.03",
            "1.2",
            "1.2.3-",
            "1.2.3-01",
            "1.2.3-a.01",
            "1.2.3-a..b",
            "1.2.3-a/b",
            "1.2.3-\u03b2",
            "1.2.3+build",
            "1.2.3\nINJECTED=value",
            "1.2.3\x00",
        ):
            with self.subTest(value=value), self.assertRaises(ValueError):
                metadata.validate_release_version(value)
        self.assertEqual(metadata.validate_release_version(" \t1.2.3-1beta\r\n"), "1.2.3-1beta")

    # Purpose: Resolve project defaults only from the intended project and reject malformed default versions.
    # Inputs: Independent multiline, other-project, absent-version and leading-zero CMake declarations.
    # Outputs: The declared default is exact; unsupported declarations raise instead of selecting a wrong version.
    def test_project_version_admission(self):
        self.assertEqual(metadata.read_project_version(self.root), "4.5.6")
        for declaration in ("project(Other VERSION 1.2.3)", "project(FlitzZip)", "project(FlitzZip VERSION 01.2.3)"):
            (self.root / "CMakeLists.txt").write_text(declaration, encoding="utf-8")
            with self.subTest(declaration=declaration), self.assertRaises(ValueError):
                metadata.read_project_version(self.root)

    # Purpose: Prevent implicit collision with either supported tag spelling without requiring CMake for overrides.
    # Inputs: Explicit/default versions and duplicate or whitespace-separated remote tags.
    # Outputs: Existing tags fail; explicit metadata uses the requested version and consistent package identities.
    def test_tag_collisions_and_explicit_override(self):
        tags = self.root / "tags.txt"
        tags.write_text(" 1.2.3 \n\nv4.5.6\n1.2.3\n", encoding="utf-8")
        self.assertEqual(metadata.read_existing_tags(None), set())
        self.assertEqual(metadata.read_existing_tags(tags), {"1.2.3", "v4.5.6"})
        for requested in ("", "4.5.6", "1.2.3"):
            with self.subTest(requested=requested), self.assertRaisesRegex(ValueError, "already exists"):
                metadata.resolve_metadata(
                    requested_version=requested, repo_root=self.root, existing_tags=metadata.read_existing_tags(tags)
                )
        (self.root / "CMakeLists.txt").unlink()
        actual = metadata.resolve_metadata(requested_version="7.8.9-1beta", repo_root=self.root, existing_tags=set())
        self.assertEqual(
            (actual.release_version, actual.release_tag, actual.package_version, actual.package_base),
            ("7.8.9-1beta", "7.8.9-1beta", "7.8.9-1beta", "FlitzZip-7.8.9-1beta-win64"),
        )

    # Purpose: Reject invalid or already published versions before any Actions or JSON output is changed.
    # Inputs: Existing output sentinels and invalid version, injection or tag-collision requests.
    # Outputs: Exit one, an actionable error, no success output and byte-identical output files.
    def test_rejected_main_preserves_all_publication_outputs(self):
        env_file, output_file, json_file = [self.root / name for name in ("env.txt", "output.txt", "metadata.json")]
        tags = self.root / "tags.txt"
        tags.write_text("1.2.3\n", encoding="utf-8")
        for requested in ("1.2.3", "1.2.3-01", "1.2.3\nINJECTED=value"):
            for target in (env_file, output_file, json_file):
                target.write_bytes(b"retain\n")
            stdout, stderr = io.StringIO(), io.StringIO()
            with (
                patch.dict(os.environ, {"GITHUB_ENV": str(env_file), "GITHUB_OUTPUT": str(output_file)}, clear=True),
                contextlib.redirect_stdout(stdout),
                contextlib.redirect_stderr(stderr),
            ):
                status = metadata.main(
                    [
                        "--requested-version",
                        requested,
                        "--existing-tags-file",
                        str(tags),
                        "--json-output",
                        str(json_file),
                        "--repo-root",
                        str(self.root),
                    ]
                )
            self.assertEqual(status, 1)
            self.assertEqual(stdout.getvalue(), "")
            self.assertTrue(stderr.getvalue().startswith("ERROR: "))
            for target in (env_file, output_file, json_file):
                self.assertEqual(target.read_bytes(), b"retain\n")

    # Purpose: Keep default resolution and explicit replacement admission independent of optional export files.
    # Inputs: An owned project, an existing tag file and an explicit allow-existing request.
    # Outputs: Both valid calls succeed without inherited Actions state or accidental local files.
    def test_main_default_and_explicit_existing_version(self):
        tags = self.root / "tags.txt"
        tags.write_text("1.2.3\n", encoding="utf-8")
        with patch.dict(os.environ, {}, clear=True), contextlib.redirect_stdout(io.StringIO()) as stdout:
            self.assertEqual(metadata.main(["--repo-root", str(self.root)]), 0)
            self.assertIn("Release version: 4.5.6\n", stdout.getvalue())
            self.assertEqual(
                metadata.main(
                    [
                        "--requested-version",
                        "1.2.3",
                        "--existing-tags-file",
                        str(tags),
                        "--allow-existing",
                    ]
                ),
                0,
            )
        self.assertEqual({p.name for p in self.root.iterdir()}, {"CMakeLists.txt", "tags.txt"})

    # Purpose: Exercise the actual command entry point and byte-safe Actions exports rather than inferred CLI hits.
    # Inputs: A valid mixed prerelease, independent output sentinels and the current interpreter.
    # Outputs: Exact metadata and appended assignments, clean diagnostics and a zero exit without publication.
    def test_real_cli_exports_consistent_metadata(self):
        env_file, output_file, json_file = [self.root / name for name in ("env.txt", "output.txt", "metadata.json")]
        env_file.write_text("EXISTING=keep\n", encoding="utf-8")
        output_file.write_text("existing=keep\n", encoding="utf-8")
        environment = {**os.environ, "GITHUB_ENV": str(env_file), "GITHUB_OUTPUT": str(output_file)}
        result = subprocess.run(
            [
                sys.executable,
                str(Path(metadata.__file__)),
                "--requested-version",
                "7.8.9-1beta",
                "--repo-root",
                str(self.root),
                "--json-output",
                str(json_file),
            ],
            env=environment,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=20,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        expected = {
            "release_version": "7.8.9-1beta",
            "release_tag": "7.8.9-1beta",
            "package_version": "7.8.9-1beta",
            "package_base": "FlitzZip-7.8.9-1beta-win64",
        }
        self.assertEqual(json.loads(json_file.read_text(encoding="utf-8")), expected)
        self.assertEqual(
            output_file.read_text(encoding="utf-8"),
            "existing=keep\n" + "".join(f"{key}={value}\n" for key, value in expected.items()),
        )
        self.assertEqual(
            env_file.read_text(encoding="utf-8"),
            "EXISTING=keep\n" + "".join(f"{key.upper()}={value}\n" for key, value in expected.items()),
        )
        self.assertEqual(
            result.stdout,
            "Release version: 7.8.9-1beta\nRelease tag: 7.8.9-1beta\nPackage base: FlitzZip-7.8.9-1beta-win64\n",
        )

    # Purpose: Refuse malformed CLI structure before resolving or exporting a version.
    # Inputs: Unknown options and a missing requested-version argument.
    # Outputs: Standard usage exit two with no stdout and no files created.
    def test_invalid_cli_shape(self):
        for arguments in (["--unknown"], ["--requested-version"]):
            with (
                self.subTest(arguments=arguments),
                contextlib.redirect_stdout(io.StringIO()) as stdout,
                contextlib.redirect_stderr(io.StringIO()) as stderr,
                self.assertRaises(SystemExit) as failure,
            ):
                metadata.main(arguments)
            self.assertEqual(failure.exception.code, 2)
            self.assertEqual(stdout.getvalue(), "")
            self.assertIn("usage:", stderr.getvalue())
        self.assertEqual({p.name for p in self.root.iterdir()}, {"CMakeLists.txt"})


if __name__ == "__main__":
    unittest.main()
