"""Whole-database C++ security admission and real Git range recurrence contracts."""

import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools import cpp_security_plan as cpp


# Purpose: Prove changed native/query inputs and periodic scans cannot silently skip C++ analysis.
# Inputs: Actual native receipt projection, workflow and isolated real Git histories.
# Outputs: Admission, rejection, output and rename/delete controls pass without compilation or scanning.
class CppSecurityContracts(unittest.TestCase):
    def test_metadata_timeout_kills_the_owned_process_and_fails_closed(self):
        """Purpose: Preserve cleanup. Inputs: Closed pipes with a live child. Outputs: Kill plus bounded ValueError."""
        process = mock.MagicMock()
        process.__enter__.return_value = process
        process.stdout = io.BytesIO(b"metadata")
        process.stderr = io.BytesIO()
        process.poll.return_value = None
        process.wait.side_effect = [subprocess.TimeoutExpired(["fixture"], 5), 0]
        with (
            mock.patch.object(cpp.subprocess, "Popen", return_value=process),
            self.assertRaisesRegex(ValueError, "metadata fixture timed out"),
        ):
            cpp.capture_metadata(["fixture"], "metadata fixture")
        process.kill.assert_called_once_with()
        self.assertEqual(process.wait.call_count, 2)

    def test_unterminated_git_inventory_is_rejected(self):
        """Purpose: Reject partial framing. Inputs: Git metadata without its final NUL. Outputs: No path is admitted."""
        with (
            mock.patch.object(cpp, "capture_metadata", return_value=b"src/example.cpp"),
            self.assertRaisesRegex(ValueError, "inventory is not terminated"),
        ):
            cpp.changed_paths(cpp.ROOT, "a" * 40)

    def test_latest_analysis_is_paginated_and_does_not_replace_newer_attempts(self):
        """Purpose: Preserve latest roles. Inputs: Two API pages. Outputs: Newest CPU and HIP identities win."""

        def record(category, commit):
            # Purpose: Build public analysis metadata. Inputs: Category/SHA. Outputs: Complete fixture record.
            return {"category": category, "commit_sha": commit, "error": "", "warning": ""}

        unrelated = record("/language:actions", "a" * 40)
        pages = [
            [record(cpp.CPP_CATEGORIES[0], "b" * 40), *[unrelated] * 99],
            [record(cpp.CPP_CATEGORIES[0], "a" * 40), record(cpp.CPP_CATEGORIES[1], "c" * 40)],
        ]
        with mock.patch.object(
            cpp, "capture_metadata", side_effect=[json.dumps(page).encode() for page in pages]
        ) as call:
            latest = cpp.latest_cpp_analysis("fixture/repository", "refs/heads/main")
        self.assertEqual(latest[cpp.CPP_CATEGORIES[0]]["commit_sha"], "b" * 40)
        self.assertEqual(latest[cpp.CPP_CATEGORIES[1]]["commit_sha"], "c" * 40)
        self.assertEqual(call.call_count, 2)
        for page, arguments in enumerate(call.call_args_list, 1):
            command = arguments.args[0]
            self.assertEqual(command[:2], ["gh", "api"])
            self.assertIn(f"&page={page}&direction=desc", command[2])
            self.assertIn("ref=refs%2Fheads%2Fmain&tool_name=CodeQL", command[2])

    def test_invalid_analysis_evidence_cannot_establish_reuse(self):
        """Purpose: Reject untrusted evidence. Inputs: Bad pages/identities. Outputs: Error, never skipped analysis."""
        valid = {"category": cpp.CPP_CATEGORIES[0], "commit_sha": "a" * 40, "error": "", "warning": ""}
        for page in [None, {}, [None], [valid] * 101, [{**valid, "commit_sha": "HEAD"}], [{**valid, "error": None}]]:
            with (
                self.subTest(page=page),
                mock.patch.object(cpp, "capture_metadata", return_value=json.dumps(page).encode()),
                self.assertRaises(ValueError),
            ):
                cpp.latest_cpp_analysis("fixture/repository", "refs/heads/main")
        for repository, reference in [("bad/name/extra", "refs/heads/main"), ("fixture/repository", "main")]:
            with self.assertRaises(ValueError):
                cpp.latest_cpp_analysis(repository, reference)
        with (
            mock.patch.object(cpp, "capture_metadata", side_effect=ValueError("API unavailable")),
            self.assertRaisesRegex(ValueError, "API unavailable"),
        ):
            cpp.latest_cpp_analysis("fixture/repository", "refs/heads/main")

    def test_incomplete_analysis_selects_both_databases_with_bounded_lookup(self):
        """Purpose: Retain coverage. Inputs: Missing/failed/warned roles. Outputs: Both configurations admitted."""
        valid = {"category": cpp.CPP_CATEGORIES[0], "commit_sha": "a" * 40, "error": "", "warning": ""}
        for latest in [
            {},
            {cpp.CPP_CATEGORIES[0]: valid},
            {category: {**valid, "category": category, "warning": "partial"} for category in cpp.CPP_CATEGORIES},
            {category: {**valid, "category": category, "error": "failed"} for category in cpp.CPP_CATEGORIES},
        ]:
            with (
                mock.patch.object(cpp, "latest_cpp_analysis", return_value=latest),
                mock.patch.object(cpp, "changed_paths") as paths,
            ):
                plan = cpp.qualify_analysis_reuse(cpp.select_cpp([]), "fixture/repository", "refs/heads/main")
            self.assertTrue(plan["codeql_cpp"])
            self.assertTrue(plan["codeql_hip_host"])
            paths.assert_not_called()
        unrelated = {**valid, "category": "/language:actions"}
        with mock.patch.object(cpp, "capture_metadata", return_value=json.dumps([unrelated] * 100).encode()) as call:
            self.assertEqual(cpp.latest_cpp_analysis("fixture/repository", "refs/heads/main"), {})
            self.assertEqual(call.call_count, 32)
        with mock.patch.object(cpp, "latest_cpp_analysis") as inventory:
            cpp.qualify_analysis_reuse(cpp.select_cpp(["src/changed.cpp"]), "fixture/repository", "refs/heads/main")
            inventory.assert_not_called()

    def test_real_cancelled_native_analysis_survives_later_documentation_push(self):
        """Purpose: Reproduce lost coverage. Inputs: Native then docs commits. Outputs: Stale HIP reselects both."""
        with tempfile.TemporaryDirectory(prefix="flitzzip-cancelled-analysis-") as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "--quiet", directory], check=True)
            for key, value in [("user.name", "Contract Fixture"), ("user.email", "fixture@example.invalid")]:
                subprocess.run(["git", "-C", directory, "config", key, value], check=True)
            (root / "src").mkdir()
            commits = []
            for name, text in [("src/codec.cpp", "old\n"), ("src/codec.cpp", "new\n"), ("README.md", "docs\n")]:
                (root / name).write_text(text, encoding="utf-8")
                subprocess.run(["git", "-C", directory, "add", name], check=True)
                subprocess.run(["git", "-C", directory, "commit", "--quiet", "-m", "Fixture update"], check=True)
                commits.append(
                    subprocess.check_output(["git", "-C", directory, "rev-parse", "HEAD"], text=True).strip()
                )
            pushed = cpp.event_plan("push", commits[1], root)
            self.assertFalse(pushed["codeql_cpp"])
            latest = {
                category: {"category": category, "commit_sha": commits[index], "error": "", "warning": ""}
                for category, index in zip(cpp.CPP_CATEGORIES, [1, 0], strict=True)
            }
            with (
                mock.patch.object(cpp, "latest_cpp_analysis", return_value=latest),
                mock.patch.object(cpp, "has_qualified_cpp_workflow", return_value=True),
            ):
                self.assertTrue(
                    cpp.qualify_analysis_reuse(pushed, "fixture/repository", "refs/heads/main", root)["codeql_cpp"]
                )
            latest[cpp.CPP_CATEGORIES[1]]["commit_sha"] = commits[1]
            with (
                mock.patch.object(cpp, "latest_cpp_analysis", return_value=latest),
                mock.patch.object(cpp, "has_qualified_cpp_workflow", return_value=True),
            ):
                self.assertFalse(
                    cpp.qualify_analysis_reuse(pushed, "fixture/repository", "refs/heads/main", root)["codeql_cpp"]
                )
            latest[cpp.CPP_CATEGORIES[1]]["commit_sha"] = "f" * 40
            with (
                mock.patch.object(cpp, "latest_cpp_analysis", return_value=latest),
                mock.patch.object(cpp, "has_qualified_cpp_workflow", return_value=True),
                self.assertRaisesRegex(ValueError, "Git range failed"),
            ):
                cpp.qualify_analysis_reuse(pushed, "fixture/repository", "refs/heads/main", root)

    def test_cancelled_or_skipped_workflow_cannot_admit_uploaded_analysis(self):
        """Purpose: Qualify consumers. Inputs: Actual workflow/job states. Outputs: Both successful jobs required."""
        run = {"id": 17, "head_sha": "a" * 40, "head_branch": "main", "status": "completed", "conclusion": "success"}
        jobs = [
            {"name": f"CodeQL C++ ({role})", "status": "completed", "conclusion": "success"}
            for role in ["CPU", "HIP host"]
        ]
        for conclusion in ["cancelled", "failure", None]:
            with mock.patch.object(
                cpp, "capture_metadata", return_value=json.dumps([{**run, "conclusion": conclusion}]).encode()
            ):
                self.assertFalse(cpp.has_qualified_cpp_workflow("fixture/repository", "refs/heads/main", "a" * 40))
        for job_page, expected in [
            (jobs, True),
            (jobs[:1], False),
            (jobs + [jobs[0]], False),
            ([jobs[0], {**jobs[1], "conclusion": "skipped"}], False),
            ([{"name": "codeql-cpp", "status": "completed", "conclusion": "skipped"}], False),
            ([{**job, "name": job["name"] + " / Other"} for job in jobs], False),
            ([jobs[0], {**jobs[1], "conclusion": "failure"}], False),
        ]:
            with mock.patch.object(
                cpp, "capture_metadata", side_effect=[json.dumps([run]).encode(), json.dumps(job_page).encode()]
            ):
                self.assertEqual(
                    cpp.has_qualified_cpp_workflow("fixture/repository", "refs/heads/main", "a" * 40), expected
                )
        latest = {
            category: {"category": category, "commit_sha": "a" * 40, "error": "", "warning": ""}
            for category in cpp.CPP_CATEGORIES
        }
        with (
            mock.patch.object(cpp, "latest_cpp_analysis", return_value=latest),
            mock.patch.object(cpp, "changed_paths", return_value=[]),
            mock.patch.object(cpp, "has_qualified_cpp_workflow", return_value=False),
        ):
            self.assertTrue(
                cpp.qualify_analysis_reuse(cpp.select_cpp([]), "fixture/repository", "refs/heads/main")["codeql_cpp"]
            )

    def test_every_native_input_root_and_file_selects_whole_database(self):
        """Purpose: Preserve input coverage. Inputs: Actual receipt projection. Outputs: Every input admits CodeQL."""
        for name in [*cpp.INPUT_FILES, *(root + "/future.input" for root in cpp.INPUT_ROOTS)]:
            with self.subTest(path=name):
                self.assertTrue(cpp.select_cpp([name])["codeql_cpp"])
                self.assertTrue(cpp.select_cpp([name])["codeql_hip_host"])
                self.assertTrue(cpp.select_cpp([name])["whole_database"])

    def test_invalid_workflow_metadata_cannot_establish_qualification(self):
        """Purpose: Reject ambiguous consumers. Inputs: Malformed workflows/jobs. Outputs: Failure, never reuse."""
        run = {"id": 17, "head_sha": "a" * 40, "head_branch": "main", "status": "completed", "conclusion": "success"}
        job = {"name": "CodeQL C++ (CPU)", "status": "completed", "conclusion": "success"}
        for page in [
            None,
            {},
            [None],
            [run] * 101,
            [{**run, "id": True}],
            [{**run, "head_sha": "b" * 40}],
            [{**run, "conclusion": 1}],
        ]:
            with (
                mock.patch.object(cpp, "capture_metadata", return_value=json.dumps(page).encode()),
                self.assertRaises(ValueError),
            ):
                cpp.has_qualified_cpp_workflow("fixture/repository", "refs/heads/main", "a" * 40)
        for page in [None, {}, [None], [job] * 101, [{**job, "status": None}], [{**job, "conclusion": True}]]:
            with (
                mock.patch.object(
                    cpp, "capture_metadata", side_effect=[json.dumps([run]).encode(), json.dumps(page).encode()]
                ),
                self.assertRaises(ValueError),
            ):
                cpp.has_qualified_cpp_workflow("fixture/repository", "refs/heads/main", "a" * 40)
        with mock.patch.object(
            cpp, "capture_metadata", return_value=json.dumps([{**run, "head_branch": "other"}]).encode()
        ) as call:
            self.assertFalse(cpp.has_qualified_cpp_workflow("fixture/repository", "refs/heads/main", "a" * 40))
            self.assertEqual(call.call_count, 1)

    def test_independent_push_jobs_and_query_changes(self):
        """Purpose: Separate execution roles. Inputs: Passive and policy changes. Outputs: Correct C++ admission."""
        for name in [
            "README.md",
            "docs/security-evidence.md",
            "tools/requirements/crawl4ai.txt",
            "tools/test_binary_corpus_transport.ps1",
        ]:
            self.assertFalse(cpp.select_cpp([name])["codeql_cpp"])
        for name in [
            *cpp.POLICY_FILES,
            ".github/codeql/codeql-config.yml",
            ".github/actions/future/action.yml",
            "fuzz/future.cpp",
            ".clusterfuzzlite/build.sh",
            "future/input.cpp",
            "SRC/core/future.cpp",
        ]:
            self.assertTrue(cpp.select_cpp([name])["codeql_cpp"])
        self.assertTrue(cpp.select_cpp(["README.md", "src/core/future.cpp"])["codeql_cpp"])

    def test_broad_events_and_initial_push(self):
        """Purpose: Retain full scans. Inputs: Scheduled/manual/new-branch events. Outputs: Unconditional admission."""
        for event in ["schedule", "workflow_dispatch"]:
            self.assertTrue(cpp.event_plan(event, "")["codeql_cpp"])
            with self.assertRaises(ValueError):
                cpp.event_plan(event, "0" * 40)
        self.assertTrue(cpp.event_plan("push", "0" * 40)["codeql_cpp"])
        for base in ["", "HEAD", "$(command)", "a" * 39, "A" * 40]:
            with self.assertRaises(ValueError):
                cpp.event_plan("push", base)

    def test_invalid_path_cannot_skip_analysis(self):
        """Purpose: Reject ambiguous inventories. Inputs: Traversal and aliases. Outputs: Failure, never skip."""
        for name in ["", "../src/a.cpp", "/src/a.cpp", "src\\a.cpp", "src/./a.cpp", "src//a.cpp", "C:source.cpp"]:
            with self.assertRaises(ValueError):
                cpp.select_cpp([name])
        with mock.patch.object(cpp, "MAX_PATH_BYTES", 8), self.assertRaises(ValueError):
            cpp.read_paths(io.BytesIO(b"012345678"))

    def test_real_git_delete_rename_and_missing_history(self):
        """Purpose: Bind complete ranges. Inputs: Real Git mutations. Outputs: Deleted/renamed source still selects."""
        with tempfile.TemporaryDirectory(prefix="flitzzip-cpp-plan-") as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "--quiet", str(root)], check=True)
            for key, value in [("user.name", "Contract Fixture"), ("user.email", "fixture@example.invalid")]:
                subprocess.run(["git", "-C", directory, "config", key, value], check=True)
            (root / "src").mkdir()
            (root / "src/original.cpp").write_text("source fixture\n", encoding="utf-8")
            subprocess.run(["git", "-C", directory, "add", "src/original.cpp"], check=True)
            subprocess.run(["git", "-C", directory, "commit", "--quiet", "-m", "Source fixture"], check=True)
            base = subprocess.check_output(["git", "-C", directory, "rev-parse", "HEAD"], text=True).strip()
            self.assertFalse(cpp.event_plan("push", base, root)["codeql_cpp"])
            (root / "README.md").write_text("source fixture\n", encoding="utf-8")
            subprocess.run(["git", "-C", directory, "rm", "--quiet", "src/original.cpp"], check=True)
            subprocess.run(["git", "-C", directory, "add", "README.md"], check=True)
            subprocess.run(["git", "-C", directory, "commit", "--quiet", "-m", "Rename source fixture"], check=True)
            self.assertTrue(cpp.event_plan("push", base, root)["codeql_cpp"])
            with self.assertRaisesRegex(ValueError, "Git range failed"):
                cpp.event_plan("push", "f" * 40, root)

    def test_output_and_workflow_enforce_the_actual_selector(self):
        """Purpose: Check the consumer. Inputs: CLI/output and workflow. Outputs: Required gate and full queries."""
        with tempfile.TemporaryDirectory(prefix="flitzzip-cpp-output-") as directory:
            output = Path(directory) / "job-output"
            with mock.patch.object(sys, "argv", ["cpp-plan", "--event", "schedule", "--github-output", str(output)]):
                cpp.main()
            self.assertEqual(
                output.read_bytes(),
                b"codeql_cpp=true\ncodeql_hip_host=true\n",
            )
        with (
            mock.patch.object(sys, "argv", ["cpp-plan", "--event", "schedule", "--repository", "fixture/repository"]),
            self.assertRaisesRegex(ValueError, "both repository and branch"),
        ):
            cpp.main()
        workflow = (cpp.ROOT / ".github/workflows/security-code-scanning.yml").read_text(encoding="utf-8")
        for required in [
            "needs: cpp-security-plan",
            "needs.cpp-security-plan.result != 'success'",
            "needs.cpp-security-plan.outputs.codeql_cpp != 'false'",
            "codeql_cpp: ${{ steps.cpp_plan.outputs.codeql_cpp }}",
            "python -B -m tools.cpp_security_plan",
            "fetch-depth: 0",
            "codeql_hip_host: ${{ steps.cpp_plan.outputs.codeql_hip_host }}",
            "needs.cpp-security-plan.outputs.codeql_hip_host != 'false'",
            "steps: &cpp-analysis-steps",
            "steps: *cpp-analysis-steps",
        ]:
            self.assertIn(required, " ".join(workflow.split()))
        self.assertNotIn("paths-ignore", workflow)
        self.assertNotIn("continue-on-error", workflow)
        self.assertIn("bootstrap_rocm_sdk.py --parent $sdkParent", workflow)

    def test_push_entry_point_routes_complete_hosted_reuse_identity(self):
        """Purpose: Exercise hosted routing.

        Inputs: Complete repository/ref identity. Outputs: Reuse qualifier owns the plan.
        """
        initial = cpp.select_cpp([])
        qualified = {**initial, "analysis_commits": {category: "a" * 40 for category in cpp.CPP_CATEGORIES}}
        arguments = [
            "cpp-plan",
            "--event",
            "push",
            "--push-base",
            "a" * 40,
            "--repository",
            "fixture/repository",
            "--ref",
            "refs/heads/main",
        ]
        with (
            mock.patch.object(sys, "argv", arguments),
            mock.patch.object(cpp, "validate_workflow"),
            mock.patch.object(cpp, "event_plan", return_value=initial),
            mock.patch.object(cpp, "qualify_analysis_reuse", return_value=qualified) as qualify,
            mock.patch("builtins.print") as output,
        ):
            cpp.main()
        qualify.assert_called_once_with(initial, "fixture/repository", "refs/heads/main")
        self.assertEqual(json.loads(output.call_args.args[0]), qualified)

    def test_real_module_entry_point_emits_scheduled_full_plan(self):
        """Purpose: Exercise executable wiring. Inputs: Scheduled CLI invocation. Outputs: Clean full-plan JSON."""
        completed = subprocess.run(
            [sys.executable, "-m", "tools.cpp_security_plan", "--event", "schedule"],
            cwd=cpp.ROOT,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(json.loads(completed.stdout)["codeql_cpp"])
        self.assertNotIn("Traceback", completed.stderr)

    def test_actual_workflow_rejects_database_merging_and_configuration_loss(self):
        """Purpose: Prevent recurrence. Inputs: Actual workflow and scope mutations. Outputs: Fail-closed admission."""
        workflow = (cpp.ROOT / ".github/workflows/security-code-scanning.yml").read_text(encoding="utf-8")
        cpp.validate_workflow(workflow)
        with self.assertRaisesRegex(ValueError, "workflow job boundaries changed"):
            cpp.validate_workflow(workflow.replace("\n  codeql-cpp-cpu:\n", "\n  renamed-cpp-job:\n"))
        for original, replacement in (
            ("    name: CodeQL C++ (CPU)\n", ""),
            ("name: CodeQL C++ (CPU)", "name: CodeQL C++ (${{ matrix.configuration }})"),
            ("name: CodeQL C++ (HIP host)", "name: CodeQL C++ (CPU)"),
            ("FLITZZIP_CODEQL_CONFIGURATION: hip-host", "FLITZZIP_CODEQL_CONFIGURATION: cpu"),
            ("outputs.codeql_hip_host != 'false'", "outputs.codeql_cpp == 'true'"),
            ("needs.cpp-security-plan.result != 'success'", "needs.cpp-security-plan.result == 'success'"),
            ("steps: *cpp-analysis-steps", "steps: []"),
            ("permissions: *cpp-analysis-permissions", "permissions: {}"),
            ("security-events: read", "security-events: none"),
            ("security-events: write", "security-events: read"),
            ("actions: read", "actions: none"),
            ("GH_TOKEN: ${{ github.token }}", "UNUSED_TOKEN: ${{ github.token }}"),
            ('--repository "$env:GITHUB_REPOSITORY" --ref "$env:GITHUB_REF"', ""),
        ):
            with self.subTest(original=original):
                self.assertIn(original, workflow)
                with self.assertRaisesRegex(ValueError, "C\\+\\+ security"):
                    cpp.validate_workflow(workflow.replace(original, replacement))

    def test_shared_analysis_rejects_missing_builds_and_partial_reports(self):
        """Purpose: Retain analysis coverage. Inputs: Shared-step mutations. Outputs: Each loss is rejected."""
        workflow = (cpp.ROOT / ".github/workflows/security-code-scanning.yml").read_text(encoding="utf-8")
        for original, replacement in (
            ("build-mode: manual", "build-mode: none"),
            ("queries: security-extended,security-and-quality", "queries: security-extended"),
            ("if: env.FLITZZIP_CODEQL_CONFIGURATION == 'cpu'", "if: env.FLITZZIP_CODEQL_CONFIGURATION == 'hip-host'"),
            ("if: env.FLITZZIP_CODEQL_CONFIGURATION == 'hip-host'", "if: always()"),
            ("if-no-files-found: error", "if-no-files-found: ignore"),
            ("-CpuOnlyValidation", "-CpuOnlyValidation\n          tools/build.ps1 -Configuration Release"),
            ("'/language:c-cpp/configuration:hip-host'", "'/language:c-cpp'"),
            ("raw-${{ env.FLITZZIP_CODEQL_CONFIGURATION }}", "raw"),
            ("steps.analyze_cpp.outcome == 'success'", "failure()"),
        ):
            with self.subTest(original=original):
                self.assertIn(original, workflow)
                with self.assertRaisesRegex(ValueError, "C\\+\\+ security"):
                    cpp.validate_workflow(workflow.replace(original, replacement))

    def test_hip_host_inputs_add_real_compilation_without_retiring_cpu_configuration(self):
        """Purpose: Cover HIP host branches. Inputs: Shared/unrelated paths. Outputs: Exact extra-build roles."""
        for path in [
            "src/gpu/hip_kernel_api.cpp",
            "src/gpu/hip_kernel_module.cpp",
            "src/gpu/hip_codec.cpp",
            "src/core/resource_limits.hpp",
            "cmake/WritePackagedRuntimeIdentity.cmake",
            "CMakeLists.txt",
            "src/core/checksum.cpp",
            "src/app/main_window_pages.cpp",
            "tests/cpp/test_gpu_telemetry.cpp",
            "third_party/lz4/lz4.c",
            *cpp.POLICY_FILES,
        ]:
            with self.subTest(path=path):
                plan = cpp.select_cpp([path])
                self.assertTrue(plan["codeql_hip_host"])
                self.assertTrue(plan["codeql_cpp"])
        for path in [
            "README.md",
            "tools/requirements/crawl4ai.txt",
        ]:
            plan = cpp.select_cpp([path])
            self.assertFalse(plan["codeql_hip_host"])
            self.assertFalse(plan["codeql_cpp"])
        self.assertTrue(cpp.select_cpp([], full=True)["codeql_hip_host"])
        workflow = (cpp.ROOT / ".github/workflows/security-code-scanning.yml").read_text(encoding="utf-8")
        self.assertIn("tools/build.ps1 -Configuration Release -CpuOnlyValidation", workflow)


if __name__ == "__main__":
    unittest.main()
