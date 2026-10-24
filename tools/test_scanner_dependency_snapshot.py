"""Independent source, inventory and installed-metadata controls for scanner graph publication."""

import copy
import importlib.metadata
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


class InstalledGraphTests(unittest.TestCase):
    def setUp(self):
        """Purpose: Use real distribution metadata with controlled lookup, never package imports or network access.
        Inputs: Owned dist-info directories. Outputs: Native metadata readers and an independently specified lock.
        """
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.pins = {"root": ("1.0", ()), "leaf": ("2.0", ()), "bridge": ("1.0", ())}
        self.distributions = {}
        self.write_distribution("root", "Root", "1.0", ["Bridge[feature]>=1", "Leaf==2; extra == 'unused'"])
        self.write_distribution("leaf", "Leaf", "2.0", [])
        self.write_distribution("bridge", "Bridge", "1.0", ["Leaf>=2; extra == 'feature'", "Root; extra == 'feature'"])

    def write_distribution(self, key, name, version, requirements):
        """Purpose: Frame actual metadata independently of graph generation.
        Inputs: Public fixture fields. Outputs: Standard-library distribution reader backed by owned files.
        """
        directory = self.root / (key + ".dist-info")
        directory.mkdir(exist_ok=True)
        text = f"Metadata-Version: 2.1\nName: {name}\nVersion: {version}\n"
        text += "".join("Requires-Dist: " + value + "\n" for value in requirements)
        (directory / "METADATA").write_text(text, encoding="utf-8")
        self.distributions[key] = importlib.metadata.PathDistribution(directory)

    def graph(self, pins=None):
        """Purpose: Inject only installed-distribution lookup while retaining real parsing and graph traversal.
        Inputs: Exact fixture lock. Outputs: Production graph or its original admission error.
        """
        with patch.object(tool.importlib.metadata, "distribution", side_effect=self.distributions.__getitem__):
            return tool.installed_graph(self.pins if pins is None else pins)

    def test_late_extra_activation_revisits_dependencies_and_terminates_cycles(self):
        """Purpose: Reject one-pass traversal that silently loses newly activated transitive requirements.
        Inputs: Bridge visited before Root activates its feature; real metadata includes a cycle and inactive edge.
        Outputs: Independently specified complete, sorted graph with no false inactive dependency.
        """
        graph = self.graph()
        expected = {
            "pkg:pypi/root@1.0": ["pkg:pypi/bridge@1.0"],
            "pkg:pypi/bridge@1.0": ["pkg:pypi/leaf@2.0", "pkg:pypi/root@1.0"],
            "pkg:pypi/leaf@2.0": [],
        }
        self.assertEqual(set(graph), set(expected))
        for purl, dependencies in expected.items():
            self.assertEqual(
                graph[purl],
                dict(package_url=purl, dependencies=dependencies, scope="development", relationship="direct"),
            )

    def test_activated_missing_conflicting_and_direct_url_edges_fail_closed(self):
        """Purpose: Prevent a partial or redirected dependency graph from being submitted as complete.
        Inputs: Hidden missing child, incompatible version and URL requirement activated after the first visit.
        Outputs: Exact edge refusal, followed by valid reuse with restored metadata.
        """
        for requirement in (
            "Absent; extra == 'feature'",
            "Leaf>=3; extra == 'feature'",
            "Leaf @ https://example.invalid/leaf.whl ; extra == 'feature'",
        ):
            with self.subTest(requirement=requirement):
                self.write_distribution("bridge", "Bridge", "1.0", [requirement])
                with self.assertRaisesRegex(ValueError, "dependency edge is missing or conflicts"):
                    self.graph()
        self.write_distribution("bridge", "Bridge", "1.0", ["Leaf==2; extra == 'feature'"])
        self.assertEqual(self.graph()["pkg:pypi/bridge@1.0"]["dependencies"], ["pkg:pypi/leaf@2.0"])

    def test_empty_inventory_and_installed_identity_drift_are_rejected(self):
        """Purpose: Bind every submitted distribution to its exact admitted name and version.
        Inputs: Empty lock, wrong installed name and wrong version. Outputs: Refusal before publication and recovery.
        """
        with self.assertRaisesRegex(ValueError, "cannot be empty"):
            self.graph({})
        for name, version in (("Foreign", "1.0"), ("Root", "1.1")):
            self.write_distribution("root", name, version, [])
            with (
                self.subTest(name=name, version=version),
                self.assertRaisesRegex(ValueError, "disagrees with its lock"),
            ):
                self.graph()
        self.write_distribution("root", "Root", "1.0", ["Bridge[feature]>=1"])
        self.assertEqual(len(self.graph()), 3)


if __name__ == "__main__":
    unittest.main()
