"""External index mirror and freshness contracts, with no package or network.

Adapted from strmt7/VulnerabilityScreener tests/test_cocoindex_agent_search.py (MIT).
"""

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.cocoindex_agent_search import (
    ROOT,
    home,
    paths,
    prepare_mirror,
    search,
    source_digest,
    tracked_files,
)


class CocoIndexAgentSearchTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
