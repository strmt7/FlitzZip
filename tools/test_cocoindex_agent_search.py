"""External index mirror and freshness contracts, with no package or network.

Adapted from strmt7/VulnerabilityScreener tests/test_cocoindex_agent_search.py (MIT).
"""

import contextlib
import hashlib
import io
import json
import math
import os
import runpy
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from tools import atomic_file
from tools import cocoindex_agent_search as agent_search
from tools.cocoindex_agent_search import (
    CONFIG_DIGEST,
    ROOT,
    home,
    paths,
    prepare_mirror,
    search,
    source_digest,
    tracked_files,
    validate_search_results,
)


class CocoIndexAgentSearchTests(unittest.TestCase):
    def test_successful_search_records_real_use_without_query_text(self):
        """Purpose: Prove routing receipt semantics.
        Inputs: fresh source/real wrapper/mock package.
        Outputs: deduplicated use record."""
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            source = base / "source.py"
            source.write_text("pass\n", encoding="utf-8")
            entries = [("source.py", source)]
            digest = source_digest(entries)
            mirror = paths(base, base)[1]
            marker = base / "active" / (mirror.name + ".json")
            marker.parent.mkdir()
            marker.write_text(
                json.dumps(
                    {"source_digest": digest, "config_digest": CONFIG_DIGEST, "cocoindex_code": agent_search.VERSION}
                ),
                encoding="utf-8",
            )
            hit = dict(file_path="source.py", start_line=1, end_line=1, score=0.9, content="pass")
            result = subprocess.CompletedProcess([], 0, stdout=json.dumps({"success": True, "results": [hit, hit]}))
            query = "private conceptual question"
            with (
                patch("tools.cocoindex_agent_search.tracked_files", return_value=entries),
                patch("tools.cocoindex_agent_search._run", return_value=result) as package,
                contextlib.redirect_stdout(io.StringIO()) as output,
            ):
                search(base, query, 1, repo=base, path="source.py")
            self.assertIn("results=1", output.getvalue())
            self.assertIn("--path", package.call_args.args[0])
            receipt = marker.with_name(mirror.name + ".usage.json").read_text()
            self.assertNotIn(query, receipt)
            self.assertEqual(json.loads(receipt)["source_digest"], digest)
            self.assertEqual(json.loads(receipt)["hit_count"], 1)
            source.write_text("changed\n", encoding="utf-8")
            original = receipt
            with (
                patch("tools.cocoindex_agent_search.tracked_files", return_value=entries),
                self.assertRaisesRegex(RuntimeError, "index is stale"),
            ):
                search(base, query, 5, repo=base)
            self.assertEqual(marker.with_name(mirror.name + ".usage.json").read_text(), original)

    def test_source_changed_during_search_cannot_record_current_use(self):
        """Purpose: Close the search freshness race.
        Inputs: source mutation during the package call. Outputs: explicit failure without a new receipt."""
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            source = base / "source.py"
            source.write_text("pass\n", encoding="utf-8")
            entries = [("source.py", source)]
            mirror = paths(base, base)[1]
            marker = base / "active" / (mirror.name + ".json")
            marker.parent.mkdir()
            marker.write_text(
                json.dumps(
                    {
                        "source_digest": source_digest(entries),
                        "config_digest": CONFIG_DIGEST,
                        "cocoindex_code": agent_search.VERSION,
                    }
                ),
                encoding="utf-8",
            )

            def mutate_source(*args, **kwargs):
                """Purpose: Inject source drift. Inputs: package argv. Outputs: successful stale search data."""
                source.write_text("changed\n", encoding="utf-8")
                return subprocess.CompletedProcess([], 0, stdout=json.dumps({"success": True, "results": []}))

            with (
                patch("tools.cocoindex_agent_search.tracked_files", return_value=entries),
                patch("tools.cocoindex_agent_search._run", side_effect=mutate_source),
                self.assertRaisesRegex(RuntimeError, "source changed during semantic search"),
            ):
                search(base, "conceptual question", 5, repo=base)
            self.assertFalse(marker.with_name(mirror.name + ".usage.json").exists())

    def test_home_requires_absolute_override(self):
        with patch.dict("os.environ", {"AGENT_CODE_HOME": "relative"}), self.assertRaisesRegex(ValueError, "absolute"):
            home()

    def test_home_uses_external_platform_default(self):
        """Purpose: Route default state. Inputs: Owned data root. Outputs: External FlitzZip path."""
        with (
            tempfile.TemporaryDirectory() as temporary,
            patch.dict(os.environ, {"LOCALAPPDATA": temporary}, clear=True),
        ):
            self.assertEqual(home(), (Path(temporary) / "FlitzZip/agent-code").resolve())

    def test_home_rejects_checkout_state(self):
        with (
            patch.dict("os.environ", {"AGENT_CODE_HOME": str(ROOT / "index-state")}),
            self.assertRaisesRegex(ValueError, "outside the checkout"),
        ):
            home()

    def test_mirror_copies_exact_bytes_and_prunes_stale_generated_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "source.py"
            source.write_bytes(b"print('safe')\n")
            mirror = root / "index-mirror"
            mirror.mkdir()
            stale = mirror / "stale.py"
            stale.write_text("old", encoding="utf-8")
            entries = [("nested/source.py", source)]
            prepare_mirror(mirror, entries)
            self.assertEqual((mirror / "nested/source.py").read_bytes(), source.read_bytes())
            self.assertFalse(stale.exists())
            first = source_digest(entries)
            source.write_bytes(b"print('changed')\n")
            self.assertNotEqual(first, source_digest(entries))
            prepare_mirror(mirror, entries)
            self.assertEqual((mirror / "nested/source.py").read_bytes(), source.read_bytes())

    def test_search_rejects_missing_index_without_starting_package(self):
        with tempfile.TemporaryDirectory() as temp, self.assertRaisesRegex(RuntimeError, "no active index"):
            search(Path(temp), "training labels", 5, repo=Path(temp))

    def test_search_rejects_unreliable_wildcard_filter(self):
        with tempfile.TemporaryDirectory() as temp, self.assertRaisesRegex(ValueError, "wildcard path filters"):
            search(Path(temp), "training labels", 5, repo=Path(temp), path="scanner/*")

    def test_search_rejects_stale_source_before_package_call(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            source = base / "source.py"
            source.write_text("pass\n", encoding="utf-8")
            mirror = paths(base, base)[1]
            marker = base / "active" / (mirror.name + ".json")
            marker.parent.mkdir()
            marker.write_text(
                json.dumps({"source_digest": "incorrect", "cocoindex_code": agent_search.VERSION}), encoding="utf-8"
            )
            with (
                patch("tools.cocoindex_agent_search.tracked_files", return_value=[("source.py", source)]),
                self.assertRaisesRegex(RuntimeError, "index is stale"),
            ):
                search(base, "training labels", 5, repo=base)

    def test_mirror_paths_are_per_repository(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            self.assertNotEqual(paths(base, base / "a")[1], paths(base, base / "b")[1])
            environment, _mirror, _config, runtime = paths(base, base / "a")
            tool = f"{agent_search.VERSION}-{agent_search.DEPENDENCY_DIGEST[:16]}"
            self.assertEqual(environment.name, f"venv-{tool}")
            self.assertEqual(runtime.parent.name, tool)

    def test_tracked_files_rejects_escape_and_real_env(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for unsafe in (b"../outside\0", b".env\0", b"secrets/key.txt\0", b"tls/private.pem\0"):
                with (
                    self.subTest(unsafe=unsafe),
                    patch("tools.cocoindex_agent_search.subprocess.run") as run,
                    self.assertRaises(ValueError),
                ):
                    run.return_value.stdout = unsafe
                    tracked_files(root)

    def test_tracked_files_uses_code_only_freshness(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "source.py").write_text("pass\n", encoding="utf-8")
            (root / "README.md").write_text("documentation\n", encoding="utf-8")
            with patch("tools.cocoindex_agent_search.subprocess.run") as run:
                run.return_value.stdout = b"README.md\0source.py\0"
                entries = tracked_files(root)
            self.assertEqual([name for name, _ in entries], ["source.py"])
            self.assertIn("--others", run.call_args.args[0])
            self.assertIn("--exclude-standard", run.call_args.args[0])

    def test_context_and_index_share_secret_and_alias_boundaries(self):
        """Purpose: Keep code mirroring behind the context reader's path policy.
        Inputs: Real mixed-case and environment-prefix fixtures plus Windows aliases. Outputs: Refusal before mirroring.
        """
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name in ("SECRETS/source.py", ".env.py", "key.PEM"):
                source = root / name
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text("controlled fixture only\n", encoding="utf-8")
            for name in ("SECRETS/source.py", ".env.py", "key.PEM", "secrets./source.py", "source.py."):
                with (
                    self.subTest(name=name),
                    patch("tools.cocoindex_agent_search.subprocess.run") as run,
                    self.assertRaises(ValueError),
                ):
                    run.return_value.stdout = name.encode("utf-8") + b"\0"
                    tracked_files(root)

    def test_flitzzip_headers_and_build_files_are_indexed_without_vendored_code(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name in ("src/codec.hpp", "CMakeLists.txt", "third_party/lib/codec.cpp"):
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("code\n", encoding="utf-8")
            with patch("tools.cocoindex_agent_search.subprocess.run") as run:
                run.return_value.stdout = b"src/codec.hpp\0CMakeLists.txt\0third_party/lib/codec.cpp\0"
                entries = tracked_files(root)
            self.assertEqual([name for name, _ in entries], ["src/codec.hpp", "CMakeLists.txt"])

    def test_tracked_files_omits_only_git_confirmed_deletions(self):
        """Purpose: Check deletion admission. Inputs: Git inventories. Outputs: only confirmed deletions omitted."""
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for deleted in (b"removed.py\0", b""):
                with self.subTest(deleted=deleted), patch("tools.cocoindex_agent_search.subprocess.run") as run:
                    run.side_effect = [
                        subprocess.CompletedProcess([], 0, stdout=b"removed.py\0"),
                        subprocess.CompletedProcess([], 0, stdout=deleted),
                    ]
                    if deleted:
                        self.assertEqual(tracked_files(root), [])
                    else:
                        with self.assertRaisesRegex(ValueError, "not a regular file"):
                            tracked_files(root)
                    self.assertIn("--deleted", run.call_args.args[0])

    def test_tracked_files_handles_real_unstaged_deletion_and_mirror_pruning(self):
        """Purpose: Exercise Git deletion. Inputs: temporary checkout. Outputs: exact live mirror and digest."""
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            repo = base / "repo"
            repo.mkdir()
            subprocess.run(["git", "init", "--quiet", str(repo)], check=True, timeout=30)
            for name in ("live.py", "removed.py"):
                (repo / name).write_text("pass\n", encoding="utf-8")
            subprocess.run(["git", "add", "--", "live.py", "removed.py"], cwd=repo, check=True, timeout=30)
            mirror = base / "mirror"
            before = tracked_files(repo)
            before_digest = source_digest(before)
            prepare_mirror(mirror, before)
            (repo / "removed.py").unlink()
            (repo / "new.py").write_text("value = 1\n", encoding="utf-8")
            after = tracked_files(repo)
            self.assertEqual({name for name, _ in after}, {"live.py", "new.py"})
            self.assertNotEqual(before_digest, source_digest(after))
            prepare_mirror(mirror, after)
            self.assertFalse((mirror / "removed.py").exists())
            self.assertEqual((mirror / "new.py").read_bytes(), (repo / "new.py").read_bytes())

    def test_staging_new_source_preserves_fingerprint_until_live_bytes_change(self):
        """Purpose: Prevent needless refresh after Git staging.
        Inputs: a real checkout moving an unchanged file from untracked to staged.
        Outputs: stable digest across ordering changes, invalidated by source mutation."""
        with tempfile.TemporaryDirectory() as temp:
            repo = Path(temp)
            subprocess.run(["git", "init", "--quiet", str(repo)], check=True, timeout=30)
            (repo / "a.py").write_text("a = 1\n", encoding="utf-8")
            subprocess.run(["git", "add", "--", "a.py"], cwd=repo, check=True, timeout=30)
            (repo / "z.py").write_text("z = 1\n", encoding="utf-8")
            before = tracked_files(repo)
            original_digest = source_digest(before)
            subprocess.run(["git", "add", "--", "z.py"], cwd=repo, check=True, timeout=30)
            after = tracked_files(repo)
            self.assertEqual({name for name, _ in before}, {name for name, _ in after})
            self.assertNotEqual([name for name, _ in before], [name for name, _ in after])
            self.assertEqual(original_digest, source_digest(after))
            (repo / "z.py").write_text("z = 2\n", encoding="utf-8")
            self.assertNotEqual(original_digest, source_digest(tracked_files(repo)))

    def test_install_does_not_select_an_unrelated_path_installer(self):
        """Keep an unrelated host uv installation outside the tool's dependency boundary."""
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            unrelated_uv = str(base / "unreviewed-uv.exe")
            paths(base)[0].mkdir(parents=True)
            responses = [
                subprocess.CompletedProcess([], 0, stdout=""),
                subprocess.CompletedProcess([], 0, stdout=""),
                subprocess.CompletedProcess([], 0, stdout="No broken requirements found."),
                subprocess.CompletedProcess([], 0, stdout=agent_search.VERSION + "\n"),
            ]
            with (
                patch.object(agent_search.shutil, "which", return_value=unrelated_uv),
                patch.object(agent_search, "_run", side_effect=responses) as run,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                agent_search.install(base)
            commands = [call.args[0] for call in run.call_args_list]
            self.assertFalse(any(command[0] == unrelated_uv for command in commands))
            owned_python = str(paths(base)[0] / ("Scripts/python.exe" if os.name == "nt" else "bin/python"))
            self.assertEqual(commands[1][:4], [owned_python, "-m", "pip", "install"])
            self.assertIn("--require-hashes", commands[1])
            frozen = paths(base)[0] / "flitzzip-requirements.txt"
            self.assertEqual(commands[1][-2:], ["-r", str(frozen)])
            self.assertEqual(frozen.read_bytes(), agent_search.REQUIREMENT_BYTES)

    def test_dependency_lock_has_its_own_environment_and_runtime(self):
        """A dependency refresh must not mutate the old environment or admit its index."""
        original = paths(Path("cache"))
        with patch.object(agent_search, "DEPENDENCY_DIGEST", "f" * 64):
            refreshed = paths(Path("cache"))
        self.assertNotEqual(original[0], refreshed[0])
        self.assertNotEqual(original[3], refreshed[3])
        self.assertEqual(original[1:3], refreshed[1:3])
        declarations = (agent_search.REQUIREMENTS.with_suffix(".in")).read_text(encoding="utf-8").splitlines()
        self.assertEqual(
            [line for line in declarations if not line.startswith("--") and line.strip()],
            [f"cocoindex-code[full]=={agent_search.VERSION}"],
        )

    def test_lock_contains_hashes_for_every_dependency_branch(self):
        """Even inactive platform requirements must have artifact hashes before publication."""
        blocks = agent_search.REQUIREMENT_BYTES.decode("utf-8").replace("\\\n", "").splitlines()
        requirements = [block for block in blocks if block.strip() and not block.startswith(("--", "#"))]
        self.assertTrue(requirements)
        for block in requirements:
            with self.subTest(requirement=block.split(" ", 1)[0]):
                self.assertRegex(block, r"\s--hash=sha256:[a-f0-9]{64}(?:\s|$)")

    def test_failed_dependency_check_cannot_admit_partial_install(self):
        """A partially installed CLI must be repaired and checked before cache reuse."""
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            venv = paths(base)[0]
            command = agent_search.ccc_path(venv)
            command.parent.mkdir(parents=True)
            command.write_bytes(b"partially installed CLI fixture")
            completion = venv / ".flitzzip-install-complete"
            success = subprocess.CompletedProcess([], 0, stdout="")
            exact = subprocess.CompletedProcess([], 0, stdout=agent_search.VERSION + "\n")
            with (
                patch.object(
                    agent_search,
                    "_run",
                    side_effect=[success, success, subprocess.CalledProcessError(1, ["pip", "check"])],
                ) as run,
                self.assertRaises(subprocess.CalledProcessError),
            ):
                agent_search.install(base)
            self.assertEqual(run.call_count, 3)
            self.assertFalse(completion.exists())
            with (
                patch.object(agent_search, "_run", side_effect=[success, success, success, exact]),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                agent_search.install(base)
            self.assertEqual(completion.read_text(encoding="utf-8").strip(), agent_search.DEPENDENCY_DIGEST)
            with (
                patch.object(agent_search, "_run", return_value=exact) as run,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                agent_search.install(base)
            run.assert_called_once_with([str(command), "version"], timeout=30)
            completion.write_text("different dependency identity\n", encoding="utf-8")
            with (
                patch.object(agent_search, "_run") as run,
                self.assertRaisesRegex(RuntimeError, "dependency identity mismatch"),
            ):
                agent_search.install(base)
            run.assert_not_called()

    def test_cache_receipt_failures_preserve_previous_bytes_and_clean_temporaries(self):
        """Real files retain the old receipt after write/replace failures and permit recovery."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "receipt"
            target.write_bytes(b"previous complete state")
            for failure in ("write", "replace"):
                with self.subTest(failure=failure), contextlib.ExitStack() as stack:
                    if failure == "write":
                        pending = stack.enter_context(tempfile.NamedTemporaryFile(dir=root, delete=False))
                        stack.enter_context(
                            patch.object(atomic_file.tempfile, "NamedTemporaryFile", return_value=pending)
                        )
                        stack.enter_context(
                            patch.object(pending, "write", side_effect=OSError("controlled write failure"))
                        )
                    else:
                        stack.enter_context(
                            patch.object(atomic_file.os, "replace", side_effect=OSError("controlled replace failure"))
                        )
                    with self.assertRaisesRegex(OSError, "controlled"):
                        agent_search.atomic_write_bytes(target, b"replacement state")
                self.assertEqual(target.read_bytes(), b"previous complete state")
                self.assertEqual(list(root.iterdir()), [target])
            agent_search.atomic_write_bytes(target, b"replacement state")
            self.assertEqual(target.read_bytes(), b"replacement state")
            self.assertEqual(list(root.iterdir()), [target])

    def test_pre_lock_index_is_stale_even_when_source_and_cli_version_match(self):
        """An old settings-only marker cannot admit an index after dependency locking."""
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            source = base / "source.py"
            source.write_text("pass\n", encoding="utf-8")
            entries = [("source.py", source)]
            mirror = paths(base, base)[1]
            marker = base / "active" / (mirror.name + ".json")
            marker.parent.mkdir()
            marker.write_text(
                json.dumps(
                    {
                        "source_digest": source_digest(entries),
                        "config_digest": hashlib.sha256(
                            (agent_search.SETTINGS + agent_search.PROJECT_SETTINGS).encode()
                        ).hexdigest(),
                        "cocoindex_code": agent_search.VERSION,
                    }
                ),
                encoding="utf-8",
            )
            with (
                patch.object(agent_search, "tracked_files", return_value=entries),
                patch.object(agent_search, "_run") as run,
                self.assertRaisesRegex(RuntimeError, "index is stale"),
            ):
                search(base, "ownership", 5, repo=base)
            run.assert_not_called()

    # Purpose: Exercise pinned installation reuse and post-install version admission without network.
    # Inputs: Owned external homes, inert CLI fixtures and bounded process results for the owned pip.
    # Outputs: Exact versions reuse/install; stale existing and mismatched new installations fail explicitly.
    def test_install_routes_and_verifies_exact_version(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cached = root / "cached"
            command = agent_search.ccc_path(paths(cached)[0])
            command.parent.mkdir(parents=True)
            command.write_bytes(b"inert CLI fixture")
            completion = paths(cached)[0] / ".flitzzip-install-complete"
            completion.write_text(agent_search.DEPENDENCY_DIGEST + "\n", encoding="utf-8")
            exact = subprocess.CompletedProcess([], 0, stdout=agent_search.VERSION + "\n")
            with (
                patch.object(agent_search, "_run", return_value=exact) as run,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                agent_search.install(cached)
            run.assert_called_once_with([str(command), "version"], timeout=30)
            stale = subprocess.CompletedProcess([], 0, stdout="0.0.0\n")
            with (
                patch.object(agent_search, "_run", return_value=stale),
                self.assertRaisesRegex(RuntimeError, "existing CocoIndex version"),
            ):
                agent_search.install(cached)

            base = root / "new"
            paths(base)[0].mkdir(parents=True)
            responses = [
                subprocess.CompletedProcess([], 0, stdout=""),
                subprocess.CompletedProcess([], 0, stdout=""),
                subprocess.CompletedProcess([], 0, stdout="No broken requirements found."),
                exact,
            ]
            with (
                patch.object(agent_search, "_run", side_effect=responses) as run,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                agent_search.install(base)
            install_command = run.call_args_list[1].args[0]
            self.assertEqual(install_command[1:4], ["-m", "pip", "install"])
            self.assertIn("--require-hashes", install_command)
            self.assertEqual(install_command[-2:], ["-r", str(paths(base)[0] / "flitzzip-requirements.txt")])
            base = root / "mismatch"
            paths(base)[0].mkdir(parents=True)
            responses = [
                subprocess.CompletedProcess([], 0, stdout=""),
                subprocess.CompletedProcess([], 0, stdout=""),
                subprocess.CompletedProcess([], 0, stdout="No broken requirements found."),
                stale,
            ]
            with (
                patch.object(agent_search, "_run", side_effect=responses),
                self.assertRaisesRegex(RuntimeError, "version verification failed"),
            ):
                agent_search.install(base)

    # Purpose: Reject malformed provider output before receipts, paths or snippets become trusted agent evidence.
    # Inputs: One exact hit plus independent response, collection, path, range, score and content mutations.
    # Outputs: The exact hit passes; every ambiguous or unbounded shape raises a controlled RuntimeError/ValueError.
    def test_search_result_schema_and_resource_admission(self):
        hit = {"file_path": "source.py", "start_line": 1, "end_line": 2, "score": 0.5, "content": "source"}
        self.assertEqual(validate_search_results({"success": True, "results": [hit]}, 5), [hit])
        failures = (
            None,
            {"success": "true", "results": []},
            {"success": False, "message": "provider failure"},
            {"success": True, "results": None},
            {"success": True, "results": [hit] * 6},
            {"success": True, "results": [None]},
        )
        for payload in failures:
            with self.subTest(payload=payload), self.assertRaises(RuntimeError):
                validate_search_results(payload, 5)
        mutations = (
            ("file_path", "../outside.py"),
            ("start_line", True),
            ("start_line", 0),
            ("end_line", 0),
            ("score", True),
            ("score", math.inf),
            ("content", None),
            ("content", "x" * (1024 * 1024 + 1)),
        )
        for field, value in mutations:
            changed = dict(hit)
            changed[field] = value
            with self.subTest(field=field, value=type(value).__name__), self.assertRaises((RuntimeError, ValueError)):
                validate_search_results({"success": True, "results": [changed]}, 5)

    # Purpose: Validate index settings, v1 migration, stable-source success and error output without package execution.
    # Inputs: Owned external state, one source file, inert CLI and controlled index process output.
    # Outputs: Missing tools/changed settings/errors fail; v1 state stops its daemon and publishes a fresh marker.
    def test_index_admission_migration_and_publication(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            repo = root / "repo"
            repo.mkdir()
            source = repo / "source.py"
            source.write_text("value = 1\n", encoding="utf-8")
            entries = [("source.py", source)]
            missing = root / "missing"
            with self.assertRaisesRegex(RuntimeError, "run install"):
                agent_search.index(missing, repo)

            changed_global = root / "changed-global"
            venv, _mirror, config, _runtime = paths(changed_global, repo)
            command = agent_search.ccc_path(venv)
            command.parent.mkdir(parents=True)
            command.write_bytes(b"CLI")
            (config / "global_settings.yml").parent.mkdir(parents=True)
            (config / "global_settings.yml").write_text("changed", encoding="utf-8")
            with (
                patch.object(agent_search, "tracked_files", return_value=entries),
                self.assertRaisesRegex(RuntimeError, "settings changed"),
            ):
                agent_search.index(changed_global, repo)

            changed_project = root / "changed-project"
            venv, mirror, _config, _runtime = paths(changed_project, repo)
            command = agent_search.ccc_path(venv)
            command.parent.mkdir(parents=True)
            command.write_bytes(b"CLI")
            project = mirror / ".cocoindex_code/settings.yml"
            project.parent.mkdir(parents=True)
            project.write_text("changed", encoding="utf-8")
            with (
                patch.object(agent_search, "tracked_files", return_value=entries),
                self.assertRaisesRegex(RuntimeError, "project settings changed"),
            ):
                agent_search.index(changed_project, repo)

            migrated = root / "migrated"
            venv, mirror, _config, _runtime = paths(migrated, repo)
            command = agent_search.ccc_path(venv)
            command.parent.mkdir(parents=True)
            command.write_bytes(b"CLI")
            project = mirror / ".cocoindex_code/settings.yml"
            project.parent.mkdir(parents=True)
            project.write_text(agent_search.PROJECT_SETTINGS_V1, encoding="utf-8")
            responses = (
                subprocess.CompletedProcess([], 0, stdout="stopped"),
                subprocess.CompletedProcess([], 0, stdout="Indexing: complete error: 0"),
            )
            with (
                patch.object(agent_search, "tracked_files", return_value=entries),
                patch.object(agent_search, "_run", side_effect=responses) as run,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                agent_search.index(migrated, repo)
            self.assertEqual(run.call_args_list[0].args[0][-2:], ["daemon", "stop"])
            marker = migrated / "active" / (mirror.name + ".json")
            self.assertEqual(json.loads(marker.read_text())["source_digest"], source_digest(entries))

            failed = root / "failed"
            venv, mirror, _config, _runtime = paths(failed, repo)
            command = agent_search.ccc_path(venv)
            command.parent.mkdir(parents=True)
            command.write_bytes(b"CLI")
            result = subprocess.CompletedProcess([], 0, stdout="Indexing: complete error: 1")
            with (
                patch.object(agent_search, "tracked_files", return_value=entries),
                patch.object(agent_search, "_run", return_value=result),
                self.assertRaisesRegex(RuntimeError, "index errors"),
            ):
                agent_search.index(failed, repo)
            self.assertFalse((failed / "active" / (mirror.name + ".json")).exists())

    # Purpose: Exercise mirror escape/link checks and the tracked-file logical size ceiling without large disk writes.
    # Inputs: Owned source/stale files plus narrowly scoped path-method doubles for hostile filesystem states.
    # Outputs: Escaping names, redirected targets/stale files and oversized source metadata fail before copying.
    def test_mirror_and_source_resource_boundaries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            mirror = root / "mirror"
            source = root / "source.py"
            source.write_text("pass\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "escapes target"):
                prepare_mirror(mirror, [("../outside.py", source)])
            target = mirror / "source.py"
            concrete_path = type(target)
            original_is_symlink = concrete_path.is_symlink

            def redirected(path):
                return path == target or original_is_symlink(path)

            with (
                patch.object(concrete_path, "is_symlink", autospec=True, side_effect=redirected),
                self.assertRaisesRegex(ValueError, "mirror path is a link"),
            ):
                prepare_mirror(mirror, [("source.py", source)])
            stale = mirror / "stale.py"
            stale.write_text("stale", encoding="utf-8")
            original_resolve = concrete_path.resolve

            def escaped(path, *args, **kwargs):
                if path == stale:
                    return root.parent / "outside.py"
                return original_resolve(path, *args, **kwargs)

            with (
                patch.object(concrete_path, "resolve", autospec=True, side_effect=escaped),
                self.assertRaisesRegex(ValueError, "stale mirror path escapes"),
            ):
                prepare_mirror(mirror, [])
            repo = root / "repo"
            repo.mkdir()
            oversized = repo / "large.py"
            oversized.write_text("pass\n", encoding="utf-8")
            canonical_oversized = oversized.resolve()
            original_stat = concrete_path.stat

            def large_stat(path, *args, **kwargs):
                if path == canonical_oversized:
                    return SimpleNamespace(st_mode=stat.S_IFREG, st_size=32 * 1024 * 1024 + 1)
                return original_stat(path, *args, **kwargs)

            with (
                patch.object(agent_search.subprocess, "run") as run,
                patch.object(concrete_path, "stat", autospec=True, side_effect=large_stat),
                self.assertRaisesRegex(ValueError, "exceeds 32 MiB"),
            ):
                run.return_value.stdout = b"large.py\0"
                tracked_files(repo)

    # Purpose: Route each public action and retain bounded child diagnostics at the script error boundary.
    # Inputs: Direct parser calls plus a module run whose installer child raises CalledProcessError with output.
    # Outputs: Actions dispatch exactly; invalid search is parser-visible; script failures exit three with diagnostics.
    def test_main_routing_and_script_failure_boundary(self):
        base = Path(tempfile.gettempdir()) / "cocoindex-agent-test-home"
        with patch.object(agent_search, "home", return_value=base), patch.object(agent_search, "install") as install:
            self.assertEqual(agent_search.main(["install"]), 0)
        install.assert_called_once_with(base)
        with patch.object(agent_search, "home", return_value=base), patch.object(agent_search, "index") as index:
            self.assertEqual(agent_search.main(["index"]), 0)
        index.assert_called_once_with(base)
        with patch.object(agent_search, "home", return_value=base), patch.object(agent_search, "search") as search_call:
            self.assertEqual(agent_search.main(["search", "conceptual query", "--limit", "2"]), 0)
        search_call.assert_called_once_with(base, "conceptual query", 2, path=None)
        with patch.object(agent_search, "home", return_value=base), self.assertRaises(SystemExit) as invalid:
            agent_search.main(["search", " ", "--limit", "0"])
        self.assertEqual(invalid.exception.code, 2)
        with tempfile.TemporaryDirectory() as temporary:
            failure = subprocess.CalledProcessError(7, ["owned child"], output="retained child output")
            with (
                patch.dict(os.environ, {"AGENT_CODE_HOME": str(Path(temporary).resolve())}),
                patch.object(subprocess, "run", side_effect=failure),
                patch("sys.argv", ["cocoindex_agent_search", "install"]),
                patch.object(sys, "path", [str(agent_search.ROOT / "tools"), *sys.path]),
                contextlib.redirect_stderr(io.StringIO()) as errors,
                self.assertRaises(SystemExit) as stopped,
            ):
                runpy.run_path(str(Path(agent_search.__file__).resolve()), run_name="__main__")
        self.assertEqual(stopped.exception.code, 3)
        self.assertIn("retained child output", errors.getvalue())
        self.assertIn("error: CocoIndex agent search", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
