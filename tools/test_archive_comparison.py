"""Focused safety tests for the bounded external archive benchmark runner."""

from __future__ import annotations

import bz2
import contextlib
import copy
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
import uuid
from pathlib import Path, PureWindowsPath
from types import SimpleNamespace
from unittest.mock import patch

from tools import neutron_corpus_benchmark as corpus_transport
from tools import run_archive_comparison as comparison


# Purpose: Exercise real Git path encoding without inheriting signing, hooks or another checkout's configuration.
# Inputs: An owned temporary repository and fixed test arguments. Outputs: Bounded command output or a test failure.
def fixture_git(root: Path, *arguments: str) -> bytes:
    return subprocess.check_output(
        [
            "git",
            "-c",
            "user.name=Coverage Test",
            "-c",
            "user.email=test@example.invalid",
            "-c",
            "commit.gpgsign=false",
            "-c",
            f"core.hooksPath={root / 'no-hooks'}",
            *arguments,
        ],
        cwd=root,
        stderr=subprocess.STDOUT,
        timeout=10,
    )


# Purpose: Own a small repository whose source and unrelated document changes are independent.
# Inputs: Safe relative source name. Outputs: Temporary root, nonexecutable binary fixture and tracked source path.
@contextlib.contextmanager
def source_fixture(name: str = "src/plain.cpp"):
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        fixture_git(root, "init", "--quiet", "-b", "main")
        source = root / name
        source.parent.mkdir(parents=True)
        source.write_bytes(b"original source\n")
        (root / "notes.md").write_bytes(b"original notes\n")
        binary = root / "fixture.bin"
        binary.write_bytes(b"nonexecutable unit fixture")
        fixture_git(root, "add", "--all")
        fixture_git(root, "commit", "--quiet", "-m", "owned unit fixture")
        yield root, binary, source


