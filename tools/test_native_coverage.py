"""Independent artifact and measurement-boundary regressions for native coverage."""

import copy
import hashlib
import io
import json
import os
import re
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path
from unittest.mock import patch
from urllib.parse import urlsplit
from urllib.request import Request

from tools import native_coverage as collector
from tools import native_coverage_report as reports
from tools.native_coverage_report import normalize_report, read_report, source_path


class CoverageTests(unittest.TestCase):
    # Purpose: Enforce the coverage XML byte boundary on the opened stream with small real files.
    # Inputs: A complete document exactly at a reduced test budget, followed by one excess byte.
    # Outputs: Exact-boundary XML parses; excess input fails without publishing a normalized report.
    def test_xml_exact_stream_budget(self):
        payload = b"<coverage/>".ljust(64, b" ")
        with patch.object(reports, "MAX_REPORT_BYTES", len(payload)):
            self.raw.write_bytes(payload)
            self.assertEqual(read_report(self.raw).tag, "coverage")
            self.raw.write_bytes(payload + b" ")
            with self.assertRaisesRegex(ValueError, "report contract"):
                read_report(self.raw)
        self.assertFalse(self.output.exists())

    # Purpose: Preserve valid Cobertura default roots while rejecting classes without executable evidence.
    # Inputs: A source-root-free report with an exact repository-relative filename and a class lacking lines.
    # Outputs: Real source line hits normalize unchanged; an empty class never publishes a report.
    def test_default_roots_and_empty_class_evidence(self):
        xml = self.xml.replace("<sources><source>src</source></sources>", "")
        xml = xml.replace('filename="example.cpp"', 'filename="src/example.cpp"')
        self.raw.write_text(xml, encoding="utf-8")
        summary = normalize_report(self.raw, self.output, self.root)
        self.assertEqual(summary["lines_valid"], 2)
        self.assertEqual(summary["lines_covered"], 1)
        self.assertEqual(summary["uncovered_lines"], {"src/example.cpp": [2]})
        self.output.unlink()
        document = ET.fromstring(xml)
        document.find("./packages/package/classes/class").remove(document.find(".//lines"))
        self.raw.write_bytes(ET.tostring(document))
        with self.assertRaisesRegex(ValueError, "executable line evidence"):
            normalize_report(self.raw, self.output, self.root)
        self.assertFalse(self.output.exists())

    # Purpose: Reject an authentic-sized collector download whose final EOF crosses the transport deadline.
    # Inputs: Inert installer bytes, an offline response, owned output paths and an installer-execution double.
    # Outputs: Late EOF prevents installer execution; the same timely bytes reach normal installer admission.
    def test_collector_download_deadline_includes_eof(self):
        version = "0.9.9.0"
        payload = b"inert collector transport fixture"
        lock = {
            "version": version,
            "url": f"https://github.com/OpenCppCoverage/OpenCppCoverage/releases/download/release-{version}/"
            f"OpenCppCoverageSetup-x64-{version}.exe",
            "installer_bytes": len(payload),
            "installer_sha256": hashlib.sha256(payload).hexdigest(),
            "files": {},
        }
        output = self.root / "out/acquisition"
        output.mkdir(parents=True)
        for elapsed, accepted in ((2, True), (121, False)):
            response = io.BytesIO(payload)
            with (
                self.subTest(elapsed=elapsed),
                patch.object(collector, "ROOT", self.root),
                patch.object(collector, "build_opener") as opener,
                patch.object(collector.time, "monotonic", side_effect=[0, 1, elapsed]),
                patch.object(collector, "run_checked") as install,
                patch.object(collector, "verify_collector", return_value=self.root / "inert-collector.exe"),
            ):
                opener.return_value.open.return_value = response
                if accepted:
                    self.assertEqual(collector.provision(lock, output), self.root / "inert-collector.exe")
                    install.assert_called_once()
                else:
                    with self.assertRaisesRegex(TimeoutError, "deadline"):
                        collector.provision(lock, output)
                    install.assert_not_called()

    # Purpose: Own small source and binary fixtures. Inputs: None. Outputs: Cleanup-bound test directory.
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        (self.root / "src").mkdir()
        (self.root / "src/example.cpp").write_text("first\nsecond\nthird\n", encoding="utf-8")
        self.raw = self.root / "raw.xml"
        self.output = self.root / "native.xml"
        self.xml = '<coverage><sources><source>src</source></sources><packages><package name="native"><classes>'
        self.xml += '<class name="example" filename="example.cpp"><lines><line number="1" hits="2"/>'
        self.xml += '<line number="2" hits="0"/></lines></class></classes></package></packages></coverage>'

    # Purpose: Keep literal PowerShell helper imports in the measured GUI scenario's source closure.
    # Inputs: Actual scenario files and their PSScriptRoot-relative imports. Outputs: Missing helper identities fail.
    def test_gui_scenarios_bind_imported_tool_sources(self):
        pattern = r"""(?:Import-Module|\.)\s+\(Join-Path\s+\$PSScriptRoot\s+["']([^"']+)["']\)"""
        imports = set()
        for name in collector.GUI_SCENARIO_INPUTS:
            text = (collector.ROOT / name).read_text(encoding="utf-8-sig")
            imports.update((Path(name).parent / relative).as_posix() for relative in re.findall(pattern, text))
        self.assertTrue(imports, "The real GUI driver must expose its imported helper dependencies")
        self.assertLessEqual(imports, set(collector.GUI_SCENARIO_INPUTS))

    # Purpose: Prove normalization retains executed and unexecuted lines rather than manufacturing coverage.
    # Inputs: Hand-authored source roots and line evidence.
    # Outputs: Portable paths, unchanged hits and independent totals.
    def test_report_preserves_hits_and_uncovered_lines(self):
        self.raw.write_text(self.xml, encoding="utf-8")
        summary = normalize_report(self.raw, self.output, self.root)
        self.assertEqual(
            summary,
            {
                "files": ["src/example.cpp"],
                "lines_valid": 2,
                "lines_covered": 1,
                "uncovered_lines": {"src/example.cpp": [2]},
            },
        )
        tree = read_report(self.output)
        self.assertEqual(tree.find("./sources/source").text, ".")
        self.assertEqual(tree.find(".//class").get("filename"), "src/example.cpp")
        self.assertEqual(
            [line.attrib for line in tree.findall(".//line")],
            [{"number": "1", "hits": "2"}, {"number": "2", "hits": "0"}],
        )

    # Purpose: Reject empty, foreign, malformed and stale-source measurements before publication.
    # Inputs: Independent invalid records. Outputs: Explicit failures with no published report.
    def test_invalid_evidence_never_publishes(self):
        fixtures = [
            self.xml.replace('hits="2"', 'hits="0"'),
            self.xml.replace('hits="2"', 'hits="-1"'),
            self.xml.replace('number="1"', 'number="4"'),
            self.xml.replace('number="1"', 'number="0"'),
            self.xml.replace('filename="example.cpp"', 'filename="missing.cpp"'),
            "<coverage><packages/></coverage>",
            "<invalid/>",
            '<!DOCTYPE coverage [<!ENTITY example "expanded">]>' + self.xml,
        ]
        for value in fixtures:
            with self.subTest(value=value):
                self.raw.write_text(value, encoding="utf-8")
                with self.assertRaises(ValueError):
                    normalize_report(self.raw, self.output, self.root)
                self.assertFalse(self.output.exists())

    # Purpose: Keep source identity inside first-party source even when XML supplies outside roots.
    # Inputs: Traversal, absolute foreign sources and conflicting source roots. Outputs: Refusal, never guessed mapping.
    def test_foreign_and_ambiguous_sources(self):
        (self.root / "foreign.cpp").write_text("outside\n", encoding="utf-8")
        for name, roots in [("../foreign.cpp", ["src"]), (str(self.root / "foreign.cpp"), ["src"])]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                source_path(name, roots, self.root)
        (self.root / "src/second").mkdir()
        (self.root / "src/second/example.cpp").write_text("other\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            source_path("example.cpp", ["src", "src/second"], self.root)

    # Purpose: Resolve the collector's actual Windows drive-root Cobertura representation.
    # Inputs: Owned source expressed relative to its drive. Outputs: Exact mapping without using the drive's cwd.
    @unittest.skipUnless(os.name == "nt", "The native collector and Windows drive paths require Windows")
    def test_collector_drive_root_is_absolute(self):
        source = self.root / "src/example.cpp"
        name = source.relative_to(source.anchor).as_posix()
        self.assertEqual(source_path(name, [source.drive], self.root), "src/example.cpp")
        with self.assertRaises(ValueError):
            source_path("unrelated-repository/src/example.cpp", [source.drive], self.root)

    # Purpose: Merge repeated header line evidence without counting duplicate declarations as extra lines.
    # Inputs: Two measured classes for one source. Outputs: Union of real line hits and untouched measured classes.
    def test_duplicate_source_lines_are_counted_once(self):
        element = ET.fromstring(self.xml)
        classes = element.find("./packages/package/classes")
        repeated = copy.deepcopy(classes[0])
        repeated.set("name", "another module")
        repeated.find("./lines/line[@number='2']").set("hits", "1")
        classes.append(repeated)
        self.raw.write_bytes(ET.tostring(element))
        summary = normalize_report(self.raw, self.output, self.root)
        self.assertEqual((summary["lines_valid"], summary["lines_covered"]), (2, 2))
        self.assertEqual(summary["uncovered_lines"], {})
        self.assertEqual(len(read_report(self.output).findall(".//class")), 2)

    # Purpose: Reject entity expansion independently of XML encoding and use the same reader for all consumers.
    # Inputs: Entity-bearing UTF-8/16/32 and ordinary UTF-16 documents. Outputs: Refusal before parsing expansion.
    def test_xml_reader_rejects_encoded_entities(self):
        entity = '<!DOCTYPE coverage [<!ENTITY repeated "payload">]><coverage>&repeated;</coverage>'
        for encoding in ("utf-8", "utf-16", "utf-16-le", "utf-32", "utf-32-le"):
            with self.subTest(encoding=encoding):
                self.raw.write_bytes(entity.encode(encoding))
                with self.assertRaises(ValueError):
                    read_report(self.raw)
        self.raw.write_bytes(self.xml.encode("utf-16-le"))
        with self.assertRaises(ValueError):
            read_report(self.raw)

    # Purpose: Exercise the normalization boundary with an entity disguised by UTF-16 byte layout.
    # Inputs: Otherwise valid source/hit evidence with an entity-valued hit count. Outputs: No report publication.
    def test_normalizer_rejects_encoded_entity_attribute(self):
        xml = '<!DOCTYPE coverage [<!ENTITY observed "2">]>' + self.xml.replace('hits="2"', 'hits="&observed;"')
        self.raw.write_bytes(xml.encode("utf-16-le"))
        with self.assertRaises(ValueError):
            normalize_report(self.raw, self.output, self.root)
        self.assertFalse(self.output.exists())

    # Purpose: Prove a trusted initial download cannot redirect to an unauthorized transport or host.
    # Inputs: Independent scheme, authority and credential mutations. Outputs: Every hostile hop fails before access.
    def test_installer_redirects_remain_on_official_https(self):
        initial = "https://github.com/OpenCppCoverage/OpenCppCoverage/releases/download/release-0.9.9.0/installer.exe"
        valid = "https://release-assets.githubusercontent.com/release-asset?signature=public-fixture"
        handler = collector.ReleaseRedirects()
        request = Request(initial)
        self.assertEqual(handler.redirect_request(request, None, 302, "Found", {}, valid).full_url, valid)
        parsed = urlsplit(valid)
        mutations = [
            parsed._replace(scheme="file"),
            parsed._replace(scheme="http"),
            parsed._replace(netloc="github.com.example.invalid"),
            parsed._replace(netloc="192.0.2.1"),  # RFC 5737 documentation address; IP hosts are never admitted.
            parsed._replace(netloc="user@github.com"),
            parsed._replace(netloc="github.com:444"),
            parsed._replace(fragment="unexpected"),
        ]
        for changed in mutations:
            with self.subTest(url=changed.geturl()), self.assertRaises(ValueError):
                handler.redirect_request(request, None, 302, "Found", {}, changed.geturl())

    # Purpose: Reject changed collector code and unexpected runtime libraries.
    # Inputs: A complete binary fixture and separate mutations. Outputs: Identity-bound acceptance or refusal.
    def test_collector_inventory_and_hashes(self):
        directory = self.root / "collector"
        directory.mkdir()
        files = {"OpenCppCoverage.exe": b"collector fixture", "runtime.dll": b"runtime fixture"}
        hashes = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
        for name, data in files.items():
            (directory / name).write_bytes(data)
        self.assertEqual(collector.verify_collector(directory, hashes), directory / "OpenCppCoverage.exe")
        (directory / "runtime.dll").write_bytes(b"changed")
        with self.assertRaises(ValueError):
            collector.verify_collector(directory, hashes)
        (directory / "runtime.dll").write_bytes(files["runtime.dll"])
        (directory / "unexpected.dll").write_bytes(b"extra")
        with self.assertRaises(ValueError):
            collector.verify_collector(directory, hashes)
        with self.assertRaises(ValueError):
            collector.owned_path(self.root, self.root / "../outside")

    # Purpose: Preserve collector failures, deadlines and excess output as failed qualification.
    # Inputs: Controlled outcomes from the real shared process boundary. Outputs: Retained diagnostics and exception.
    def test_child_failures_remain_failures(self):
        outcome = {"exit_code": 0, "timed_out": False, "output_limit_exceeded": False}
        for field, value in [("exit_code", 9), ("timed_out", True), ("output_limit_exceeded", True)]:
            failure = {**outcome, field: value}
            with self.subTest(field=field), patch.object(collector, "process_owner") as owner:
                owner.return_value.run_bounded_command.return_value = failure
                with self.assertRaises(RuntimeError):
                    collector.run_checked(["owned-test-command"], 2, self.root / "process.json")
                self.assertEqual(json.loads((self.root / "process.json").read_text()), failure)

    # Purpose: Prevent a failed, truncated or successful CLI process from satisfying an expected rejection.
    # Inputs: Independent process outcomes. Outputs: Raw evidence is retained and every invalid outcome fails.
    def test_cli_admission_requires_complete_expected_failure(self):
        valid = {
            "exit_code": 1,
            "stdout": "",
            "stderr": "error: expected rejection\r\n",
            "timed_out": False,
            "output_truncated": False,
            "output_limit_exceeded": False,
        }
        changes = [
            {"exit_code": 0},
            {"exit_code": 2},
            {"stderr": "different failure"},
            {"stdout": "entries=1\n"},
            {"timed_out": True},
            {"output_truncated": True},
            {"output_limit_exceeded": True},
        ]
        log = self.root / "cli-case.json"
        case = (["invalid-input"], 1, "expected rejection")
        with patch.object(collector, "process_owner") as owner:
            for change in changes:
                outcome = {**valid, **change}
                owner.return_value.run_bounded_command.return_value = outcome
                with self.subTest(change=change), self.assertRaises(RuntimeError):
                    collector.run_cli_admission_case(self.root / "cli.exe", case, log)
                self.assertEqual(json.loads(log.read_text())["result"], outcome)
            owner.return_value.run_bounded_command.return_value = valid
            collector.run_cli_admission_case(self.root / "cli.exe", case, log)
            usage = {**valid, "exit_code": 2, "stderr": "", "stdout": "FlitzZip CLI\r\nUsage:\r\n"}
            owner.return_value.run_bounded_command.return_value = usage
            collector.run_cli_admission_case(self.root / "cli.exe", ([], 2, None), log)
            for field, value in (("stdout", ""), ("stderr", "unexpected error")):
                owner.return_value.run_bounded_command.return_value = {**usage, field: value}
                with self.assertRaises(RuntimeError):
                    collector.run_cli_admission_case(self.root / "cli.exe", ([], 2, None), log)

    # Purpose: Detect rejection paths that modify caller bytes or leave publication debris despite correct errors.
    # Inputs: Controlled mutation after one case. Outputs: Immediate refusal; clean repeated cases remain admissible.
    def test_cli_rejection_checks_files_and_publication_debris(self):
        for mutation in (None, "input with spaces.txt", "preserved.zip", "independent.zip", "unexpected.tmp"):
            # Purpose: Model a CLI side effect independently from its exit status.
            # Inputs: Owned case paths. Outputs: A report and the selected unwanted write.
            def execute_case(binary, case, log, selected_mutation=mutation):
                log.write_text("{}\n")
                if selected_mutation:
                    (log.parent / selected_mutation).write_bytes(b"changed")

            with (
                self.subTest(mutation=mutation),
                patch.object(collector, "ROOT", self.root),
                patch.object(collector, "cli_admission_cases", return_value=[([], 2, None)] * 2),
                patch.object(collector, "run_cli_capability_contracts", return_value=[]),
                patch.object(collector, "run_cli_admission_case", side_effect=execute_case) as run,
            ):
                if mutation:
                    with self.assertRaisesRegex(RuntimeError, "changed caller files"):
                        collector.run_cli_admission(self.root / "cli.exe", self.root)
                    self.assertEqual(run.call_count, 1)
                else:
                    collector.run_cli_admission(self.root / "cli.exe", self.root)
                    self.assertEqual(run.call_count, 2)

    # Purpose: Prove ZIP-only rejection cases reach format admission using an independently readable valid archive.
    # Inputs: Native collector fixture builder and no child execution. Outputs: Exact stored content and both readers.
    def test_compatibility_rejection_fixture_is_independent_and_valid(self):
        with (
            patch.object(collector, "ROOT", self.root),
            patch.object(collector, "cli_admission_cases", return_value=[]),
            patch.object(collector, "run_cli_capability_contracts", return_value=[]),
        ):
            collector.run_cli_admission(self.root / "cli.exe", self.root)
        directory = next(self.root.glob("cli-admission-*"))
        with zipfile.ZipFile(io.BytesIO((directory / "independent.zip").read_bytes())) as archive:
            self.assertIsNone(archive.testzip())
            self.assertEqual(archive.namelist(), ["item.txt"])
            self.assertEqual(archive.read("item.txt"), b"Independent ZIP admission fixture\n")
        cases = collector.cli_admission_cases(
            directory / "input.txt", directory / "output.zip", directory / "independent.zip"
        )
        readers = {
            arguments[0]
            for arguments, code, message in cases
            if message and "compatibility does not support FLZIP GPU" in message and code == 1
        }
        self.assertEqual(readers, {"extract", "verify"})

    # Purpose: Supply independent native capability observations without starting a GPU during offline contracts.
    # Inputs: Explicit fixture role. Outputs: A complete process observation with legal zero checksum.
    @staticmethod
    def capability_result(kind):
        hip = kind != "cpu"
        fields = {
            "hip_compiled": str(hip).lower(),
            "hip_runtime_loadable": str(hip).lower(),
            "hip_kernel_loadable": str(hip).lower(),
            "available": str(hip).lower(),
            "dependency_status": "ready" if hip else "cpu_only_build",
        }
        if kind == "diagnostic":
            del fields["hip_kernel_loadable"]
            del fields["dependency_status"]
            fields.update(
                {
                    "gcn_arch": "gfx1201",
                    "diagnostic_bytes": str(16 * 1024**2),
                    "diagnostic_kernel_launches": "2",
                    "diagnostic_kernel_ms": "1.25",
                    "diagnostic_h2d_bytes": str(16 * 1024**2),
                    "diagnostic_d2h_bytes": "1024",
                    "diagnostic_device_allocation_bytes": str(16 * 1024**2 + 1024),
                    "diagnostic_checksum": "0",
                    "diagnostic_wall_seconds": "1.1",
                }
            )
        return {
            "stdout": "\n".join(key + "=" + value for key, value in fields.items()),
            "stderr": "",
            "exit_code": 0 if hip else 10,
            "timed_out": False,
            "output_limit_exceeded": False,
            "output_truncated": False,
        }

    # Purpose: Keep offline CPU status and actual-HIP diagnostics distinct and bound every external command.
    # Inputs: Independent successful observations. Outputs: Exact argv, private directory, limits and raw evidence.
    def test_capability_contracts_require_explicit_mode_and_bounded_commands(self):
        for hip in (False, True):
            directory = self.root / str(hip)
            directory.mkdir()
            results = [self.capability_result("hip" if hip else "cpu")]
            if hip:
                results.append(self.capability_result("diagnostic"))
            with self.subTest(hip=hip), patch.object(collector, "process_owner") as owner:
                runner = owner.return_value.run_bounded_command
                runner.side_effect = results
                names = collector.run_cli_capability_contracts(self.root / "cli.exe", directory, hip)
                self.assertEqual(names, ["dependency-check.json"] + (["gpu-diagnostic.json"] if hip else []))
                expected = [[str(self.root / "cli.exe"), "dependency-check"]]
                if hip:
                    expected.append(
                        [
                            str(self.root / "cli.exe"),
                            "gpu-diagnostic",
                            "--seconds",
                            "1",
                            "--buffer-mib",
                            "16",
                            "--inner-iterations",
                            "1",
                        ]
                    )
                self.assertEqual([call.args[0] for call in runner.call_args_list], expected)
                for call, name, result in zip(runner.call_args_list, names, results, strict=True):
                    self.assertEqual(
                        call.kwargs,
                        {
                            "timeout_seconds": 30,
                            "max_output_bytes": 65536,
                            "response_tail_bytes": 65536,
                            "working_directory": directory,
                        },
                    )
                    self.assertEqual(json.loads((directory / name).read_text())["result"], result)

    # Purpose: Reject ambiguous, interrupted and contradictory process evidence while preserving its raw contents.
    # Inputs: Independent transport failures, duplicate output and CPU/HIP mode contradictions.
    # Outputs: No diagnostic runs after a bad dependency result; each error has an exclusive raw log.
    def test_capability_contracts_fail_closed_on_process_and_mode_errors(self):
        valid = self.capability_result("hip")
        mutations = [{"exit_code": code} for code in (True, "0", 1, None)]
        mutations += [{key: True} for key in ("timed_out", "output_limit_exceeded", "output_truncated")]
        mutations += [
            {"stdout": value} for value in (None, "", "malformed", "=value", valid["stdout"] + "\navailable=true")
        ]
        mutations += [
            {"stdout": valid["stdout"].replace(key + "=true", key + "=false")}
            for key in ("hip_compiled", "hip_runtime_loadable", "hip_kernel_loadable", "available")
        ]
        mutations.append(
            {"stdout": valid["stdout"].replace("dependency_status=ready", "dependency_status=cpu_only_build")}
        )
        for number, mutation in enumerate(mutations):
            directory = self.root / str(number)
            directory.mkdir()
            result = {**valid, **mutation}
            with self.subTest(mutation=mutation), patch.object(collector, "process_owner") as owner:
                runner = owner.return_value.run_bounded_command
                runner.return_value = result
                with self.assertRaises((ValueError, RuntimeError)):
                    collector.run_cli_capability_contracts(self.root / "cli.exe", directory, True)
                self.assertEqual(runner.call_count, 1)
                self.assertEqual(json.loads((directory / "dependency-check.json").read_text())["result"], result)

    # Purpose: Reject telemetry that claims GPU execution without coherent measured transfers, timing and work.
    # Inputs: One-field mutations of independently specified successful observations.
    # Outputs: Every malformed result fails, retains both reports, and cannot qualify the measurement.
    def test_gpu_diagnostic_telemetry_refuses_missing_nonfinite_or_impossible_values(self):
        valid = self.capability_result("diagnostic")
        fields = dict(line.split("=", 1) for line in valid["stdout"].splitlines())
        mutations = [(key, None) for key in fields if key.startswith("diagnostic_")]
        mutations += [
            ("gcn_arch", ""),
            ("hip_runtime_loadable", "false"),
            ("diagnostic_bytes", "1024"),
            ("diagnostic_kernel_launches", "1"),
            ("diagnostic_h2d_bytes", "1024"),
            ("diagnostic_device_allocation_bytes", "1024"),
            ("diagnostic_d2h_bytes", "0"),
            ("diagnostic_kernel_launches", "2.5"),
            ("diagnostic_checksum", "-1"),
            ("diagnostic_checksum", str(2**64)),
        ]
        mutations += [
            (key, value)
            for key in ("diagnostic_kernel_ms", "diagnostic_wall_seconds")
            for value in ("0", "-1", "nan", "inf", "bad")
        ]
        for number, (key, value) in enumerate(mutations):
            directory = self.root / str(number)
            directory.mkdir()
            changed = dict(fields)
            if value is None:
                del changed[key]
            else:
                changed[key] = value
            result = {**valid, "stdout": "\n".join(key + "=" + value for key, value in changed.items())}
            with self.subTest(key=key, value=value), patch.object(collector, "process_owner") as owner:
                owner.return_value.run_bounded_command.side_effect = [self.capability_result("hip"), result]
                with self.assertRaises(ValueError):
                    collector.run_cli_capability_contracts(self.root / "cli.exe", directory, True)
                self.assertEqual(json.loads((directory / "gpu-diagnostic.json").read_text())["result"], result)

    # Purpose: Keep CLI admission a required consumer of the actual measured executable.
    # Inputs: Passing earlier scenarios and a failed CLI contract. Outputs: Collection fails with the original cause.
    def test_cli_admission_failure_prevents_coverage_acceptance(self):
        with (
            patch.object(collector, "coverage_build"),
            patch.object(collector, "run_checked"),
            patch.object(collector, "run_cli_admission", side_effect=RuntimeError("CLI defect")),
            self.assertRaisesRegex(RuntimeError, "CLI defect"),
        ):
            collector.run_scenarios(self.root)

    # Purpose: Reject executable substitution even when its PDB and application receipt remain unchanged.
    # Inputs: A frozen test binary, matching symbols and a controlled mutation during the collector invocation.
    # Outputs: No accepted summary is published for the changed test executable.
    def test_collection_rejects_changed_test_executable(self):
        binary = self.root / "build/Debug"
        binary.mkdir(parents=True)
        for name in (
            "flitzzip_tests.exe",
            "flitzzip_tests.pdb",
            "flitzzip_cli.exe",
            "flitzzip_cli.pdb",
            "FlitzZip.exe",
            "FlitzZip.pdb",
        ):
            (binary / name).write_bytes(b"original native fixture")
        for name in (*collector.SCENARIO_INPUTS, *collector.GUI_SCENARIO_INPUTS):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"original scenario fixture")
        before = {
            "recipe": {"FLITZZIP_ENABLE_HIP": "OFF"},
            "inputs_sha256": "a" * 64,
            "receipt_sha256": "b" * 64,
            "receipt": {"inputs": {"files": {}}},
        }

        # Purpose: Model external substitution after the collector captures its initial evidence identity.
        # Inputs: Collector boundary and current subtest path. Outputs: A changed binary, symbols or scenario.
        def substitute(*_args):
            target.write_bytes(b"substituted fixture")

        with (
            patch.object(collector, "ROOT", self.root),
            patch.object(collector, "validate_current", return_value=before),
            patch.object(collector, "run_checked", side_effect=substitute),
            patch.object(collector, "verify_collector"),
            patch.object(collector, "normalize_report") as normalize,
        ):
            for include_gui, name in (
                (False, "build/Debug/flitzzip_tests.exe"),
                (False, "build/Debug/flitzzip_cli.exe"),
                (False, "build/Debug/flitzzip_cli.pdb"),
                (False, "tools/test_memory_benchmark_corpus.ps1"),
                (True, "build/Debug/FlitzZip.exe"),
                (True, "build/Debug/FlitzZip.pdb"),
                (True, "tools/gui_smoke.ps1"),
                (True, "tools/FlitzZip.GuiSmoke.Ui.psm1"),
                (True, "tools/process_environment.ps1"),
            ):
                target = self.root / name
                original = target.read_bytes()
                with self.subTest(name=name), self.assertRaisesRegex(ValueError, "changed during coverage"):
                    collector.collect(self.root / "collector.exe", {"files": {}}, self.root, include_gui=include_gui)
                target.write_bytes(original)
            normalize.assert_not_called()
        self.assertFalse((self.root / "summary.json").exists())

    # Purpose: Keep GUI execution explicit and retain its failures through the canonical child-process runner.
    # Inputs: CPU and HIP selections, an admitted receipt and an independently injected GUI failure.
    # Outputs: Only requested GUI work opens windows; all-page arguments and failure propagation remain required.
    def test_gui_scenario_is_explicit_and_fail_closed(self):
        with (
            patch.object(collector, "ROOT", self.root),
            patch.object(collector, "coverage_build"),
            patch.object(collector, "require_hip_device"),
            patch.object(collector, "run_cli_admission"),
        ):
            for hip_host in (False, True):
                with self.subTest(hip_host=hip_host), patch.object(collector, "run_checked") as run:
                    collector.run_scenarios(self.root, hip_host, False)
                    self.assertEqual(run.call_count, 6)
                    self.assertNotIn("gui-process.json", [call.args[2].name for call in run.call_args_list])
                with patch.object(
                    collector, "run_checked", side_effect=[None] * 6 + [RuntimeError("GUI failure")]
                ) as run:
                    with self.assertRaisesRegex(RuntimeError, "GUI failure"):
                        collector.run_scenarios(self.root, hip_host, True)
                    arguments, timeout, log = run.call_args.args
                    self.assertEqual(
                        arguments,
                        [
                            "powershell.exe",
                            "-NoProfile",
                            "-ExecutionPolicy",
                            "Bypass",
                            "-File",
                            str(self.root / "tools/gui_smoke.ps1"),
                            "-Configuration",
                            "Debug",
                            "-ScreenshotPath",
                            str(self.root / "gui.png"),
                        ],
                    )
                    self.assertEqual(log, self.root / "gui-process.json")
                    self.assertLessEqual(timeout, 900)
                    self.assertEqual(run.call_count, 7)

    # Purpose: Stop collection at every failed scenario without running later commands or rebuilding binaries.
    # Inputs: CPU Debug receipt and independent subprocess failures at each scenario boundary.
    # Outputs: The original failure propagates, only preceding scenarios run, and each has retained diagnostics.
    def test_scenario_failure_stops_collection(self):
        before = {"recipe": {"FLITZZIP_ENABLE_HIP": "OFF"}}
        expected = ("registry", "cli-help", "cli-version", "cli-plan", "cli-corpus", "cli-formats")
        with (
            patch.object(collector, "ROOT", self.root),
            patch.object(collector, "validate_current", return_value=before),
        ):
            for failure_at in range(len(expected)):
                outcomes = [None] * failure_at + [RuntimeError("scenario failure")]
                with patch.object(collector, "run_checked", side_effect=outcomes) as run:
                    with self.assertRaisesRegex(RuntimeError, "scenario failure"):
                        collector.run_scenarios(self.root)
                    self.assertEqual(run.call_count, failure_at + 1)
                    self.assertEqual(
                        [call.args[2].name for call in run.call_args_list],
                        [name + "-process.json" for name in expected[: failure_at + 1]],
                    )
                    for call in run.call_args_list[3:]:
                        self.assertEqual(call.args[0][0], "powershell.exe")
                        self.assertEqual(call.args[0][-2:], ["-Configuration", "Debug"])
                    if failure_at == 5:
                        self.assertEqual(run.call_args_list[-1].args[1], run.call_args_list[0].args[1])
                        self.assertGreater(run.call_args_list[-1].args[1], run.call_args_list[-2].args[1])
            before["recipe"]["FLITZZIP_ENABLE_HIP"] = "ON"
            with patch.object(collector, "run_checked") as run, self.assertRaisesRegex(ValueError, "HIP OFF"):
                collector.run_scenarios(self.root)
            run.assert_not_called()

    # Purpose: Keep CPU-only CI and actual HIP host collection distinct at the receipt and subprocess boundaries.
    # Inputs: Independently selected build modes and a deliberately conflicting receipt.
    # Outputs: Matching modes propagate; mismatches fail before any scenario launches.
    def test_host_modes_require_matching_build_and_real_gpu(self):
        for hip_host in (False, True):
            for configured in ("OFF", "ON"):
                before = {"recipe": {"FLITZZIP_ENABLE_HIP": configured}}
                with (
                    self.subTest(hip_host=hip_host, configured=configured),
                    patch.object(collector, "validate_current", return_value=before) as validate,
                    patch.object(collector, "require_hip_device") as require,
                    patch.object(collector, "run_checked") as run,
                    patch.object(collector, "run_cli_admission") as admission,
                ):
                    if hip_host != (configured == "ON"):
                        with self.assertRaisesRegex(ValueError, "selected measurement mode"):
                            collector.run_scenarios(self.root, hip_host)
                        run.assert_not_called()
                        require.assert_not_called()
                        admission.assert_not_called()
                    else:
                        collector.run_scenarios(self.root, hip_host)
                        self.assertEqual(run.call_count, 6)
                        self.assertEqual(require.call_count, int(hip_host))
                        admission.assert_called_once_with(
                            collector.ROOT / "build/Debug/flitzzip_cli.exe", self.root, hip_host=hip_host
                        )
                    validate.assert_called_once_with(collector.ROOT, "Debug", hip_host)

    # Purpose: Prevent unavailable, truncated or ambiguous device output from qualifying a HIP host run.
    # Inputs: Native capability fields and independent field, count, duplication and truncation failures.
    # Outputs: Each invalid report fails before the native registry; complete capability evidence is accepted.
    def test_hip_host_requires_complete_capability_evidence(self):
        fields = ("hip_compiled", "hip_runtime_loadable", "hip_kernel_loadable", "available")
        text = "\n".join([*(name + "=true" for name in fields), "device_count=1", "status=AMD HIP ready"])
        valid = {"stdout": text, "output_truncated": False}
        invalid = [
            {"stdout": text.replace(name + "=true", name + "=false"), "output_truncated": False} for name in fields
        ]
        invalid.extend(
            {"stdout": text.replace("device_count=1", "device_count=" + count), "output_truncated": False}
            for count in ("0", "-1", "", "unknown", "1.5")
        )
        invalid.extend(
            [
                {"stdout": text, "output_truncated": True},
                {"stdout": text + "\navailable=true", "output_truncated": False},
                {"stdout": text + "\nmalformed", "output_truncated": False},
            ]
        )
        for result in invalid:
            with (
                self.subTest(result=result),
                patch.object(collector, "run_checked", return_value=result),
                self.assertRaisesRegex(ValueError, "HIP host coverage (requires|contains duplicate fields)"),
            ):
                collector.require_hip_device(self.root)
        with patch.object(collector, "run_checked", return_value=valid) as run:
            collector.require_hip_device(self.root)
            self.assertEqual(run.call_args.args[0][-1], "gpu-info")
            self.assertEqual(run.call_args.args[2], self.root / "gpu-info-process.json")

    # Purpose: Never label debugger coverage of a HIP-enabled executable as measured device instructions.
    # Inputs: A source-bound host report and explicit HIP selection.
    # Outputs: The child uses HIP mode; summary retains host scope and leaves device measurement false.
    def test_hip_collection_preserves_host_only_scope(self):
        sources = [
            "src/core/archive_format.cpp",
            "src/core/path_safety.cpp",
            "src/zip/zip_adapter.cpp",
            "src/cli/main.cpp",
            "src/cli/memory_benchmark.cpp",
            "src/cli/memory_benchmark_source.cpp",
        ]
        before = {
            "inputs_sha256": "a" * 64,
            "receipt_sha256": "b" * 64,
            "receipt": {"inputs": {"files": sources + ["src/gpu/unmeasured.cpp"]}},
        }
        identities = {"build/Debug/flitzzip_tests.pdb": "c" * 64, "build/Debug/flitzzip_tests.exe": "d" * 64}
        report = {"files": sources, "lines_valid": 8, "lines_covered": 7, "uncovered_lines": {sources[0]: [1]}}
        with (
            patch.object(collector, "coverage_build", return_value=before) as build,
            patch.object(collector, "measurement_identity", return_value=identities),
            patch.object(collector, "run_checked") as run,
            patch.object(collector, "verify_collector"),
            patch.object(collector, "normalize_report", return_value=report),
            patch.object(collector, "digest", return_value="e" * 64),
        ):
            collector.collect(self.root / "collector.exe", {"files": {}, "version": "fixture"}, self.root, True)
        self.assertEqual([call.args for call in build.call_args_list], [(True,), (True,)])
        self.assertEqual(run.call_args.args[0][-2:], ["--run-scenarios", "--hip-host"])
        summary = json.loads((self.root / "summary.json").read_text())
        self.assertEqual(summary["scope"], "native-hip-host-first-party-lines")
        self.assertIs(summary["gpu_device_coverage"], False)
        self.assertEqual(summary["unmeasured_source_files"], ["src/gpu/unmeasured.cpp"])
        self.assertEqual(summary["uncovered_lines"], report["uncovered_lines"])

    # Purpose: Prevent a CLI-only report or omitted child option from being accepted as GUI coverage.
    # Inputs: Independent native/GUI source inventories and explicit GUI collection.
    # Outputs: Missing GUI consumers fail; accepted reports retain original hits, symbols and host-only scope.
    def test_gui_collection_requires_gui_source_evidence(self):
        native = [
            "src/core/archive_format.cpp",
            "src/core/path_safety.cpp",
            "src/zip/zip_adapter.cpp",
            "src/cli/main.cpp",
            "src/cli/memory_benchmark.cpp",
            "src/cli/memory_benchmark_source.cpp",
        ]
        gui = ["src/app/main.cpp", "src/app/main_window.cpp"]
        identities = {"build/Debug/flitzzip_tests.pdb": "c" * 64, "build/Debug/flitzzip_tests.exe": "d" * 64}
        before = {
            "inputs_sha256": "a" * 64,
            "receipt_sha256": "b" * 64,
            "receipt": {"inputs": {"files": native + gui + ["src/app/unmeasured.cpp"]}},
        }
        for measured_gui in (False, True):
            output = self.root / str(measured_gui)
            output.mkdir()
            report = {
                "files": native + (gui if measured_gui else []),
                "lines_valid": 12,
                "lines_covered": 9,
                "uncovered_lines": {"src/app/main_window.cpp": [42, 43, 44]},
            }
            with (
                self.subTest(measured_gui=measured_gui),
                patch.object(collector, "coverage_build", return_value=before),
                patch.object(collector, "measurement_identity", return_value=identities) as identity,
                patch.object(collector, "run_checked") as run,
                patch.object(collector, "verify_collector"),
                patch.object(collector, "normalize_report", return_value=report),
                patch.object(collector, "digest", return_value="e" * 64),
            ):
                if not measured_gui:
                    with self.assertRaisesRegex(ValueError, "missed required GUI consumers"):
                        collector.collect(self.root / "collector.exe", {"files": {}}, output, True, True)
                    self.assertFalse((output / "summary.json").exists())
                    continue
                collector.collect(self.root / "collector.exe", {"files": {}, "version": "fixture"}, output, True, True)
                self.assertEqual([call.args for call in identity.call_args_list], [(True,), (True,)])
                self.assertEqual(run.call_args.args[0][-3:], ["--run-scenarios", "--hip-host", "--gui"])
            summary = json.loads((output / "summary.json").read_text())
            self.assertIs(summary["gui_host_coverage"], True)
            self.assertIs(summary["gpu_device_coverage"], False)
            self.assertEqual(summary["measurement_inputs"], identities)
            self.assertEqual(summary["lines_covered"], 9)
            self.assertEqual(summary["lines_valid"], 12)
            self.assertEqual(summary["uncovered_lines"], report["uncovered_lines"])
            self.assertEqual(summary["unmeasured_source_files"], ["src/app/unmeasured.cpp"])


if __name__ == "__main__":
    unittest.main()
