"""External index mirror and freshness contracts, with no package or network.

Adapted from strmt7/VulnerabilityScreener tests/test_cocoindex_agent_search.py (MIT).
"""

import contextlib
import io
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.cocoindex_agent_search import (
    CONFIG_DIGEST,
    ROOT,
    home,
    paths,
    prepare_mirror,
    search,
    source_digest,
    tracked_files,
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
                json.dumps({"source_digest": digest, "config_digest": CONFIG_DIGEST, "cocoindex_code": "0.2.41"}),
                encoding="utf-8",
            )
            hit = dict(file_path="source.py", start_line=1, end_line=1, score=0.9, content="pass")
            result = subprocess.CompletedProcess([], 0, stdout=json.dumps({"success": True, "results": [hit, hit]}))
            query = "private conceptual question"
            with (
                patch("tools.cocoindex_agent_search.tracked_files", return_value=entries),
                patch("tools.cocoindex_agent_search._run", return_value=result),
                contextlib.redirect_stdout(io.StringIO()) as output,
            ):
                search(base, query, 5, repo=base)
            self.assertIn("results=1", output.getvalue())
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
                        "cocoindex_code": "0.2.41",
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
            marker.write_text(json.dumps({"source_digest": "incorrect", "cocoindex_code": "0.2.41"}), encoding="utf-8")
            with (
                patch("tools.cocoindex_agent_search.tracked_files", return_value=[("source.py", source)]),
                self.assertRaisesRegex(RuntimeError, "index is stale"),
            ):
                search(base, "training labels", 5, repo=base)

    def test_mirror_paths_are_per_repository(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            self.assertNotEqual(paths(base, base / "a")[1], paths(base, base / "b")[1])

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

    def test_superzip_headers_and_build_files_are_indexed_without_vendored_code(self):
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


if __name__ == "__main__":
    unittest.main()
