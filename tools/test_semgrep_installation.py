"""Offline gates for official scanner pins and the production hash-locked install."""

import re
import shlex
import unittest

from tools.test_semgrep_runtime import REQUIREMENTS, pinned_version


# Purpose: Admit only the scanner's single official index, binary policy and bounded package/hash tokens.
# Inputs: Tracked requirements input or lock text. Outputs: Returns normally or raises ValueError on unknown syntax.
def validate_requirements_options(contents: str) -> None:
    tokens = shlex.split(contents.replace("\\\n", ""), comments=True)
    options = {}
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in ("--index-url", "--only-binary"):
            if index + 1 == len(tokens):
                raise ValueError("Missing requirements option value")
            name, value = token, tokens[index + 1]
            index += 2
        elif token == "--only-binary=:all:":
            name, value = "--only-binary", ":all:"
            index += 1
        elif re.fullmatch(r"--hash=sha256:[a-f0-9]{64}", token) or re.fullmatch(
            r"[a-z][a-z0-9-]*(?:\[[a-z0-9,-]+\])?(?:==|>=)[0-9][a-zA-Z0-9.-]*", token
        ):
            index += 1
            continue
        else:
            raise ValueError("Unsupported scanner requirements syntax")
        if name in options:
            raise ValueError("Duplicate requirements option")
        options[name] = value
    if options != {"--index-url": "https://pypi.org/simple", "--only-binary": ":all:"}:
        raise ValueError("Scanner requirements need the official index and binary-only policy")


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
