"""Independent source, inventory and installed-metadata controls for scanner graph publication."""

import copy
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import scanner_dependency_snapshot as tool
from tools.scanner_requirements import locked_requirements


class SnapshotTests(unittest.TestCase):
    def setUp(self):
        """Purpose: Freeze a complete actual-lock fixture.
        Inputs: Tracked requirements.
        Outputs: Independent package inventory.
        """
        self.environment = dict(
            GITHUB_SHA="a" * 40,
            GITHUB_REF="refs/heads/main",
            GITHUB_REPOSITORY="owner/repo",
            GITHUB_RUN_ID="123",
            GITHUB_RUN_ATTEMPT="2",
        )
        self.pins = locked_requirements((tool.ROOT / tool.MANIFEST).read_text(encoding="utf-8"))
        graph = {
            tool.package_url(name, record[0]): dict(
                package_url=tool.package_url(name, record[0]),
                scope="development",
                relationship="direct",
                dependencies=[],
            )
            for name, record in self.pins.items()
        }
        self.snapshot = tool.context(self.environment) | dict(
            scanned="2026-10-22T12:00:00Z",
            manifests={
                tool.MANIFEST: dict(name=tool.MANIFEST, file=dict(source_location=tool.MANIFEST), resolved=graph)
            },
        )

    def test_complete_lock_inventory_and_local_version_are_preserved(self):
        """Purpose: Prevent private distribution omission.
        Inputs: Actual complete lock.
        Outputs: Every exact escaped PURL admitted.
        """
        tool.validate_snapshot(self.snapshot, self.environment)
        graph = self.snapshot["manifests"][tool.MANIFEST]["resolved"]
        self.assertEqual(len(graph), len(self.pins))
        self.assertIn("pkg:pypi/semgrep@1.181.0%2Bflitzzip.security2", graph)
        self.assertIn("pkg:pypi/opentelemetry-api@1.45.1", graph)
        self.assertNotIn("pkg:pypi/opentelemetry-api@1.37.0", graph)
        self.assertNotIn("file:", json.dumps(self.snapshot))

    def test_changed_sources_runs_types_and_manifest_scope_are_rejected(self):
        """Purpose: Prevent stale or misplaced publication.
        Inputs: Altered source/run/manifest records.
        Outputs: Refusal.
        """
        for key, value in (
            ("sha", "b" * 40),
            ("ref", "refs/heads/other"),
            ("version", False),
            ("scanned", "2026-02-31T00:00:00Z"),
            ("scanned", None),
            ("scanned", "2026-10-22"),
            ("manifests", {}),
        ):
            with self.subTest(key=key), self.assertRaises(ValueError):
                tool.validate_snapshot(self.snapshot | {key: value}, self.environment)
        for key in ("metadata", "job", "detector"):
            with self.subTest(key=key), self.assertRaises(ValueError):
                tool.validate_snapshot(self.snapshot | {key: {}}, self.environment)
        bad = copy.deepcopy(self.snapshot)
        bad["manifests"][tool.MANIFEST]["file"]["source_location"] = "another/requirements.txt"
        with self.assertRaises(ValueError):
            tool.validate_snapshot(bad, self.environment)

    def test_partial_inventories_wrong_versions_and_foreign_edges_are_rejected(self):
        """Purpose: Never conceal an installed distribution.
        Inputs: Missing/extra nodes and altered edges.
        Outputs: Refusal.
        """
        for mutation in ("missing", "extra", "identity", "scope", "edge", "duplicate"):
            bad = copy.deepcopy(self.snapshot)
            graph = bad["manifests"][tool.MANIFEST]["resolved"]
            first = next(iter(graph))
            if mutation == "missing":
                del graph[first]
            elif mutation == "extra":
                graph["pkg:pypi/foreign@1"] = graph[first]
            elif mutation in ("identity", "scope"):
                graph[first]["package_url" if mutation == "identity" else "scope"] = "wrong"
            else:
                graph[first]["dependencies"] = ["pkg:pypi/foreign@1"] if mutation == "edge" else [first, first]
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                tool.validate_snapshot(bad, self.environment)

    def test_publication_context_never_accepts_a_partial_identity(self):
        """Purpose: Prevent unbound manual publication.
        Inputs: Missing Actions identities.
        Outputs: Explicit refusal.
        """
        for key in self.environment:
            with self.subTest(key=key), self.assertRaises(ValueError):
                tool.context(self.environment | {key: ""})
        for ref in ("refs/heads/feature", "refs/tags/v1.0.0"):
            with self.subTest(ref=ref):
                self.assertEqual(tool.context(self.environment | {"GITHUB_REF": ref})["ref"], ref)

    def test_cli_retains_evidence_and_refuses_overwrite_or_malformed_input(self):
        """Purpose: Exercise the actual artifact entry point without installing or publishing anything.
        Inputs: Owned path, complete inert graph and malformed documents. Outputs: Retained evidence or refusal.
        """
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, self.environment):
            path = Path(directory) / "snapshot.json"
            graph = self.snapshot["manifests"][tool.MANIFEST]["resolved"]
            with (
                patch.object(sys, "argv", ["snapshot", "build", str(path)]),
                patch.object(tool, "installed_graph", return_value=graph),
            ):
                tool.main()
                original = path.read_bytes()
                with self.assertRaises(FileExistsError):
                    tool.main()
                self.assertEqual(path.read_bytes(), original)
            with patch.object(sys, "argv", ["snapshot", "validate", str(path)]):
                tool.main()
                for malformed in ('{"version":0,"version":1}', '{"value":NaN}', "x" * (1024 * 1024 + 1)):
                    path.write_text(malformed, encoding="utf-8")
                    with self.subTest(input_size=len(malformed)), self.assertRaises(ValueError):
                        tool.main()

    def test_publication_is_isolated_from_scanning_and_bound_to_the_default_branch(self):
        """Purpose: Prevent privilege or artifact-source expansion.
        Inputs: Actual publication and scanner jobs. Outputs: Exact-run validation precedes authenticated upload.
        """
        workflow = (tool.ROOT / ".github/workflows/security-code-scanning.yml").read_text(encoding="utf-8")
        scanner = workflow.split("\n  semgrep:\n", 1)[1].split("\n  scanner-dependencies:\n", 1)[0]
        publisher = workflow.split("\n  scanner-dependencies:\n", 1)[1].split("\n  devskim:\n", 1)[0]
        self.assertIn("contents: read", scanner)
        self.assertNotIn("contents: write", scanner)
        self.assertIn("needs: semgrep", publisher)
        self.assertIn("!cancelled() && needs.semgrep.outputs.dependency_artifact != ''", publisher)
        self.assertIn("github.ref == format('refs/heads/{0}', github.event.repository.default_branch)", publisher)
        self.assertIn("artifact-ids: ${{ needs.semgrep.outputs.dependency_artifact }}", publisher)
        self.assertNotIn("run-id:", publisher)
        self.assertIn("persist-credentials: false", publisher)
        self.assertLess(
            publisher.index("scanner_dependency_snapshot validate"), publisher.index("gh api --method POST")
        )
        self.assertEqual(publisher.count("GH_TOKEN:"), 1)
        self.assertLess(publisher.index("- name: Submit the actual installed graph"), publisher.index("GH_TOKEN:"))

    def test_module_entry_point_propagates_admission_failure(self):
        """Purpose: Verify real process exit status, rather than only an imported function's exception.
        Inputs: Complete snapshot followed by a stale version. Outputs: Success only for the admitted document.
        """
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "snapshot.json"
            for version, expected in ((0, 0), (1, 1)):
                path.write_text(json.dumps(self.snapshot | {"version": version}), encoding="utf-8")
                result = subprocess.run(
                    [sys.executable, "-B", "-m", "tools.scanner_dependency_snapshot", "validate", str(path)],
                    env=os.environ | self.environment,
                    capture_output=True,
                    text=True,
                    timeout=15,
                    check=False,
                )
                with self.subTest(version=version):
                    self.assertEqual(result.returncode, expected, result.stderr)
                    if expected:
                        self.assertIn("source, recipe or run changed", result.stderr)


if __name__ == "__main__":
    unittest.main()
