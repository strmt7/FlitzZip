"""Regression tests for reviewed application-comparison evidence and charts."""

from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import render_comparison_graph as graph
from tools.render_svg_graph import parse_bounded_document
from tools.run_archive_comparison import CASES, EXPECTED, command_templates


# Purpose: Build a complete, synthetic schema-one record for validator tests.
# Inputs: None; all hashes and timings are non-production fixtures.
# Outputs: Returns three correctly paired ZIP/Zstandard cases.
def fixture() -> dict:
    files = [
        {
            "name": name,
            "bytes": size,
            "sha256": sha256,
            "download_url": f"https://sun.aei.polsl.pl/~sdeor/corpus/{name}.bz2",
            "download_sha256": "e" * 64,
        }
        for name, (size, sha256) in sorted(EXPECTED.items())
    ]
    cases = []
    for name, fmt, file_names, tools in CASES:
        size = sum(EXPECTED[file][0] for file in file_names)
        reference_sha = "b" * 64
        results = []
        for index, tool in enumerate(tools):
            create, extract = command_templates(tool, fmt, file_names)
            results.append(
                {
                    "tool": tool,
                    "create_argv": create,
                    "extract_argv": extract,
                    "archive_bytes": [size // (2 + index)] * 5,
                    "archive_sha256": [reference_sha] * 5,
                    "extract_reference_sha256": reference_sha,
                    "compress_seconds": [1.0 + index + step / 100 for step in range(5)],
                    "extract_seconds": [0.5 + index + step / 100 for step in range(5)],
                    "independent_verified": True,
                }
            )
        resource = {
            "cpu_load_percent": 10,
            "free_ram_bytes": 16 * 1024**3,
            "paging_pages_per_second": 0,
            "disk_busy_percent": 2,
            "gpu_engine_max_percent": 0.5,
            "workspace_bus_type": "NVMe",
        }
        cases.append(
            {
                "name": name,
                "format": fmt,
                "files": list(file_names),
                "input_bytes": size,
                "extract_reference_tool": tools[1],
                "extract_reference_bytes": results[1]["archive_bytes"][-1],
                "extract_reference_sha256": reference_sha,
                "resource_before": resource,
                "resource_after": resource,
                "results": results,
            }
        )
    return {
        "schema_version": 1,
        "benchmark_kind": "archive_application_comparison",
        "methodology": "docs/comparative-benchmark-methodology.md",
        "source_commit": "c" * 40,
        "source_dirty": False,
        "flitzzip_binary_sha256": "d" * 64,
        "build": {
            "configuration": "Release",
            "platform": "x64",
            "generator": "Visual Studio 17 2022",
            "hip_enabled": True,
            "hip_arch": "gfx1201",
            "cmake_cache_sha256": "a" * 64,
        },
        "host": {
            "os": "Windows",
            "os_name": "Windows 11",
            "os_build": "26200",
            "cpu": "Test CPU",
            "cpu_cores": 16,
            "cpu_threads": 32,
            "gpu": "Test GPU",
            "gpu_driver": "1.0",
            "ram_bytes": 32 * 1024**3,
            "ram_modules": 2,
            "ram_configured_mts": 5200,
            "storage_model": "Test NVMe",
            "storage_bus_type": "NVMe",
        },
        "tools": [
            {"name": name, "version": "test", "binary_sha256": "d" * 64, "source_url": "https://example.invalid/tool"}
            for name in ("FlitzZip", "7-Zip", "Zstd")
        ],
        "corpus": {"source_url": "https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia", "files": files},
        "settings": {"level": 5, "warmups_per_command": 1, "order": "alternating AB/BA", "cache": "warm", "runs": 5},
        "cases": cases,
    }


class ComparisonGraphTests(unittest.TestCase):
    # Purpose: Refuse missing or stale committed charts without silently creating or replacing them during --check.
    # Inputs: A valid comparison record with an absent output followed by an unrelated existing sentinel.
    # Outputs: The real entry point fails explicitly in both cases and preserves the destination's prior state.
    def test_check_preserves_destination_on_mismatch(self) -> None:
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temporary:
            source, output = Path(temporary) / "record.json", Path(temporary) / "graph.svg"
            source.write_text(json.dumps(fixture()), encoding="utf-8")
            command = [
                sys.executable,
                "-m",
                "tools.render_comparison_graph",
                "--input",
                str(source),
                "--output",
                str(output),
                "--check",
            ]
            for sentinel in (None, b"existing unrelated bytes"):
                if sentinel is not None:
                    output.write_bytes(sentinel)
                checked = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=30)
                self.assertEqual(checked.returncode, 1)
                self.assertIn("does not match", checked.stderr)
                self.assertNotIn("Traceback", checked.stderr)
                self.assertEqual(output.read_bytes() if output.exists() else None, sentinel)

    # Purpose: Refuse contradictory, mistyped or impossible evidence before it can become a published chart.
    # Inputs: One-field mutations of an otherwise admitted independent record, including Python's bool/int aliases.
    # Outputs: Every invalid record raises ValueError; the complete unmodified record remains admissible.
    def test_evidence_types_and_resource_bounds(self) -> None:
        invalid = [
            (("schema_version",), True),
            (("settings", "warmups_per_command"), True),
            (("settings", "warmups_per_command"), 1.0),
            (("build", "generator"), 7),
            (("build", "hip_arch"), ["gfx1201"]),
            (("host", "cpu"), 7),
            (("corpus", "files", 0, "bytes"), float(fixture()["corpus"]["files"][0]["bytes"])),
            (("cases", 0, "input_bytes"), float(fixture()["cases"][0]["input_bytes"])),
            (("cases", 0, "resource_before", "cpu_load_percent"), 101),
            (("cases", 0, "resource_after", "disk_busy_percent"), -1),
            (("cases", 0, "resource_before", "gpu_engine_max_percent"), float("nan")),
            (("cases", 0, "resource_after", "paging_pages_per_second"), float("inf")),
            (("cases", 0, "resource_before", "free_ram_bytes"), -1),
            (("cases", 0, "resource_before", "free_ram_bytes"), True),
            (("cases", 0, "resource_before", "free_ram_bytes"), fixture()["host"]["ram_bytes"] + 1),
            (("cases", 0, "resource_after", "workspace_bus_type"), ""),
            (("tools",), fixture()["tools"] + [copy.deepcopy(fixture()["tools"][0])]),
            (("tools",), fixture()["tools"] + [None]),
            (("cases", 0), None),
            (("cases", 0, "results", 0), None),
        ]
        graph.summarize(fixture())
        for path, value in invalid:
            record = fixture()
            target = record
            for key in path[:-1]:
                target = target[key]
            target[path[-1]] = value
            with self.subTest(path=path, value=value), self.assertRaises(ValueError):
                graph.summarize(record)

    # Purpose: Exercise every provenance and trial boundary with malformed evidence rather than favorable-only data.
    # Inputs: Independent one-field corruptions of source/build/tool/corpus/reference/sample contracts.
    # Outputs: Each corruption fails with the owning validator's actionable diagnostic.
    def test_incomplete_identity_and_trial_contracts(self) -> None:
        invalid = [
            (("source_commit",), "short", "source identity"),
            (("flitzzip_binary_sha256",), None, "binary SHA"),
            (("build",), None, "build identity"),
            (("methodology",), "other", "methodology"),
            (("host", "ram_modules"), 0, "numeric specification"),
            (("tools", 0, "binary_sha256"), "bad", "executable"),
            (("tools", 0, "version"), "", "provenance"),
            (("tools", 0, "binary_sha256"), "a" * 64, "binary identity differs"),
            (("corpus", "source_url"), "other", "corpus provenance"),
            (("corpus", "files"), [], "manifest"),
            (("corpus", "files", 0, "name"), "missing", "unknown corpus"),
            (("corpus", "files", 1), fixture()["corpus"]["files"][0], "duplicate"),
            (("corpus", "files", 0, "download_sha256"), None, "download provenance"),
            (("settings", "runs"), 4, "run count"),
            (("settings", "round_pause_ms"), True, "pause"),
            (("cases", 0, "name"), "other", "case identity"),
            (("cases", 0, "input_bytes"), 1, "input size"),
            (("cases", 0, "resource_before"), None, "resource context"),
            (("cases", 0, "results"), [], "tool pairing"),
            (("cases", 0, "extract_reference_bytes"), 0, "reference is missing"),
            (("cases", 0, "extract_reference_bytes"), 123, "reference differs"),
            (("cases", 0, "results", 0, "compress_seconds"), None, "individual runs"),
            (("cases", 0, "results", 0, "extract_seconds"), [1] * 6, "run count differs"),
            (("cases", 0, "results", 0, "archive_bytes"), [1, 2, 1, 1, 1], "archive size"),
            (("cases", 0, "results", 0, "archive_sha256"), ["bad"] * 5, "SHA-256 evidence"),
            (("cases", 0, "results", 0, "create_argv"), [None], "recorded command"),
        ]
        for path, value, diagnostic in invalid:
            record = fixture()
            target = record
            for key in path[:-1]:
                target = target[key]
            target[path[-1]] = value
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, diagnostic):
                graph.summarize(record)

    # Purpose: Preserve inclusive physical telemetry limits and precise command/token admission.
    # Inputs: Zero/full percentage and RAM bounds, a large paging rate, and malformed command vectors.
    # Outputs: Physical endpoints remain admissible; nonstring tokens and unequal command extents are refused.
    def test_resource_endpoints_and_command_tokens(self) -> None:
        for percentage, free in ((0, 0), (100, fixture()["host"]["ram_bytes"])):
            record = fixture()
            for case in record["cases"]:
                for field in ("resource_before", "resource_after"):
                    sample = case[field]
                    for key in ("cpu_load_percent", "disk_busy_percent", "gpu_engine_max_percent"):
                        sample[key] = percentage
                    sample["free_ram_bytes"] = free
                    sample["paging_pages_per_second"] = 1000.5
            graph.summarize(record)
        for command in (None, [], [None], ["wrong"]):
            with self.subTest(command=command):
                self.assertFalse(graph.matching_command(command, ["fixed"]))

    # Purpose: Keep derived statistics finite when individually finite samples overflow the even-count median.
    # Inputs: Six extreme, positive binary64 times whose midpoint addition exceeds binary64 capacity.
    # Outputs: Rejects the unrepresentable summary before it can produce invalid SVG coordinates.
    def test_unrepresentable_median_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "nonfinite"):
            graph.timing([1e308] * 6, "compression")

    # Purpose: Verify the published renderer CLI honors explicit effort instead of silently assuming five.
    # Inputs: A temporary reviewed-record fixture for each effort and its exact command vectors.
    # Outputs: All nine charts are generated and byte-checked through the same entry point used by CI.
    def test_cli_all_efforts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "record.json"
            output = Path(temporary) / "graph.svg"
            for level in range(1, 10):
                record = fixture()
                record["settings"]["level"] = level
                for case in record["cases"]:
                    for result in case["results"]:
                        result["create_argv"], result["extract_argv"] = command_templates(
                            result["tool"], case["format"], tuple(case["files"]), level
                        )
                source.write_text(json.dumps(record), encoding="utf-8")
                argv = ["render", "--input", str(source), "--output", str(output), "--level", str(level)]
                with self.subTest(level=level):
                    with patch.object(graph.sys, "argv", argv):
                        self.assertEqual(graph.main(), 0)
                    with patch.object(graph.sys, "argv", [*argv, "--check"]):
                        self.assertEqual(graph.main(), 0)

    # Purpose: Keep the new SHA-256 pins aligned with already reviewed corpus evidence.
    # Inputs: The committed level-five Silesia comparison record.
    # Outputs: Its corpus validates without changing historical result files.
    def test_reviewed_corpus_matches_sha256_pins(self) -> None:
        path = Path(__file__).resolve().parents[1] / "docs/benchmarks/data/fixtures/comparison-v1.json"
        record = json.loads(path.read_text(encoding="utf-8"))
        graph.summarize(record)

    # Purpose: Validate Windows and POSIX renderers against one recorded command contract.
    # Inputs: The Zstd output placeholder expressed with either path separator.
    # Outputs: Both spellings pass, while a different output filename fails.
    def test_placeholder_path_separators_are_portable(self) -> None:
        for slash in ("/", "\\"):
            record = fixture()
            extract = record["cases"][1]["results"][1]["extract_argv"]
            output_index = next(index for index, arg in enumerate(extract) if "{output}" in arg)
            extract[output_index] = f"{{output}}{slash}mozilla"
            with self.subTest(slash=slash):
                graph.summarize(record)
        record = fixture()
        extract = record["cases"][1]["results"][1]["extract_argv"]
        output_index = next(index for index, arg in enumerate(extract) if "{output}" in arg)
        extract[output_index] = "{output}/wrong-file"
        with self.assertRaisesRegex(ValueError, "recorded command"):
            graph.summarize(record)

    # Purpose: Ensure effort-specific records cannot be silently labeled as a different setting.
    # Inputs: All nine effort fixtures with exact commands and deliberately mismatched expectations.
    # Outputs: Each effort validates; a different label and malformed expectations are rejected.
    def test_explicit_effort_contract(self) -> None:
        for level in range(1, 10):
            record = fixture()
            record["settings"]["level"] = level
            for case in record["cases"]:
                for result in case["results"]:
                    result["create_argv"], result["extract_argv"] = command_templates(
                        result["tool"], case["format"], tuple(case["files"]), level
                    )
            with self.subTest(level=level):
                graph.summarize(record, expected_level=level)
                with self.assertRaisesRegex(ValueError, "settings differ"):
                    graph.summarize(record, expected_level=level % 9 + 1)
        for level in (0, 10, True, 5.0, "5", None):
            with self.subTest(level=level), self.assertRaisesRegex(ValueError, "unsupported comparison effort"):
                graph.summarize(fixture(), expected_level=level)
        for level in (True, 5.0, "5", None):
            record = fixture()
            record["settings"]["level"] = level
            with self.subTest(recorded_level=level), self.assertRaisesRegex(ValueError, "settings differ"):
                graph.summarize(record, expected_level=1 if level is True else 5)

    # Purpose: Prove exact sizes, medians, accessibility text, and byte-stable SVG output.
    # Inputs: One complete synthetic record.
    # Outputs: Three rows and one deterministic parsed SVG.
    def test_complete_record_renders(self) -> None:
        commit, rows = graph.summarize(fixture())
        self.assertEqual(len(rows), 3)
        self.assertEqual(rows[0]["input_bytes"], 51_770_558)
        self.assertAlmostEqual(rows[0]["metrics"][0]["compression"][0], 1.02)
        image = graph.render(commit, rows)
        self.assertEqual(image, graph.render(commit, rows))
        self.assertIn(b"?>\r\n<svg", image)
        self.assertNotIn(b"\n", image.replace(b"\r\n", b""))
        root = parse_bounded_document(image)
        self.assertEqual(root.attrib["role"], "img")
        self.assertIn(b"ranges are observed, not confidence intervals", image)

    # Purpose: Reject incomplete trials, failed decoding, and favorable-only case selection.
    # Inputs: Independently mutated sample, verification, and case fields.
    # Outputs: Each record fails before graph generation.
    def test_missing_or_failed_evidence_is_rejected(self) -> None:
        for mutation in ("short", "nan", "decode", "missing-case"):
            record = fixture()
            result = record["cases"][0]["results"][0]
            if mutation == "short":
                result["compress_seconds"].pop()
            elif mutation == "nan":
                result["extract_seconds"][0] = float("nan")
            elif mutation == "decode":
                result["independent_verified"] = False
            else:
                record["cases"].pop()
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                graph.summarize(record)

    # Purpose: Prevent corpus, command, binary, or host evidence from being altered silently.
    # Inputs: Wrong input hash, CLI switch, source dirtiness, and missing resource sample.
    # Outputs: All scientific identity violations are rejected.
    def test_identity_and_method_mismatch_is_rejected(self) -> None:
        for mutation in ("hash", "command", "dirty", "resource", "reference"):
            record = copy.deepcopy(fixture())
            if mutation == "hash":
                record["corpus"]["files"][0]["sha256"] = "bad"
            elif mutation == "command":
                record["cases"][0]["results"][0]["create_argv"].append("--faster")
            elif mutation == "dirty":
                record["source_dirty"] = True
            elif mutation == "reference":
                record["cases"][0]["results"][0]["extract_reference_sha256"] = "f" * 64
            else:
                record["cases"][0]["resource_before"].pop("free_ram_bytes")
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                graph.summarize(record)


if __name__ == "__main__":
    unittest.main()
