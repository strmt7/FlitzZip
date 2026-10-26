"""Keep reviewed public-checksum exceptions separate from authentication material."""

import hashlib
import json
import re
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

    def test_exact_public_source_values_and_authentication_boundaries(self):
        """Match the retained build receipt without requiring historical Git objects.

        The receipt records the original source checksums. This test can also run
        from a source archive; attribution changes do not alter its evidence.
        """
        fixture = ROOT / "docs/benchmarks/data/fixtures/native-v1.json"
        record = json.loads(fixture.read_text(encoding="utf-8-sig"))
        source_hashes = record["native_build_receipt"]["inputs"]["files"]
        for name in ("tests/cpp/sdk_byte_access_checks.hpp", "tests/cpp/test_sdk_byte_access.cpp"):
            digest = source_hashes[name]
            self.assertRegex(digest, r"^[a-f0-9]{64}$")
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
