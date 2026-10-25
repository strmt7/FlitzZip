"""Real-file context bounds, provenance, collision and mandatory-instruction contracts."""

import argparse
import contextlib
import io
import json
import math
import runpy
import sys
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

    def test_read_history_rejects_invalid_shape_and_full_session(self):
        """Purpose: Bound disposable session history.
        Inputs: Non-list and 200-record cache states. Outputs: Early refusal without state growth.
        """
        args = self.read_args(session="bounded-session")
        name = context.state_name("sessions", args.session)
        for reads, message in (({}, "invalid/oversized"), ([{"record": index} for index in range(200)], "is full")):
            context.write_state(self.root, name, {"schema": 1, "reads": reads})
            with self.subTest(reads=type(reads).__name__), self.assertRaisesRegex(ValueError, message):
                context.read_window(self.root, args)

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
        with patch.object(context, "MAX_FILE_BYTES", 64), self.assertRaisesRegex(ValueError, "state exceeds"):
            context.write_state(self.root, "out/agent-context/large.json", {"schema": 1, "value": "x" * 100})
        with self.assertRaises(ValueError):
            context.write_state(self.root, "out/agent-context/nonfinite.json", {"schema": 1, "value": math.nan})

    def test_state_rejects_ambiguous_json_and_non_integer_schema(self):
        """Purpose: Keep reusable context state typed and unambiguous.
        Inputs: Boolean schema, duplicate members and nonfinite values. Outputs: Controlled refusal.
        """
        path = self.root / "state.json"
        for payload in ('{"schema":true}', '{"schema":1,"schema":1}', '{"schema":1,"value":NaN}'):
            path.write_text(payload, encoding="utf-8")
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                context.load_state(self.root, "state.json")

    def test_resolved_escape_is_rejected_after_component_checks(self):
        """Purpose: Preserve the final containment check across path resolution changes.
        Inputs: Modeled final resolution outside the checkout. Outputs: Refusal before reading.
        """
        outside = self.root.parent / "outside"
        concrete_path = type(self.source)
        original = concrete_path.resolve

        def resolve(path, *args, **kwargs):
            """Purpose: Model final path escape. Inputs: Path/resolve options. Outputs: Actual root or outside file."""
            if path == self.source:
                return outside
            return original(path, *args, **kwargs)

        with (
            patch.object(concrete_path, "resolve", autospec=True, side_effect=resolve),
            self.assertRaisesRegex(ValueError, "escapes checkout"),
        ):
            context.safe_path(self.root, "guide.md")

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

    def test_note_input_and_recall_schema_boundaries(self):
        """Purpose: Reject incomplete synthesis and malformed retained provenance.
        Inputs: Blank summary, no sources, missing provenance and timezone-free expiry. Outputs: Controlled refusal.
        """
        args = self.note_args()
        (self.root / args.summary_file).write_text(" \n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "summary"):
            context.remember(self.root, args)
        with self.assertRaisesRegex(ValueError, "evidence"):
            context.remember(self.root, self.note_args(source=[]))

        name = context.state_name("notes", "malformed-note")
        context.write_state(self.root, name, {"schema": 1, "sources": []})
        with self.assertRaisesRegex(ValueError, "provenance"):
            context.recall(self.root, "malformed-note")

        _, digest = context.read_source(self.root, "guide.md")
        context.write_state(
            self.root,
            name,
            {
                "schema": 1,
                "kind": "decision",
                "summary": "Bounded evidence.\n",
                "sources": [{"path": "guide.md", "sha256": digest}],
                "expires_at": "2030-01-01T00:00:00",
            },
        )
        with self.assertRaisesRegex(ValueError, "timezone"):
            context.recall(self.root, "malformed-note")

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

    def test_startup_requires_both_declared_skill_paths(self):
        """Purpose: Prevent a generic mandatory claim from replacing executable skill routing.
        Inputs: Current instructions with one exact skill path removed. Outputs: Explicit startup refusal.
        """
        agents, _ = context.read_source(context.ROOT, "AGENTS.md")
        agents = agents.replace(".agents/skills/caveman/SKILL.md", "missing-caveman-skill")
        (self.root / "AGENTS.md").write_text(agents, encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "mandatory Caveman and CocoIndex routing"):
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

    def test_cli_routes_startup_read_remember_and_direct_entrypoint(self):
        """Purpose: Exercise every remaining public command route.
        Inputs: Controlled handlers, valid read/note arguments and direct help. Outputs: Exact status and dispatch.
        """
        with (
            patch.object(context, "startup", return_value={"status": "ready"}) as start,
            contextlib.redirect_stdout(io.StringIO()) as output,
        ):
            self.assertEqual(context.main(["startup"]), 0)
        start.assert_called_once_with(context.ROOT)
        self.assertEqual(json.loads(output.getvalue())["status"], "ready")
        with patch.object(context, "ROOT", self.root):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(context.main(["read", "guide.md", "--start", "1", "--end", "1"]), 0)
            args = self.note_args()
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(
                    context.main(
                        [
                            "remember",
                            args.key,
                            "--summary-file",
                            args.summary_file,
                            "--source",
                            args.source[0],
                            "--kind",
                            args.kind,
                        ]
                    ),
                    0,
                )
            with contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(
                    context.main(
                        [
                            "remember",
                            "bad-expiry",
                            "--summary-file",
                            args.summary_file,
                            "--source",
                            args.source[0],
                            "--kind",
                            args.kind,
                            "--expires-hours",
                            "0",
                        ]
                    ),
                    2,
                )
            self.assertIn("1-168 hours", json.loads(output.getvalue())["error"])
        original_path = list(sys.path)
        try:
            with (
                patch.object(sys, "argv", ["agent_context.py", "--help"]),
                patch.object(sys, "stdout", io.StringIO()),
                self.assertRaises(SystemExit) as status,
            ):
                runpy.run_path(str(Path(context.__file__).resolve()), run_name="__main__")
            self.assertEqual(status.exception.code, 0)
        finally:
            sys.path[:] = original_path

    def invoke(self, argv):
        """Purpose: Exercise the real reader interface. Inputs: CLI arguments. Outputs: Exit and decoded response."""
        with patch.object(context, "ROOT", self.root), contextlib.redirect_stdout(io.StringIO()) as output:
            code = context.main(argv)
        return code, json.loads(output.getvalue())

    def test_json_report_pages_recover_every_finding_without_printing_embedded_logs(self):
        """Purpose: Preserve complete navigation with bounded context.
        Inputs: Report with many findings and large logs. Outputs: Every exact pointer, explicit omitted payloads.
        """
        report = {
            "status": "failed",
            "findings": [{"id": index, "message": "must not disappear"} for index in range(53)],
            "log": "retained diagnostic\n" * 10000,
        }
        path = self.root / "report.json"
        original = json.dumps(report).encode()
        path.write_bytes(original)
        code, root = self.invoke(["json", "report.json"])
        self.assertEqual(code, 0)
        self.assertEqual(root["items"][0]["value"], "failed")
        self.assertFalse(root["items"][1]["expanded"])
        self.assertFalse(root["items"][2]["expanded"])
        self.assertNotIn("retained diagnostic", json.dumps(root))
        self.assertEqual(root["acceptance"], "not_established_by_context_tool")
        seen, offset = [], 0
        while offset is not None:
            code, page = self.invoke(
                ["json", "report.json", "--pointer", "/findings", "--offset", str(offset), "--max-chars", "800"]
            )
            self.assertEqual(code, 0)
            self.assertLessEqual(len(context.compact_json(page)), 800)
            seen.extend(entry["pointer"] for entry in page["items"])
            offset = page["next_offset"]
        self.assertEqual(seen, [f"/findings/{index}" for index in range(53)])
        code, selected = self.invoke(["json", "report.json", "--pointer", "/findings/52/id"])
        self.assertEqual((code, selected["value"]), (0, 52))
        self.assertEqual(path.read_bytes(), original)

    def test_json_pointer_preserves_escaped_keys_empty_keys_and_scalar_types(self):
        """Purpose: Prevent selecting the wrong evidence. Inputs: RFC 6901 edge cases. Outputs: Exact values."""
        value = {"a/b": {"~key": [False, None, 123]}, "": {"01": "object key"}}
        (self.root / "report.json").write_text(json.dumps(value), encoding="utf-8")
        for pointer, expected in (
            ("/a~1b/~0key/0", False),
            ("/a~1b/~0key/1", None),
            ("/a~1b/~0key/2", 123),
            ("//01", "object key"),
        ):
            with self.subTest(pointer=pointer):
                code, result = self.invoke(["json", "report.json", "--pointer", pointer])
                self.assertEqual(code, 0)
                self.assertEqual(result["value"], expected)
                self.assertEqual(type(result["value"]), type(expected))
        (self.root / "report.json").write_text('{"value":1.5}', encoding="utf-8")
        self.assertEqual(self.invoke(["json", "report.json", "--pointer", "/value"])[1]["value"], 1.5)
        for pointer in (
            "a/b",
            "/a~2b",
            "/missing",
            "/value/nested",
            "/a~1b/~0key/01",
            "/a~1b/~0key/-",
            "/a~1b/~0key/8",
        ):
            with self.subTest(pointer=pointer):
                self.assertEqual(self.invoke(["json", "report.json", "--pointer", pointer])[0], 2)

    def test_json_budget_counts_serialized_unicode_and_does_not_mislabel_omissions(self):
        """Purpose: Bound actual emitted text. Inputs: Escaped Unicode and oversized scalars. Outputs: Honest extent."""
        for value in ("λ" * 5000, {"λ" * 500: "value"}):
            (self.root / "report.json").write_text(json.dumps(value), encoding="utf-8")
            code, result = self.invoke(["json", "report.json", "--max-chars", "512"])
            if isinstance(value, str):
                self.assertEqual(code, 0)
                self.assertFalse(result["expanded"])
                self.assertNotIn("value", result)
                self.assertEqual(result["characters"], len(value))
            else:
                self.assertEqual(code, 2)
                self.assertIn("cannot fit", result["error"])
            self.assertLessEqual(len(context.compact_json(result)), 512)
        key = "k" * 500
        (self.root / "report.json").write_text(json.dumps({key: 1}), encoding="utf-8")
        self.assertEqual(self.invoke(["json", "report.json", "--pointer", "/" + key, "--max-chars", "512"])[0], 2)

    def test_json_projection_preserves_failures_and_refuses_missing_fields(self):
        """Purpose: Inspect many results without repeated reads. Inputs: Real mixed outcomes. Outputs: Exact fields."""
        rows = [{"id": "one", "passed": False, "log": "large" * 10000}, {"id": "two", "passed": True}]
        (self.root / "report.json").write_text(json.dumps(rows), encoding="utf-8")
        code, result = self.invoke(["json", "report.json", "--field", "id", "--field", "passed"])
        self.assertEqual(code, 0)
        self.assertEqual(
            [[field["value"] for field in item["fields"]] for item in result["items"]],
            [["one", False], ["two", True]],
        )
        self.assertTrue(all(not item["expanded"] for item in result["items"]))
        self.assertNotIn("large", json.dumps(result))
        for args in (["--field", "missing"], ["--field", "id", "--field", "id"], ["--pointer", "/0", "--field", "id"]):
            self.assertEqual(self.invoke(["json", "report.json", *args])[0], 2)
        (self.root / "report.json").write_text("[1,2]", encoding="utf-8")
        self.assertEqual(self.invoke(["json", "report.json", "--field", "id"])[0], 2)

    def test_json_rejects_ambiguous_nonfinite_deep_or_oversized_evidence(self):
        """Purpose: Refuse unbounded evidence. Inputs: Invalid JSON families. Outputs: Explicit failures."""
        path = self.root / "report.json"
        for text in (
            '{"status":"failed","status":"passed"}',
            '{"other":NaN}',
            '{"other":Infinity}',
            '{"other":1e999}',
            "[" * 2000,
        ):
            path.write_text(text, encoding="utf-8")
            with self.subTest(text=text[:80]):
                self.assertEqual(self.invoke(["json", "report.json"])[0], 2)
        path.write_bytes(b'"' + b"x" * 1024 + b'"')
        with patch.object(context, "MAX_REPORT_BYTES", 1024):
            self.assertEqual(self.invoke(["json", "report.json"])[0], 2)
        for argv in (
            ["json", "../outside.json"],
            ["json", "report.json", "--offset", "-1"],
            ["json", "report.json", "--limit", "0"],
            ["json", "report.json", "--max-chars", "32001"],
            ["json", "report.json", "--offset", "1"],
        ):
            with self.subTest(argv=argv):
                self.assertEqual(self.invoke(argv)[0], 2)

    def test_empty_json_containers_and_final_pages_are_distinct_from_missing_evidence(self):
        """Purpose: Preserve empty results. Inputs: Containers and invalid offsets. Outputs: Exact status."""
        path = self.root / "report.json"
        for value in ({}, []):
            path.write_text(json.dumps(value), encoding="utf-8")
            code, result = self.invoke(["json", "report.json"])
            self.assertEqual(code, 0)
            self.assertEqual(result["items"], [])
            self.assertIsNone(result["next_offset"])
            self.assertEqual(self.invoke(["json", "report.json", "--offset", "1"])[0], 2)
        path.write_text("[1,2]", encoding="utf-8")
        self.assertEqual(self.invoke(["json", "report.json", "--offset", "2"])[0], 0)
        path.unlink()
        self.assertEqual(self.invoke(["json", "report.json"])[0], 2)

    def test_python_outline_preserves_decorators_nested_scopes_and_never_executes_source(self):
        """Purpose: Navigate code safely. Inputs: Nested definitions and execution trap. Outputs: Exact spans."""
        source = self.root / "module.py"
        source.write_text(
            "raise RuntimeError('outline must never execute source')\n"
            "@decorator\nclass First:\n    @other\n    async def method(self):\n"
            "        def nested():\n            pass\n        return nested\n"
            "def final():\n    pass\n",
            encoding="utf-8",
        )
        code, result = self.invoke(["outline", "module.py"])
        self.assertEqual(code, 0)
        self.assertEqual(
            [(item["name"], item["start"], item["end"]) for item in result["symbols"]],
            [("First", 2, 8), ("First.method", 4, 8), ("First.method.nested", 6, 7), ("final", 9, 10)],
        )
        source.write_text("def broken(", encoding="utf-8")
        self.assertEqual(self.invoke(["outline", "module.py"])[0], 2)

    def test_outline_pagination_retains_every_heading_and_source_hash(self):
        """Purpose: Bound navigation. Inputs: Many headings and small pages. Outputs: Complete traversal."""
        self.source.write_text("".join(f"## Heading {index}\nbody\n" for index in range(75)), encoding="utf-8")
        entries, identities, offset = [], set(), 0
        while offset is not None:
            code, result = self.invoke(["outline", "guide.md", "--offset", str(offset), "--max-chars", "700"])
            self.assertEqual(code, 0)
            self.assertLessEqual(len(context.compact_json(result)), 700)
            identities.add(result["sha256"])
            entries.extend(result["headings"])
            offset = result["next_offset"]
        self.assertEqual(len(identities), 1)
        self.assertEqual([entry["title"] for entry in entries], [f"Heading {index}" for index in range(75)])
        self.assertEqual([entry["start"] for entry in entries], list(range(1, 150, 2)))
        self.assertEqual(self.invoke(["outline", "guide.md", "--offset", "76"])[0], 2)
        self.source.write_text("## " + "H" * 1000 + "\n", encoding="utf-8")
        self.assertEqual(self.invoke(["outline", "guide.md", "--max-chars", "512"])[0], 2)


if __name__ == "__main__":
    unittest.main()
