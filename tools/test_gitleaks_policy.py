"""Keep reviewed public-checksum exceptions separate from authentication material."""

import hashlib
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

    # Purpose: Require each admitted value to be a real source hash, with no neighboring credential bypass.
    # Inputs: Current source bytes and production regexes. Outputs: Both source fields match; changed/auth values fail.
    def test_exact_public_source_values_and_authentication_boundaries(self):
        for name in ("tests/cpp/sdk_byte_access_checks.hpp", "tests/cpp/test_sdk_byte_access.cpp"):
            digest = hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
            positive = f'"{name}": "{digest}",'
            matches = [item for item in self.allowlists if re.fullmatch(item["regexes"][0], positive)]
            self.assertEqual(len(matches), 1, "Source checksum changed: require a fresh provenance review")
            pattern = matches[0]["regexes"][0]
            for negative in (
                f'"api_key": "{digest}",',
                f'"{name}": "{hashlib.sha256(b"different public fixture").hexdigest()}",',
                positive + f' "api_key": "{digest}"',
                f'"other/{name}": "{digest}"',
            ):
                self.assertIsNone(re.fullmatch(pattern, negative))

    # Purpose: Reject exclusions outside the exact dated benchmark record family.
    # Inputs: Representative record paths and neighboring product/configuration paths. Outputs: Bounded admission only.
    def test_dated_record_paths_only(self):
        for item in self.allowlists:
            pattern = item["paths"][0]
            for positive in (
                "docs/benchmarks/data/beta-effort-size-L4-20261003.json",
                "docs/benchmarks/data/beta-mixed-l5-b8192-20261003.json",
            ):
                self.assertIsNotNone(re.fullmatch(pattern, positive))
            for negative in (
                "src/benchmark.json",
                ".github/secrets.json",
                "docs/benchmarks/data/beta-effort-size-L4-20261004.json",
                "docs/benchmarks/data/beta-effort-size-L4-20261003.json.backup",
                "other/docs/benchmarks/data/beta-effort-size-L4-20261003.json",
            ):
                self.assertIsNone(re.fullmatch(pattern, negative))


if __name__ == "__main__":
    unittest.main()
