"""Offline gates for identified scanner pins and the production hash-locked install."""

import re
import unittest

from tools.scanner_requirements import validate_locked_packages, validate_requirements_options
from tools.semgrep_distribution import recipe
from tools.test_semgrep_runtime import REQUIREMENTS, pinned_version


class InstallationTests(unittest.TestCase):
    # Purpose: Keep every active certifi tooling lane above the compromised emSign trust-store range.
    # Inputs: Both Linux resolver inputs and all three installed dependency locks, including the crawler.
    # Outputs: Exact locks meet the advisory floor and both reusable Linux inputs retain that floor for refreshes.
    def test_certificate_authority_advisory_floor(self):
        floor = (2026, 10, 19)
        root = REQUIREMENTS.parents[1]
        paths = [REQUIREMENTS / f"requirements-{lane}-linux.txt" for lane in ("semgrep", "gvm-tools")]
        paths.append(root / "tools/requirements/crawl4ai.txt")
        for path in paths:
            version = pinned_version(path.read_text(encoding="utf-8"), "certifi")
            with self.subTest(lock=path.name):
                self.assertRegex(version, r"^[0-9]+\.[0-9]+\.[0-9]+$")
                self.assertGreaterEqual(tuple(map(int, version.split("."))), floor)
        for lane in ("semgrep", "gvm-tools"):
            contents = (REQUIREMENTS / f"requirements-{lane}-linux.in").read_text(encoding="utf-8")
            minimum = re.findall(r"(?m)^certifi>=([0-9]+\.[0-9]+\.[0-9]+)$", contents)
            with self.subTest(input=lane):
                self.assertEqual(len(minimum), 1)
                self.assertGreaterEqual(tuple(map(int, minimum[0].split("."))), floor)

    # Purpose: Keep the reviewed scanner packaging revision and official fixed dependencies aligned.
    # Inputs: Requirements, artifact provenance and complete lock. Outputs: Exact identities and derived hashes.
    def test_official_configuration_parity(self):
        source = (REQUIREMENTS / "requirements-semgrep-linux.in").read_text(encoding="utf-8")
        locked = (REQUIREMENTS / "requirements-semgrep-linux.txt").read_text(encoding="utf-8")
        for package in ("semgrep", "pyjwt"):
            self.assertEqual(pinned_version(source, package), pinned_version(locked, package))
        data = recipe()
        self.assertEqual(pinned_version(locked, "semgrep"), data["version"])
        self.assertEqual(pinned_version(locked, "mcp"), data["mcp_after"])
        self.assertIn("mcp>=" + data["mcp_after"], source)
        for pin in data["wheels"].values():
            self.assertIn("--hash=sha256:" + pin["derived_sha256"], locked)
        self.assertNotIn("+", pinned_version(locked, "pyjwt"))
        self.assertIn("pyjwt[crypto]==", source)
        for contents in (source, locked):
            validate_requirements_options(contents)

    # Purpose: Reject added install locations, duplicate options and malformed tokens rather than matching substrings.
    # Inputs: Synthetic option mutations. Outputs: Requires explicit refusal of every unsupported configuration.
    def test_requirements_option_admission(self):
        valid = "--index-url https://pypi.org/simple\n--only-binary=:all:\nsemgrep==1.179.0\n"
        validate_requirements_options(valid)
        validate_requirements_options(valid.replace("--only-binary=:all:", "--only-binary :all:"))
        mutations = (
            "",
            valid + "--unsupported-location https://invalid.example\n",
            valid + "--index-url https://pypi.org/simple\n",
            valid + "--only-binary=:all:\n",
            valid + "--index-url\n",
            valid + "--hash=sha256:invalid\n",
            valid + "https://invalid.example/package.whl\n",
            valid.replace("https://pypi.org/simple", "https://invalid.example"),
            valid.replace("--only-binary=:all:", "--only-binary=:none:"),
        )
        for contents in mutations:
            with self.subTest(contents=contents), self.assertRaises(ValueError):
                validate_requirements_options(contents)

    # Purpose: Require an exact artifact digest for every dependency rather than only top-level packages.
    # Inputs: generated pip lock tokens. Outputs: exact pins and SHA-256 checks for the full graph.
    def test_every_locked_dependency_has_hashes(self):
        contents = (REQUIREMENTS / "requirements-semgrep-linux.txt").read_text(encoding="utf-8")
        packages = validate_locked_packages(contents)
        self.assertIn("semgrep", packages)
        self.assertIn("pyjwt", packages)
        self.assertIn("cryptography", packages)

    # Purpose: Accept standard extras without losing duplicate, exact-version or hash checks.
    # Inputs: Valid locks and independent malformed cases. Outputs: Correct identity or explicit rejection.
    def test_lock_extras_and_distribution_identity(self):
        policy = "--index-url https://pypi.org/simple\n--only-binary :all:\n"
        digest = "--hash=sha256:" + "a" * 64
        entry = "example[feature-one,feature_two]==1.2.3 " + digest + "\n"
        self.assertEqual(validate_locked_packages(policy + entry), {"example"})
        for malformed in (
            entry + "example==1.2.3 " + digest,
            entry.replace(digest, ""),
            entry.replace(digest, "--hash=sha256:invalid"),
            entry.replace("==1.2.3", ">=1.2.3"),
            entry.replace("[feature-one,feature_two]", "[]"),
            entry.replace("[feature-one,feature_two]", "[feature-one,]"),
        ):
            with self.subTest(malformed=malformed), self.assertRaises(ValueError):
                validate_locked_packages(policy + malformed)

    # Purpose: Refuse missing, ambiguous or nonexact package identities in runtime gates.
    # Inputs: well-formed and deliberately malformed requirements text. Outputs: exact pin or explicit rejection.
    def test_pin_reader(self):
        self.assertEqual(pinned_version("pyjwt[crypto]==2.15.1\n", "pyjwt"), "2.15.1")
        self.assertEqual(pinned_version("semgrep==1.179.0 \\\n --hash=sha256:test\n", "semgrep"), "1.179.0")
        for contents in ("", "semgrep>=1.179.0\n", "semgrep==1.179.*\n", "semgrep==1.179.0\nsemgrep==1.179.0\n"):
            with self.subTest(contents=contents), self.assertRaises(ValueError):
                pinned_version(contents, "semgrep")

    # Purpose: Keep production CI on normal dependency resolution, exact hashes and real runtime controls.
    # Inputs: tracked security workflow. Outputs: no-bypass and unchanged coverage/SARIF contracts.
    def test_production_installation_contract(self):
        workflow = (REQUIREMENTS.parents[1] / ".github/workflows/security-code-scanning.yml").read_text(
            encoding="utf-8"
        )
        for command in (
            "python -m unittest tools.test_semgrep_installation",
            "tools.test_semgrep_distribution",
            'python -m tools.semgrep_distribution --output "$RUNNER_TEMP/semgrep-distribution"',
            '--find-links "$RUNNER_TEMP/semgrep-distribution"',
            "--require-hashes",
            "--only-binary=:all:",
            "-r .github/requirements/requirements-semgrep-linux.txt",
            '"$RUNNER_TEMP/semgrep/bin/python" -m pip check',
            '"$RUNNER_TEMP/semgrep/bin/python" -m unittest tools.test_semgrep_runtime',
            "--no-git-ignore",
            "--disable-nosem",
            "--config p/default",
            "--config p/github-actions",
            "--sarif",
            "python -m tools.semgrep_coverage",
        ):
            self.assertIn(command, workflow)
        for bypass in ("--no-deps", "prepare_semgrep_wheel", "scanner-wheels"):
            self.assertNotIn(bypass, workflow)


if __name__ == "__main__":
    unittest.main()
