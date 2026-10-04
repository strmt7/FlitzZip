"""Keep reviewed public-checksum exceptions separate from authentication material."""

import hashlib
import json
import re
import subprocess
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class GitleaksPolicyTests(unittest.TestCase):
    # Purpose: Load the production scanner policy, never a copied fixture policy.
    # Inputs: Tracked TOML. Outputs: Fresh configuration for each boundary test.
    def setUp(self):
        self.policy = tomllib.loads((ROOT / ".github/gitleaks.toml").read_text(encoding="utf-8"))
        self.allowlists = self.policy["rules"][0]["allowlists"]

    # Purpose: Preserve every upstream detector and constrain exceptions to one reviewed rule.
    # Inputs: Production policy. Outputs: Assertions reject rule removal and broad allowlist criteria.
    def test_default_detection_and_narrow_conditions(self):
        self.assertEqual(set(self.policy), {"title", "extend", "rules"})
        self.assertEqual(self.policy["extend"], {"useDefault": True})
        self.assertEqual(len(self.policy["rules"]), 1)
        self.assertEqual(set(self.policy["rules"][0]), {"id", "allowlists"})
        self.assertEqual(self.policy["rules"][0]["id"], "generic-api-key")
        self.assertEqual(len(self.allowlists), 2)
        for allowed in self.allowlists:
            self.assertEqual(set(allowed), {"description", "condition", "regexTarget", "paths", "regexes"})
            self.assertEqual(allowed["condition"], "AND")
            self.assertEqual(allowed["regexTarget"], "line")
            self.assertEqual(len(allowed["paths"]), 1)
            self.assertEqual(len(allowed["regexes"]), 1)

    # Purpose: Bind historical public checksums to the report's exact committed source bytes.
    # Inputs: The immutable benchmark source revision and production regexes.
    # Outputs: Both original source fields match; changed or authentication values remain rejected.
    def test_exact_public_source_values_and_authentication_boundaries(self):
        record = json.loads((ROOT / "docs/benchmarks/data/beta-effort-size-L4-20261003.json").read_text("utf-8-sig"))
        revision = record["source_commit"]
        self.assertRegex(revision, r"\A[0-9a-f]{40}\Z")
        for name in ("tests/cpp/sdk_byte_access_checks.hpp", "tests/cpp/test_sdk_byte_access.cpp"):
            source = subprocess.check_output(["git", "show", f"{revision}:{name}"], cwd=ROOT, timeout=10)
            digest = hashlib.sha256(source).hexdigest()
            positive = f'"{name}": "{digest}",'
            matches = [item for item in self.allowlists if re.fullmatch(item["regexes"][0], positive)]
            self.assertEqual(len(matches), 1, "Historical public-source identity changed: require provenance review")
            pattern = matches[0]["regexes"][0]
            for negative in (
                f'"api_key": "{digest}",',
                f'"{name}": "{hashlib.sha256(b"different public fixture").hexdigest()}",',
                positive + f' "api_key": "{digest}"',
                f'"other/{name}": "{digest}"',
            ):
                self.assertIsNone(re.fullmatch(pattern, negative))

    # Purpose: Admit reviewed values only in the passive report role without date-specific maintenance.
    # Inputs: Representative record paths and neighboring product/configuration paths. Outputs: Bounded admission only.
    def test_report_directory_paths_only(self):
        for item in self.allowlists:
            pattern = item["paths"][0]
            for positive in (
                "docs/benchmarks/data/beta-effort-size-L4-20261003.json",
                "docs/benchmarks/data/beta-mixed-l5-b8192-20261003.json",
                "docs/benchmarks/data/beta-effort-size-L4-20261004.json",
                "docs/benchmarks/data/future/report.json",
            ):
                self.assertIsNotNone(re.fullmatch(pattern, positive))
            for negative in (
                "src/benchmark.json",
                ".github/secrets.json",
                "docs/benchmarks/corpora/source-pin.json",
                "tests/fixtures/report.json",
                "docs/benchmarks/data/beta-effort-size-L4-20261003.json.backup",
                "other/docs/benchmarks/data/beta-effort-size-L4-20261003.json",
            ):
                self.assertIsNone(re.fullmatch(pattern, negative))


if __name__ == "__main__":
    unittest.main()