class ComparisonRunnerTests(unittest.TestCase):
    # Purpose: Reject corpus directories redirected through an actual Windows junction before permission or file use.
    # Inputs: A private junction and nested descendant with minimal top-level metadata admission.
    # Outputs: Both paths fail closed, the external target is untouched and only the junction is removed on cleanup.
    @unittest.skipUnless(os.name == "nt", "Windows junction semantics")
    def test_manifest_junction_ancestry_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target, link = root / "outside", root / "redirected"
            (target / "nested").mkdir(parents=True)
            script = root / "junction.ps1"
            script.write_text(
                "param([string]$Link,[string]$Target)\n"
                "New-Item -ItemType Junction -Path $Link -Target $Target -ErrorAction Stop | Out-Null\n"
            )
            command = [
                "powershell.exe",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(script),
                str(link),
                str(target),
            ]
            subprocess.run(
                command,
                check=True,
                capture_output=True,
                timeout=10,
                env=corpus_transport.process_owner().child_environment(command),
            )
            try:
                with (
                    patch.object(
                        comparison,
                        "read_json",
                        return_value={"schema_version": 1, "name": "owned unit fixture", "files": [{}], "cases": []},
                    ),
                    patch.object(comparison, "require_permission") as permission,
                ):
                    for directory in (link, link / "nested"):
                        with self.subTest(directory=directory), self.assertRaisesRegex(ValueError, "traverse links"):
                            comparison.verify_manifest(root / "unit.json", directory)
                    permission.assert_not_called()
                self.assertEqual([path.name for path in target.iterdir()], ["nested"])
                self.assertEqual(list((target / "nested").iterdir()), [])
            finally:
                os.rmdir(link)

    # Purpose: Prevent a failed host sampler from being silently interpreted as valid resource evidence.
    # Inputs: Explicit sampler process failure with a bounded diagnostic tail.
    # Outputs: A descriptive RuntimeError is raised before any measurement is labelled qualified.
    def test_host_sampler_failure_is_explicit(self) -> None:
        with (
            patch.object(comparison, "ROOT", PureWindowsPath("C:/unit-fixture")),
            patch.object(
                comparison.subprocess,
                "run",
                return_value=SimpleNamespace(returncode=3, stdout="", stderr="unit sampler error"),
            ),
            self.assertRaisesRegex(RuntimeError, "sampling failed: unit sampler error"),
        ):
            comparison.host_snapshot()

    # Purpose: Preserve complete Git filename records and fail closed on malformed rename/copy transport.
    # Inputs: NUL-delimited paths with whitespace/Unicode and truncated or invalid status records.
    # Outputs: Exact path identities survive decoding; malformed or non-UTF-8 input never appears clean.
    def test_git_status_record_contract(self) -> None:
        self.assertEqual(comparison.changed_paths(b""), [])
        status = (
            " M src/space name.cpp\0R  docs/new.md\0src/old.cpp\0C  notes/new\0notes/original\0?? notes/caf\u00e9\0"
        )
        self.assertEqual(
            comparison.changed_paths(status.encode()),
            [
                "src/space name.cpp",
                "docs/new.md",
                "src/old.cpp",
                "notes/new",
                "notes/original",
                "notes/caf\u00e9",
            ],
        )
        for invalid in (b" M src/x", b" M \0", b"X  src/x\0", b" M x\0\0", b"R  new\0", b"C  new\0\0"):
            with self.subTest(status=invalid), self.assertRaises(RuntimeError):
                comparison.changed_paths(invalid)
        with self.assertRaises(UnicodeDecodeError):
            comparison.changed_paths(b" M \xff\0")

    # Purpose: Refuse to publish observations if their source/binary identity changes or an output appears mid-run.
    # Inputs: Real temporary Git identities with an offline case double that mutates one publication prerequisite.
    # Outputs: Only unchanged identities publish; existing output survives and failed cohorts create no result file.
    def test_main_rechecks_identity_and_preserves_concurrent_output(self) -> None:
        for mutation in ("none", "historical", "missing-tool", "binary", "source", "commit", "output"):
            with self.subTest(mutation=mutation), source_fixture() as (root, binary, source):
                output = root / "out/benchmarks/results.json"
                args = SimpleNamespace(
                    manifest=None if mutation == "historical" else root / "unit-manifest.json",
                    corpus=root,
                    downloads=root,
                    flitzzip=binary,
                    sevenzip=None if mutation == "missing-tool" else binary,
                    zstd=None,
                    output=output,
                    runs=5,
                    level=5,
                    round_pause_ms=0,
                )
                files = [
                    {
                        "name": "unit.txt",
                        "bytes": 3,
                        "sha256": hashlib.sha256(b"abc").hexdigest(),
                        "subject": "owned unit fixture",
                    }
                ]
                case = ("offline-unit-control", "zip", ("unit.txt",), ("FlitzZip", "7-Zip"))
                observed = {
                    "results": [
                        {"tool": name, "compress_seconds": [1.0], "extract_seconds": [2.0], "archive_bytes": [3]}
                        for name in case[3]
                    ]
                }

                # Purpose: Model a change outside a measured command. Inputs: Case args. Outputs: Unit observations.
                def observe(
                    *_arguments,
                    mutation=mutation,
                    binary=binary,
                    source=source,
                    root=root,
                    output=output,
                    observed=observed,
                ):
                    if mutation == "binary":
                        binary.write_bytes(b"different executable identity")
                    elif mutation == "source":
                        source.write_bytes(b"different source identity")
                    elif mutation == "commit":
                        fixture_git(root, "commit", "--quiet", "--allow-empty", "-m", "different unit revision")
                    elif mutation == "output":
                        output.write_bytes(b"concurrent owner must survive")
                    return copy.deepcopy(observed)

                with (
                    patch.object(comparison, "ROOT", root),
                    patch.object(comparison, "parse_arguments", return_value=args),
                    patch.object(
                        comparison, "verify_manifest", return_value=(files, (case,), {"name": "unit fixture"})
                    ),
                    patch.object(comparison, "CASES", (case,)),
                    patch.object(comparison, "verify_corpus", return_value=files),
                    patch.object(comparison, "require_permission", return_value={"unit-only": True}),
                    patch.object(comparison, "build_identity", return_value={"scope": "offline unit fixture"}),
                    patch.object(comparison, "tool_preflight", return_value=[]),
                    patch.object(comparison, "host_snapshot", return_value={"scope": "offline unit fixture"}),
                    patch.object(comparison, "measure_case", side_effect=observe) as measure,
                    patch.object(comparison, "timing_eligibility", return_value={"headline": False}),
                    patch.object(comparison, "result_record", return_value={"scope": "offline unit fixture"}),
                    contextlib.redirect_stdout(io.StringIO()),
                ):
                    if mutation in ("none", "historical"):
                        self.assertEqual(comparison.main(), 0)
                        self.assertEqual(json.loads(output.read_text()), {"scope": "offline unit fixture"})
                    else:
                        exception = (
                            FileExistsError
                            if mutation == "output"
                            else (ValueError if mutation == "missing-tool" else RuntimeError)
                        )
                        with self.assertRaises(exception):
                            comparison.main()
                        if mutation == "missing-tool":
                            measure.assert_not_called()
                        if mutation == "output":
                            self.assertEqual(output.read_bytes(), b"concurrent owner must survive")
                        else:
                            self.assertFalse(output.exists())

    # Purpose: Keep quoted, Unicode, tracked and untracked source changes out of published benchmark identities.
    # Inputs: Actual Git repositories with space/Unicode paths and independent unrelated document edits.
    # Outputs: Clean identities retain exact hashes; every production edit is rejected while notes remain allowed.
    def test_source_identity_uses_real_git_path_boundaries(self) -> None:
        for name in ("src/plain.cpp", "src/space name.cpp", "src/caf\u00e9.cpp"):
            with (
                self.subTest(name=name),
                source_fixture(name) as (root, binary, source),
                patch.object(comparison, "ROOT", root),
            ):
                commit, binary_hash, dirty = comparison.source_identity(binary)
                self.assertEqual(commit, fixture_git(root, "rev-parse", "HEAD").decode().strip())
                self.assertEqual(binary_hash, hashlib.sha256(binary.read_bytes()).hexdigest())
                self.assertEqual(dirty, [])
                (root / "notes.md").write_bytes(b"unrelated edit\n")
                self.assertEqual(comparison.source_identity(binary)[2], ["notes.md"])
                source.write_bytes(b"modified source\n")
                with self.assertRaisesRegex(RuntimeError, "product or comparison method is dirty"):
                    comparison.source_identity(binary)
                source.write_bytes(b"original source\n")
                (root / "src/untracked name.cpp").write_bytes(b"new source\n")
                with self.assertRaisesRegex(RuntimeError, "product or comparison method is dirty"):
                    comparison.source_identity(binary)

    # Purpose: Qualify the real controller's warmups, alternating rounds, byte checks and failure cleanup.
    # Inputs: Tiny owned files and explicit Python ZIP stand-ins, not product binaries or benchmark observations.
    # Outputs: Every retained archive is verified; corrupt/missing output fails and all owned case trees are removed.
    def test_measure_case_roundtrips_order_and_failure_cleanup(self) -> None:
        script = (
            "import sys,zipfile;from pathlib import Path\n"
            "action,archive,target,mode,*names=sys.argv[1:]\n"
            "if action=='create' and mode!='missing':\n"
            " with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED) as z:\n"
            "  for name in names: z.write(name,arcname=name)\n"
            "elif action=='extract':\n"
            " with zipfile.ZipFile(archive) as z:\n"
            "  for name in names:\n"
            "   value=z.read(name)\n"
            "   if mode=='corrupt': value=bytes([value[0]^1])+value[1:]\n"
            "   (Path(target)/name).write_bytes(value)\n"
        )
        for mode in ("valid", "missing", "corrupt"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                corpus, work = root / "corpus", root / "work"
                corpus.mkdir()
                work.mkdir()
                payload = b"owned controller fixture\x00\xff\n" * 7
                (corpus / "sample.txt").write_bytes(payload)
                manifest = {"sample.txt": {"bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}}
                case = ("offline-unit-control", "zip", ("sample.txt",), ("FlitzZip", "7-Zip"))

                # Purpose: Substitute fixed unit workers only. Inputs: Controller command fields. Outputs: Safe argv.
                def worker_commands(tool, _fmt, files, archive, target, _binaries, _level, mode=mode, script=script):
                    common = [sys.executable, "-c", script]
                    return (
                        [*common, "create", str(archive), str(target), mode, *files],
                        [*common, "extract", str(archive), str(target), mode, *files],
                    )

                with (
                    patch.object(comparison, "commands", side_effect=worker_commands),
                    patch.object(comparison, "run", wraps=comparison.run) as command,
                    patch.object(comparison.time, "sleep") as pause,
                ):
                    if mode != "valid":
                        with self.assertRaisesRegex(RuntimeError, "missing archive|SHA-256 differs"):
                            comparison.measure_case(case, {}, corpus, 2, manifest, work, round_pause_ms=1)
                    else:
                        record = comparison.measure_case(case, {}, corpus, 2, manifest, work, round_pause_ms=1)
                        self.assertEqual(record["input_bytes"], len(payload))
                        self.assertEqual(record["extract_reference_tool"], "7-Zip")
                        for result in record["results"]:
                            self.assertTrue(result["independent_verified"])
                            self.assertEqual(len(result["archive_sha256"]), 2)
                            self.assertEqual(len(result["archive_bytes"]), 2)
                            self.assertTrue(all(value > 0 for value in result["compress_seconds"]))
                            self.assertEqual(len(result["extract_seconds"]), 2)
                            self.assertEqual(result["extract_reference_sha256"], record["extract_reference_sha256"])
                        self.assertEqual(command.call_count, 18)
                        creates = [call.args[0][4] for call in command.call_args_list if call.args[0][3] == "create"]
                        self.assertEqual(
                            [Path(path).parent.name for path in creates],
                            [
                                "create--1-7-Zip",
                                "create--1-FlitzZip",
                                "create-0-FlitzZip",
                                "create-0-7-Zip",
                                "create-1-7-Zip",
                                "create-1-FlitzZip",
                            ],
                        )
                        self.assertEqual(pause.call_count, 4)
                        self.assertTrue(all(call.args == (0.001,) for call in pause.call_args_list))
                self.assertEqual(list(work.iterdir()), [])
                self.assertEqual((corpus / "sample.txt").read_bytes(), payload)

    # Purpose: Reject oversized filesystem cases before any command, directory creation or source allocation.
    # Inputs: A declaration one byte beyond the existing correctness-smoke budget.
    # Outputs: Explicit admission error and no subprocess invocation.
    def test_measure_case_admits_size_before_work(self) -> None:
        case = ("too-large", "zip", ("sample",), ("FlitzZip", "7-Zip"))
        with patch.object(comparison, "run") as command, self.assertRaisesRegex(ValueError, "64 MiB"):
            comparison.measure_case(
                case, {}, Path("unused"), 5, {"sample": {"bytes": comparison.MAX_FILESYSTEM_BYTES + 1}}, Path("unused")
            )
        command.assert_not_called()

    # Purpose: Require the declared Release/HIP/x64 configuration rather than any nearby executable or cache.
    # Inputs: Canonical owned path fixtures, an explicitly noncanonical alias and malformed CMake settings.
    # Outputs: Valid identity hashes the exact cache; wrong binary, absent cache, CPU-only and wrong arch fail closed.
    def test_build_identity_configuration_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            alias_parent = Path(temporary) / "existing-alias-parent"
            alias_parent.mkdir()
            root = (alias_parent / "..").resolve(strict=True)
            binary = root / "build/Release/flitzzip_cli.exe"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b"owned nonexecutable unit fixture")
            cache = root / "build/CMakeCache.txt"
            valid = "# unit fixture\nCMAKE_GENERATOR_PLATFORM:STRING=x64\nFLITZZIP_ENABLE_HIP:BOOL=ON\n"
            with patch.object(comparison, "ROOT", root):
                with self.assertRaisesRegex(ValueError, "repository x64 Release"):
                    comparison.build_identity(root / "other.exe")
                with self.assertRaisesRegex(ValueError, "cache is missing"):
                    comparison.build_identity(binary)
                for invalid in (valid.replace("x64", "Win32"), valid.replace("=ON", "=OFF"), ""):
                    cache.write_text(invalid)
                    with self.subTest(cache=invalid), self.assertRaisesRegex(ValueError, "HIP-enabled x64"):
                        comparison.build_identity(binary)
                cache.write_text(valid)
                record = comparison.build_identity(binary)
                self.assertEqual(record["cmake_cache_sha256"], hashlib.sha256(cache.read_bytes()).hexdigest())
                self.assertEqual(record["configuration"], "Release")
                self.assertTrue(record["hip_enabled"])
                alias = alias_parent / ".." / "build/Release/flitzzip_cli.exe"
                self.assertEqual(alias.resolve(strict=True), binary)
                with self.assertRaisesRegex(ValueError, "repository x64 Release"):
                    comparison.build_identity(alias)

    # Purpose: Preserve subprocess failures and reject impossible elapsed intervals instead of publishing fake speed.
    # Inputs: Actual owned Python successes/failures, plus controlled monotonic intervals and a timeout transport.
    # Outputs: Only positive finite intervals succeed; nonzero exits, timeouts and malformed timings propagate.
    def test_command_execution_and_wall_clock_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.assertGreater(comparison.run([sys.executable, "-c", "pass"], root), 0)
            self.assertEqual(comparison.run([sys.executable, "-c", "pass"], root, timed=False), 0)
            with self.assertRaisesRegex(RuntimeError, "code 7.*"):
                comparison.run(
                    [sys.executable, "-c", "import sys;print('unit failure',file=sys.stderr);sys.exit(7)"], root
                )
            for end in (0.0, -1.0, float("inf"), float("nan")):
                with (
                    patch.object(comparison.time, "perf_counter", side_effect=[0.0, end]),
                    self.subTest(end=end),
                    self.assertRaisesRegex(RuntimeError, "wall-clock interval"),
                ):
                    comparison.run([sys.executable, "-c", "pass"], root)
            with (
                patch.object(comparison.subprocess, "run", side_effect=subprocess.TimeoutExpired("unit", 120)),
                self.assertRaises(subprocess.TimeoutExpired),
            ):
                comparison.run([sys.executable], root)

    # Purpose: Enforce publication sample counts, method limits and new owned output before any workload starts.
    # Inputs: Real argparse calls with valid edge values and individually invalid options or output locations.
    # Outputs: Valid CLI intent is preserved; invalid requests exit two without creating output directories.
    def test_argument_admission_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "out/benchmarks/new.json"
            base = [
                "comparison",
                "--corpus",
                str(root),
                "--flitzzip",
                str(root / "unit.bin"),
                "--output",
                str(output),
                "--manifest",
                str(root / "unit.json"),
            ]
            with patch.object(comparison, "ROOT", root):
                for runs, pause in ((5, 0), (10, 1000)):
                    with patch.object(sys, "argv", [*base, "--runs", str(runs), "--round-pause-ms", str(pause)]):
                        args = comparison.parse_arguments()
                        self.assertEqual((args.runs, args.round_pause_ms), (runs, pause))
                for options in (
                    ("--runs", "4"),
                    ("--runs", "11"),
                    ("--round-pause-ms", "-1"),
                    ("--round-pause-ms", "1001"),
                    ("--level", "10"),
                    ("--output", str(root / "outside.json")),
                ):
                    with (
                        self.subTest(options=options),
                        patch.object(sys, "argv", [*base, *options]),
                        contextlib.redirect_stderr(io.StringIO()),
                        self.assertRaises(SystemExit) as failed,
                    ):
                        comparison.parse_arguments()
                    self.assertEqual(failed.exception.code, 2)
                with (
                    patch.object(sys, "argv", base[:-2]),
                    contextlib.redirect_stderr(io.StringIO()),
                    self.assertRaises(SystemExit),
                ):
                    comparison.parse_arguments()
                self.assertFalse(output.parent.exists())
                output.parent.mkdir(parents=True)
                output.write_bytes(b"preserve existing result")
                with (
                    patch.object(sys, "argv", base),
                    contextlib.redirect_stderr(io.StringIO()),
                    self.assertRaises(SystemExit),
                ):
                    comparison.parse_arguments()
                self.assertEqual(output.read_bytes(), b"preserve existing result")

    # Purpose: Check executable permissions and release identity before using any external comparator version banner.
    # Inputs: Owned nonexecutable byte fixtures and mocked version/permission transports, never installed comparators.
    # Outputs: Exact argv, hashes and release decisions survive; permission or version failure stops preflight.
    def test_tool_preflight_identity_and_failure_order(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "not-executable.bin"
            binary.write_bytes(b"owned tool identity fixture")
            binaries = {name: binary for name in ("FlitzZip", "7-Zip", "Zstd")}
            with (
                patch.object(comparison, "require_permission", return_value={"unit-permission": True}) as permission,
                patch.object(comparison, "fetch_release_source", return_value=b"unit API response") as fetch,
                patch.object(comparison, "parse_release", return_value="unit-version"),
                patch.object(comparison, "require_binary_identity") as identity,
                patch.object(comparison, "require_current_release", return_value={"unit-release": True}) as release,
                patch.object(
                    comparison.subprocess,
                    "run",
                    return_value=SimpleNamespace(returncode=0, stdout="\n  unit version\n", stderr=""),
                ) as execute,
            ):
                record = comparison.tool_preflight(binaries, ("owned unit corpus",))
                self.assertEqual([row["name"] for row in record], list(binaries))
                self.assertTrue(
                    all(row["binary_sha256"] == hashlib.sha256(binary.read_bytes()).hexdigest() for row in record)
                )
                self.assertEqual(
                    [call.args[0] for call in execute.call_args_list],
                    [[str(binary), "--version"], [str(binary), "i"], [str(binary), "--version"]],
                )
                self.assertEqual([call.args[0] for call in fetch.call_args_list], ["7-Zip", "Zstd"])
                self.assertEqual(identity.call_count, 2)
                self.assertEqual(release.call_count, 2)
                self.assertEqual(permission.call_args_list[0].args, ("owned unit corpus", "execute"))
                execute.return_value = SimpleNamespace(returncode=0, stdout="\n", stderr="")
                self.assertEqual(comparison.tool_preflight({"FlitzZip": binary})[0]["version"], "unknown")
                execute.return_value = SimpleNamespace(returncode=4, stdout="", stderr="unit failure")
                with self.assertRaisesRegex(RuntimeError, "could not read version"):
                    comparison.tool_preflight({"FlitzZip": binary})
                execute.reset_mock()
                permission.side_effect = ValueError("unit permission denied")
                with self.assertRaisesRegex(ValueError, "permission denied"):
                    comparison.tool_preflight(binaries)
                execute.assert_not_called()

    # Purpose: Keep raw result metadata complete without leaking unrelated paths or merging corpus identities.
    # Inputs: Explicit unit observations with required host fields and either licensed or historical corpus metadata.
    # Outputs: Exact bytes, settings and source identity are retained; the independently sampled final host is present.
    def test_result_record_preserves_metadata_scope(self) -> None:
        fields = (
            "os_name",
            "os_build",
            "cpu_model",
            "cpu_cores",
            "cpu_threads",
            "gpu_model",
            "gpu_driver",
            "ram_bytes",
            "ram_modules",
            "ram_configured_mts",
            "storage_model",
            "workspace_bus_type",
        )
        host = dict.fromkeys(fields, "unit fixture, not host evidence")
        args = SimpleNamespace(level=5, runs=5, round_pause_ms=250)
        for corpus in (None, {"name": "owned unit corpus"}):
            with patch.object(comparison, "host_snapshot", return_value={"final-unit-sample": True}):
                record = comparison.result_record(
                    args, "a" * 40, "b" * 64, ["private/unrelated/path"], {"unit-build": True}, [], [], [], host, corpus
                )
            self.assertEqual(record["schema_version"], 2 if corpus else 1)
            self.assertNotIn("private/unrelated/path", json.dumps(record))
            self.assertEqual(record["unrelated_dirty_count"], 1)
            self.assertEqual(record["source_commit"], "a" * 40)
            self.assertEqual(record["host"]["resource_after"], {"final-unit-sample": True})
            self.assertEqual(record["settings"]["runs"], 5)
            self.assertEqual(record["corpus"]["name"], "owned unit corpus" if corpus else "Silesia selected files")

    # Purpose: Exercise real CLI entry points without launching a product, crawling, downloading or benchmarking.
    # Inputs: Help and a missing local manifest under a new ignored result filename.
    # Outputs: Help exits zero; missing input exits one with a diagnostic and no output publication.
    def test_cli_reports_input_failure_without_publication(self) -> None:
        output = comparison.ROOT / "out/benchmarks" / f"unit-cli-{uuid.uuid4().hex}.json"
        command = [sys.executable, "-m", "tools.run_archive_comparison"]
        helped = subprocess.run([*command, "--help"], capture_output=True, text=True, timeout=15, check=False)
        self.assertEqual(helped.returncode, 0, helped.stderr)
        self.assertIn("--round-pause-ms", helped.stdout)
        with tempfile.TemporaryDirectory() as temporary:
            missing = Path(temporary) / "missing"
            failed = subprocess.run(
                [
                    *command,
                    "--corpus",
                    str(missing),
                    "--manifest",
                    str(missing),
                    "--flitzzip",
                    str(missing),
                    "--output",
                    str(output),
                ],
                capture_output=True,
                text=True,
                timeout=15,
                check=False,
            )
        self.assertEqual(failed.returncode, 1)
        self.assertIn("comparison failed:", failed.stderr)
        self.assertFalse(output.exists())

    # Purpose: Reject source moves in either direction even when the other rename endpoint is unrelated.
    # Inputs: Real staged Git renames between src and docs, including whitespace in the destination.
    # Outputs: Both endpoints participate in the source-change guard; neither rename is accepted as clean.
    def test_source_identity_checks_both_rename_endpoints(self) -> None:
        for origin, destination in (("docs/input.cpp", "src/new name.cpp"), ("src/input.cpp", "docs/new name.cpp")):
            with self.subTest(origin=origin), source_fixture(origin) as (root, binary, _):
                (root / Path(destination).parent).mkdir(exist_ok=True)
                fixture_git(root, "mv", origin, destination)
                with patch.object(comparison, "ROOT", root), self.assertRaisesRegex(RuntimeError, "is dirty"):
                    comparison.source_identity(binary)

    # Purpose: Verify replacement-corpus provenance, permissions and actual bytes before any product execution.
    # Inputs: An owned temporary input and manifest with a mocked reviewed permission decision.
    # Outputs: Exact input/cases pass; malformed metadata, wrong bytes and denied permissions fail.
    def test_licensed_manifest(self) -> None:
        payload = b"owned synthetic CSV fixture\n"
        source = "https://archive.ics.uci.edu/dataset/280/higgs"
        spec = {
            "schema_version": 1,
            "name": "owned offline fixture, not benchmark data",
            "files": [
                {
                    "name": "sample.csv",
                    "bytes": len(payload),
                    "sha256": hashlib.sha256(payload).hexdigest(),
                    "subject": "HIGGS",
                    "source_url": source,
                    "transformation": "owned test fixture, no downloaded data",
                }
            ],
            "cases": [{"name": "numeric", "format": "zst", "files": ["sample.csv"], "tools": ["FlitzZip", "Zstd"]}],
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            corpus = root / "raw"
            corpus.mkdir()
            raw = corpus / "sample.csv"
            raw.write_bytes(payload)
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps(spec), encoding="utf-8")
            with patch.object(comparison, "require_permission", return_value={"evidence": [source]}) as permission:
                files, cases, metadata = comparison.verify_manifest(manifest, corpus)
                self.assertEqual(files, spec["files"])
                self.assertEqual(cases, (("numeric", "zst", ("sample.csv",), ("FlitzZip", "Zstd")),))
                self.assertEqual(metadata["manifest_sha256"], comparison.digest(manifest))
                permission.assert_called_once_with("HIGGS", "execute")
                for field, value in (
                    ("name", "../escape"),
                    ("name", "-switch"),
                    ("name", "NUL.csv"),
                    ("name", "COM1.txt"),
                    ("name", "trailing."),
                    ("bytes", True),
                    ("bytes", 65 * 1024**2),
                    ("sha256", "bad"),
                    ("source_url", "https://unreviewed.invalid/data"),
                    ("transformation", ""),
                    ("subject", ""),
                    ("bytes", len(payload) + 1),
                ):
                    modified = copy.deepcopy(spec)
                    modified["files"][0][field] = value
                    manifest.write_text(json.dumps(modified), encoding="utf-8")
                    with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                        comparison.verify_manifest(manifest, corpus)
                for invalid in (
                    {**spec, "schema_version": True},
                    {**spec, "name": ""},
                    {**spec, "name": "n" * 121},
                    {**spec, "files": []},
                    {**spec, "files": [spec["files"][0]] * 129},
                    {**spec, "files": [{}]},
                    {**spec, "cases": []},
                    {**spec, "unexpected": True},
                    {**spec, "files": [spec["files"][0], spec["files"][0]]},
                    {**spec, "cases": spec["cases"] * 2},
                ):
                    manifest.write_text(json.dumps(invalid), encoding="utf-8")
                    with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                        comparison.verify_manifest(manifest, corpus)
                unused = copy.deepcopy(spec)
                unused["files"].append({**spec["files"][0], "name": "unused.csv"})
                (corpus / "unused.csv").write_bytes(payload)
                manifest.write_text(json.dumps(unused), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "unused source files"):
                    comparison.verify_manifest(manifest, corpus)
                manifest.write_text(json.dumps(spec), encoding="utf-8")
                raw.write_bytes(b"x" * len(payload))
                with self.assertRaisesRegex(ValueError, "SHA-256 differs"):
                    comparison.verify_manifest(manifest, corpus)
            with (
                patch.object(comparison, "require_permission", side_effect=ValueError("permission denied")),
                self.assertRaisesRegex(ValueError, "permission denied"),
            ):
                comparison.verify_manifest(manifest, corpus)

    # Purpose: Reject case mutations that change the measurement or exceed the filesystem budget.
    # Inputs: Incompatible formats/tools, repeated inputs and oversized aggregate cases.
    # Outputs: No unsupported case reaches a subprocess; valid ZIP and Zstd pairings normalize exactly.
    def test_manifest_case_boundaries(self) -> None:
        files = {"first": {"bytes": 33 * 1024**2}, "second": {"bytes": 33 * 1024**2}}
        row = {"name": "files", "format": "zip", "files": ["first"], "tools": ["FlitzZip", "7-Zip"]}
        for invalid in (None, [], {**row, "unknown": True}):
            with self.subTest(case=invalid), self.assertRaisesRegex(ValueError, "fields are missing or unknown"):
                comparison.manifest_case(invalid, files)
        self.assertEqual(comparison.manifest_case(row, files), ("files", "zip", ("first",), ("FlitzZip", "7-Zip")))
        for field, value in (
            ("name", "../escape"),
            ("format", "flzip"),
            ("format", None),
            ("files", ["missing"]),
            ("files", ["first", "first"]),
            ("files", ["first", "second"]),
            ("files", [False]),
            ("tools", ["FlitzZip", "FlitzZip"]),
            ("tools", ["7-Zip", "FlitzZip"]),
            ("tools", ["FlitzZip", "WinZip"]),
        ):
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                comparison.manifest_case({**row, field: value}, files)
        with self.assertRaises(ValueError):
            comparison.manifest_case(
                {**row, "format": "zst", "files": ["first", "second"], "tools": ["FlitzZip", "Zstd"]}, files
            )

    # Purpose: Preserve all nine efforts and reject malformed values before starting a process.
    # Inputs: Each supported software/format pairing and valid or invalid effort values.
    # Outputs: Exact effort switches pass; non-integers and out-of-range efforts fail.
    def test_all_effort_commands(self) -> None:
        for level in range(1, 10):
            for _, fmt, files, tools in comparison.CASES:
                for tool in tools:
                    with self.subTest(level=level, tool=tool, fmt=fmt):
                        create, extract = comparison.command_templates(tool, fmt, files, level)
                        switch = {"FlitzZip": str(level), "7-Zip": f"-mx={level}", "Zstd": f"-{level}"}[tool]
                        self.assertIn(switch, create)
                        self.assertTrue(extract)
        for level in (0, 10, -1, True, False, 5.0, "5", None):
            with self.subTest(level=level), self.assertRaisesRegex(ValueError, "unsupported comparison effort"):
                comparison.command_templates("FlitzZip", "zip", ("sample",), level)
        for tool, fmt, files in (
            ("7-Zip", "zst", ("sample",)),
            ("Zstd", "zip", ("sample",)),
            ("Zstd", "zst", ("a", "b")),
            ("unknown", "zip", ("sample",)),
        ):
            with self.subTest(tool=tool, fmt=fmt), self.assertRaisesRegex(ValueError, "unsupported tool/format"):
                comparison.command_templates(tool, fmt, files)

    # Purpose: Require a real total-processor counter rather than a nullable per-CPU CIM field.
    # Inputs: Simulated Windows performance-counter output, then a missing total-load value.
    # Outputs: A valid snapshot passes and the missing counter fails closed.
    def test_host_snapshot_requires_total_processor_load(self) -> None:
        sample = {
            "cpu_model": "Test CPU",
            "cpu_cores": 8,
            "cpu_threads": 16,
            "gpu_model": "Test GPU",
            "gpu_driver": "1",
            "os_name": "Windows",
            "os_build": "1",
            "ram_bytes": 16 * 1024**3,
            "ram_modules": 2,
            "ram_configured_mts": 5200,
            "storage_model": "Test SSD",
            "cpu_load_percent": 2,
            "free_ram_bytes": 8 * 1024**3,
            "paging_pages_per_second": 0,
            "disk_busy_percent": 0,
            "gpu_engine_max_percent": 0,
            "workspace_bus_type": "NVMe",
        }
        with (
            patch.object(
                comparison.subprocess,
                "run",
                return_value=SimpleNamespace(returncode=0, stdout=json.dumps(sample), stderr=""),
            ) as command,
            patch.object(comparison, "ROOT", PureWindowsPath("C:/FlitzZip")),
        ):
            self.assertEqual(comparison.host_snapshot()["cpu_load_percent"], 2)
            self.assertIn("Win32_PerfFormattedData_PerfOS_Processor", command.call_args.args[0][-1])
            self.assertIn("Get-Partition -DriveLetter 'C'", command.call_args.args[0][-1])
        sample["cpu_load_percent"] = None
        with (
            patch.object(
                comparison.subprocess,
                "run",
                return_value=SimpleNamespace(returncode=0, stdout=json.dumps(sample), stderr=""),
            ),
            patch.object(comparison, "ROOT", PureWindowsPath("C:/FlitzZip")),
            self.assertRaisesRegex(RuntimeError, "missing required fields"),
        ):
            comparison.host_snapshot()

    # Purpose: Accept only an exact corpus payload with the pinned SHA-256 digest.
    # Inputs: One temporary file and patched small-file manifest.
    # Outputs: Matching bytes pass; a one-byte edit fails.
    def test_corpus_hash_gate(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            raw = Path(temporary) / "raw"
            downloads = Path(temporary) / "downloads"
            raw.mkdir()
            downloads.mkdir()
            path = raw / "sample"
            path.write_bytes(b"known corpus")
            (downloads / "sample.bz2").write_bytes(bz2.compress(b"known corpus"))
            expected = {"sample": (path.stat().st_size, hashlib.sha256(b"known corpus").hexdigest())}
            with patch.dict(comparison.EXPECTED, expected, clear=True):
                with self.assertRaisesRegex(ValueError, "directory does not exist"):
                    comparison.verify_corpus(Path(temporary) / "missing", downloads)
                self.assertEqual(comparison.verify_corpus(raw, downloads)[0]["name"], "sample")
                downloaded = downloads / "sample.bz2"
                downloaded.unlink()
                with self.assertRaisesRegex(ValueError, "original corpus download is missing"):
                    comparison.verify_corpus(raw, downloads)
                downloaded.write_bytes(bz2.compress(b"known corpus plus trailing bytes"))
                with self.assertRaisesRegex(ValueError, "expands beyond expected size"):
                    comparison.verify_corpus(raw, downloads)
                (downloads / "sample.bz2").write_bytes(bz2.compress(b"wrong corpus"))
                with self.assertRaisesRegex(ValueError, "download differs"):
                    comparison.verify_corpus(raw, downloads)
                (downloads / "sample.bz2").write_bytes(bz2.compress(b"known corpus"))
                path.write_bytes(b"wrong corpus")
                with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                    comparison.verify_corpus(raw, downloads)
                path.unlink()
                with self.assertRaisesRegex(ValueError, "file missing, linked, or wrong size"):
                    comparison.verify_corpus(raw, downloads)

    # Purpose: Keep independent extraction output and cleanup inside owned paths.
    # Inputs: One expected file plus an unrelated outside file.
    # Outputs: Exact tree passes; extra content and outside deletion are refused.
    def test_tree_and_cleanup_boundary(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            owned = root / "owned"
            owned.mkdir()
            target = owned / "decoded"
            target.mkdir()
            (target / "sample").write_bytes(b"abc")
            manifest = {"sample": {"bytes": 3, "sha256": hashlib.sha256(b"abc").hexdigest()}}
            comparison.verify_tree(target, ("sample",), manifest)
            (target / "sample").write_bytes(b"a")
            with self.assertRaisesRegex(RuntimeError, "size or type differs"):
                comparison.verify_tree(target, ("sample",), manifest)
            (target / "sample").write_bytes(b"abc")
            unexpected_directory = target / "unexpected-empty-directory"
            unexpected_directory.mkdir()
            with self.assertRaisesRegex(RuntimeError, "paths differ"):
                comparison.verify_tree(target, ("sample",), manifest)
            unexpected_directory.rmdir()
            (target / "extra").write_bytes(b"x")
            with self.assertRaisesRegex(RuntimeError, "paths differ"):
                comparison.verify_tree(target, ("sample",), manifest)
            outside = root / "unrelated"
            outside.write_bytes(b"leave intact")
            with self.assertRaisesRegex(RuntimeError, "outside"):
                comparison.discard(outside, owned)
            self.assertEqual(outside.read_bytes(), b"leave intact")
            owned_file = owned / "owned-file"
            owned_file.write_bytes(b"discard only this")
            comparison.discard(owned_file, owned)
            self.assertFalse(owned_file.exists())
            comparison.discard(owned_file, owned)


if __name__ == "__main__":
    unittest.main()
