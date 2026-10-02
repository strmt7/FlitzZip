"""Offline gates for official scanner pins and the production hash-locked install."""

import re
import shlex
import unittest

from tools.test_semgrep_runtime import REQUIREMENTS, pinned_version


class InstallationTests(unittest.TestCase):
    # Purpose: Keep direct scanner/JWT pins aligned with the complete official dependency lock.
    # Inputs: tracked requirements input and lock. Outputs: matching identities without local metadata revisions.
    def test_official_configuration_parity(self):
        source = (REQUIREMENTS / "requirements-semgrep-linux.in").read_text(encoding="utf-8")
        locked = (REQUIREMENTS / "requirements-semgrep-linux.txt").read_text(encoding="utf-8")
        for package in ("semgrep", "pyjwt"):
            self.assertEqual(pinned_version(source, package), pinned_version(locked, package))
            self.assertNotIn("+", pinned_version(locked, package))
        self.assertIn("pyjwt[crypto]==", source)
        for contents in (source, locked):
            self.assertIn("--index-url https://pypi.org/simple\n", contents)
            self.assertIn("--only-binary :all:\n" if contents == locked else "--only-binary=:all:\n", contents)
            self.assertNotIn("--find-links", contents)
            self.assertNotIn("--extra-index-url", contents)

    # Purpose: Require an exact artifact digest for every dependency rather than only top-level packages.
    # Inputs: generated pip lock tokens. Outputs: exact pins and SHA-256 checks for the full graph.
    def test_every_locked_dependency_has_hashes(self):
        contents = (REQUIREMENTS / "requirements-semgrep-linux.txt").read_text(encoding="utf-8")
        tokens = shlex.split(contents.replace("\\\n", ""), comments=True)
        self.assertEqual(tokens[:4], ["--index-url", "https://pypi.org/simple", "--only-binary", ":all:"])
        packages = set()
        index = 4
        while index < len(tokens):
            package = re.fullmatch(r"([a-z0-9-]+)==([0-9][a-zA-Z0-9.-]*)", tokens[index])
            self.assertIsNotNone(package, tokens[index])
            self.assertNotIn(package[1], packages)
            packages.add(package[1])
            index += 1
            hashes = 0
            while index < len(tokens) and tokens[index].startswith("--hash="):
                self.assertRegex(tokens[index], r"^--hash=sha256:[a-f0-9]{64}$")
                hashes += 1
                index += 1
            self.assertGreater(hashes, 0, package[1])
        self.assertIn("semgrep", packages)
        self.assertIn("pyjwt", packages)
        self.assertIn("cryptography", packages)

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
