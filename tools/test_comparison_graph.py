"""Regression tests for reviewed application-comparison evidence and charts."""

from __future__ import annotations

import copy
import unittest
import xml.etree.ElementTree as ET

from tools import render_comparison_graph as graph
from tools.run_archive_comparison import CASES, EXPECTED, command_templates


# Purpose: Build a complete, synthetic schema-one record for validator tests.
# Inputs: None; all hashes and timings are non-production fixtures.
# Outputs: Returns three correctly paired ZIP/Zstandard cases.
def fixture() -> dict:
    files = [
        {
            "name": name,
            "bytes": size,
            "md5": md5,
            "sha256": "a" * 64,
            "download_url": f"https://sun.aei.polsl.pl/~sdeor/corpus/{name}.bz2",
            "download_sha256": "e" * 64,
        }
        for name, (size, md5) in sorted(EXPECTED.items())
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
        "superzip_binary_sha256": "d" * 64,
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
            for name in ("SuperZip", "7-Zip", "Zstd")
        ],
        "corpus": {"source_url": "https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia", "files": files},
        "settings": {"level": 5, "warmups_per_command": 1, "order": "alternating AB/BA", "cache": "warm", "runs": 5},
        "cases": cases,
    }


class ComparisonGraphTests(unittest.TestCase):
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
        root = ET.fromstring(image)
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
