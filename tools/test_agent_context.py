"""Real-file context bounds, provenance, collision and mandatory-instruction contracts."""

import argparse
import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import agent_context as context


class AgentContextTests(unittest.TestCase):
    def setUp(self):
        """Purpose: Isolate state. Inputs: temporary filesystem. Outputs: owned checkout with mixed Markdown fences."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "guide.md"
        self.source.write_text(
            "# Guide\nintro\n## Selected\nrequired\n```md\n## Fake\n```\n### Nested\nmore\n## Other\nend\n",
            encoding="utf-8",
        )

    def read_args(self, **changes):
        """Purpose: Select test extent. Inputs: option overrides. Outputs: ordinary CLI-equivalent namespace."""
        values = dict(path="guide.md", section="Selected", start=1, end=None, max_chars=8000, session=None, force=False)
        return argparse.Namespace(**(values | changes))

    def note_args(self, **changes):
        """Purpose: Select cited note. Inputs: overrides. Outputs: typed synthesis with explicit source and expiry."""
        values = dict(
            key="timing-decision", summary_file="summary.md", source=["guide.md"], kind="decision", expires_hours=24
        )
        (self.root / "summary.md").write_text("Use the exact measured counters.\n", encoding="utf-8")
        return argparse.Namespace(**(values | changes))

    def test_section_includes_descendants_and_ignores_fenced_heading(self):
        """Purpose: Preserve required reading.
        Inputs: nested/fenced document.
        Outputs: exact complete section extent."""
        result = context.read_window(self.root, self.read_args())
        self.assertEqual((result["start"], result["end"]), (3, 9))
        self.assertEqual(result["status"], "complete")
        self.assertIn("9: more", result["content"])
        self.assertNotIn("10: ## Other", result["content"])
        self.assertEqual(
            [item["title"] for item in context.headings(self.source.read_text())],
            ["Guide", "Selected", "Nested", "Other"],
        )

    def test_partial_budget_reports_unread_and_never_enters_cache(self):
        """Purpose: Prevent false read coverage.
        Inputs: too-small budget/session.
        Outputs: unread range on every attempt."""
        args = self.read_args(max_chars=20, session="one-context")
        first = context.read_window(self.root, args)
        second = context.read_window(self.root, args)
        self.assertEqual(first, second)
        self.assertEqual(first["status"], "partial")
        self.assertLessEqual(first["content_chars"], 20)
        self.assertEqual(first["unread"], {"start": 4, "end": 9})
        self.assertFalse((self.root / context.state_name("sessions", args.session)).exists())

    def test_complete_cache_requires_same_context_range_and_bytes(self):
        """Purpose: Admit only exact reuse.
        Inputs: retained/new context and modified source.
        Outputs: reliable invalidation."""
        args = self.read_args(session="one-context")
        first = context.read_window(self.root, args)
        self.assertEqual(context.read_window(self.root, args)["status"], "unchanged_in_explicit_session")
        self.assertEqual(context.read_window(self.root, self.read_args(session="new-context"))["status"], "complete")
        self.assertEqual(context.read_window(self.root, self.read_args(session="one-context", force=True)), first)
        self.source.write_text(self.source.read_text() + "new source\n", encoding="utf-8")
        self.assertEqual(context.read_window(self.root, args)["status"], "complete")
        narrower = self.read_args(section=None, start=4, end=4, session="one-context")
        self.assertEqual(context.read_window(self.root, narrower)["content"], "4: required\n")

    def test_long_line_returns_zero_content_and_entire_unread_range(self):
        """Purpose: Bound indivisible source lines.
        Inputs: long line/tiny budget.
        Outputs: no hidden line truncation."""
        self.source.write_text("x" * 1000, encoding="utf-8")
        result = context.read_window(self.root, self.read_args(section=None, max_chars=10))
        self.assertEqual(result["content"], "")
        self.assertEqual(result["unread"], {"start": 1, "end": 1})

    def test_unknown_or_duplicate_section_and_invalid_ranges_fail(self):
        """Purpose: Reject guessed locations. Inputs: unknown/duplicate headings and bad bounds. Outputs: errors."""
        for args in (
            self.read_args(section="missing"),
            self.read_args(section=None, start=0),
            self.read_args(section=None, start=12),
        ):
            with self.subTest(args=args), self.assertRaises(ValueError):
                context.read_window(self.root, args)
        self.source.write_text("## Duplicate\na\n## Duplicate\nb\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "exactly one"):
            context.read_window(self.root, self.read_args(section="Duplicate"))

    def test_source_and_state_reject_escape_binary_and_large_files(self):
        """Purpose: Protect bounded local state.
        Inputs: hostile paths/binary/oversized text.
        Outputs: rejected access."""
        for name in (
            "",
            ".",
            "../escape",
            "/absolute",
            "C:/absolute",
            "folder\\path",
            ".git/config",
            ".GIT/config",
            "SECRETS/value.md",
            ".env.local",
            ".ENV.local",
            "key.pem",
        ):
            with self.subTest(name=name), self.assertRaises(ValueError):
                context.safe_path(self.root, name)
        for data in (b"binary\0", b"x" * (context.MAX_FILE_BYTES + 1)):
            self.source.write_bytes(data)
            with self.assertRaises(ValueError):
                context.read_source(self.root, "guide.md")
        for key in ("../escape", "Caps", "", "a" * 65):
            with self.subTest(key=key), self.assertRaises(ValueError):
                context.state_name("notes", key)

    def test_symlink_ancestor_is_rejected_for_reads_and_writes(self):
        """Purpose: Contain aliases.
        Inputs: real temporary symlink when supported.
        Outputs: rejected reparse traversal."""
        link = self.root / "alias"
        try:
            link.symlink_to(self.root, target_is_directory=True)
        except OSError as exc:
            self.skipTest(f"host cannot create test-owned symlink: {exc}")
        with self.assertRaisesRegex(ValueError, "reparse"):
            context.read_source(self.root, "alias/guide.md")
        with self.assertRaisesRegex(ValueError, "reparse"):
            context.write_state(self.root, "alias/note.json", {"schema": 1})

    def test_windows_path_aliases_cannot_bypass_source_or_state_policy(self):
        """Purpose: Reject Windows-normalized aliases before any context file access.
        Inputs: Real protected fixtures and trailing-dot/space path components. Outputs: No read or state mutation.
        """
        for directory in ("secrets", ".git"):
            protected = self.root / directory
            protected.mkdir()
            marker = protected / "fixture.json"
            original = '{"schema":1,"value":"controlled fixture"}'
            marker.write_text(original, encoding="utf-8")
            for alias in (directory + ".", directory + " "):
                with self.subTest(alias=alias):
                    name = alias + "/fixture.json"
                    with self.assertRaises(ValueError):
                        context.read_source(self.root, name)
                    with self.assertRaises(ValueError):
                        context.write_state(self.root, name, {"schema": 1})
                    self.assertEqual(marker.read_text(encoding="utf-8"), original)
        for name in ("guide.md.", "guide.md ", "key.pem.", ".env.", "out./note.json"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                context.safe_path(self.root, name)

    def test_note_freshness_is_not_validation_and_source_change_withholds_summary(self):
        """Purpose: Keep synthesis evidence honest.
        Inputs: cited note/source mutation.
        Outputs: stale rejection, no pass claim."""
        args = self.note_args()
        context.remember(self.root, args)
        first = context.recall(self.root, args.key)
        self.assertEqual(first["status"], "declared_sources_unchanged")
        self.assertEqual(first["acceptance"], "not_established_by_context_tool")
        self.source.write_text("changed evidence\n", encoding="utf-8")
        stale = context.recall(self.root, args.key)
        self.assertIsNone(stale["summary"])
        self.assertEqual(stale["reasons"], [{"path": "guide.md", "reason": "source_changed"}])
        self.source.unlink()
        self.assertEqual(context.recall(self.root, args.key)["reasons"][0]["reason"], "source_unavailable")

    def test_expired_note_is_withheld_and_existing_note_is_never_overwritten(self):
        """Purpose: Preserve old evidence and expiry.
        Inputs: collision/expired note.
        Outputs: original survives, stale summary hidden."""
        args = self.note_args()
        context.remember(self.root, args)
        path = self.root / context.state_name("notes", args.key)
        original = path.read_bytes()
        with self.assertRaises(FileExistsError):
            context.remember(self.root, args)
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual(list(path.parent.glob(".context-*")), [])
        note = json.loads(original)
        note["expires_at"] = "2000-01-01T00:00:00+00:00"
        context.write_state(self.root, context.state_name("notes", args.key), note)
        self.assertEqual(context.recall(self.root, args.key)["reasons"], [{"reason": "expired"}])

    def test_startup_delivers_current_mandatory_skills_and_fails_if_missing(self):
        """Purpose: Check actual instruction delivery.
        Inputs: repository and missing local fixture.
        Outputs: full skills, explicit failure."""
        delivered = context.startup(context.ROOT)
        self.assertEqual(len(delivered["skills"]), 2)
        for skill in delivered["skills"]:
            actual, digest = context.read_source(context.ROOT, skill["path"])
            self.assertEqual((skill["instructions"], skill["sha256"]), (actual, digest))
        agents, digest = context.read_source(context.ROOT, "AGENTS.md")
        research = delivered["research"]
        self.assertIn(research["instructions"], agents)
        self.assertEqual(research["sha256"], digest)
        self.assertEqual(research["launcher_sha256"], context.read_source(context.ROOT, research["launcher"])[1])
        (self.root / "AGENTS.md").write_text("Mandatory skills are missing.\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            context.startup(self.root)

    def test_startup_rejects_removed_research_rule_and_missing_portable_launcher(self):
        """Purpose: Prevent silent research-route removal.
        Inputs: real current instructions and missing tool/rule mutations.
        Outputs: offline startup fails; no crawler installation or ordinary native build occurs."""
        agents, _ = context.read_source(context.ROOT, "AGENTS.md")
        for name in context.SKILLS:
            relative = f".agents/skills/{name}/SKILL.md"
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(context.read_source(context.ROOT, relative)[0], encoding="utf-8")
        (self.root / "AGENTS.md").write_bytes(agents.encode("utf-8"))
        with self.assertRaises(FileNotFoundError):
            context.startup(self.root)
        target = self.root / "tools/crawl4ai_tool.py"
        target.parent.mkdir()
        target.write_text(context.read_source(context.ROOT, "tools/crawl4ai_tool.py")[0], encoding="utf-8")
        delivered = context.startup(self.root)
        (self.root / "AGENTS.md").write_bytes(agents.replace(delivered["research"]["instructions"], "").encode("utf-8"))
        with self.assertRaisesRegex(ValueError, "Crawl4AI"):
            context.startup(self.root)

    def test_cli_budget_errors_and_stale_exit_remain_machine_visible(self):
        """Purpose: Verify real command dispatch.
        Inputs: valid/invalid CLI and expired note.
        Outputs: JSON and distinct exit status."""
        args = self.note_args()
        context.remember(self.root, args)
        name = context.state_name("notes", args.key)
        note = context.load_state(self.root, name)
        note["expires_at"] = "2000-01-01T00:00:00+00:00"
        context.write_state(self.root, name, note)
        with patch.object(context, "ROOT", self.root):
            for argv, expected in (
                (["outline", "guide.md"], 0),
                (["read", "guide.md", "--max-chars", "0"], 2),
                (["recall", args.key], 3),
            ):
                with self.subTest(argv=argv), contextlib.redirect_stdout(io.StringIO()) as output:
                    self.assertEqual(context.main(argv), expected)
                    self.assertIsInstance(json.loads(output.getvalue()), dict)


if __name__ == "__main__":
    unittest.main()
