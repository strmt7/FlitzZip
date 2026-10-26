"""Linux command-contract tests for the production ClusterFuzzLite build script."""

import json
import os
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE = ("path_safety", "file_manifest", "file_publish", "progress")
TARGETS = {
    "archive_index": (),
    "path_safety": ("path_safety",),
    "iso": CORE,
    "cab_header": ("path_safety",),
    "rpm_header": (),
    "sevenzip": CORE,
    "lzma": CORE,
    "lzip": CORE,
    "arj": CORE,
    "arc": CORE,
    "macbinary": CORE,
    "lha": CORE,
    "cpio": CORE,
    "ar": CORE,
    "xar": CORE,
}
COMPILER = """#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
with Path(os.environ['COMMAND_LOG']).open('a', encoding='utf-8') as stream:
    stream.write(json.dumps(args) + '\\n')
if os.environ.get('FAIL_SOURCE') in args:
    sys.exit(7)
for source in args:
    if source.endswith(('.cpp', '.c')) and not Path(source).is_file():
        sys.exit(8)
output = Path(args[args.index('-o') + 1])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_bytes(b'command-contract fixture, not a compiled fuzzer')
output.chmod(0o700)
"""


class FuzzBuildTests(unittest.TestCase):
    # Purpose: Execute the unchanged production script with a noncompiling compiler fixture.
    # Inputs: Temporary output root, sanitizer, optional failing source and whitespace-separated extra flags.
    # Outputs: Process result and complete compiler argv, without compiling or executing product code.
    def run_plan(self, directory: Path, sanitizer: str, fail_source: str = "", extra_flags: str = "") -> tuple:
        compiler = directory / "compiler"
        compiler.write_text(COMPILER, encoding="utf-8")
        compiler.chmod(0o700)
        log = directory / "commands.jsonl"
        (directory / "output").mkdir()
        environment = os.environ | {
            "CC": str(compiler),
            "CXX": str(compiler),
            "CFLAGS": f"-fsanitize={sanitizer} {extra_flags}",
            "CXXFLAGS": f"-fsanitize={sanitizer} {extra_flags}",
            "LIB_FUZZING_ENGINE": "-fsanitize=fuzzer",
            "OUT": str(directory / "output"),
            "SANITIZER": sanitizer,
            "COMMAND_LOG": str(log),
            "FAIL_SOURCE": fail_source,
        }
        result = subprocess.run(
            ["bash", ".clusterfuzzlite/build.sh"],
            cwd=ROOT,
            env=environment,
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )
        commands = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()] if log.is_file() else []
        return result, commands

    # Purpose: Preserve compiler argument boundaries while refusing filesystem-dependent wildcard expansion.
    # Inputs: A matching real file, literal wildcard, tabs/newlines and quote characters in framework flags.
    # Outputs: Every actual compiler invocation receives the independently expected tokens without shell expansion.
    def test_flag_arrays_preserve_literals_and_whitespace_boundaries(self):
        with tempfile.TemporaryDirectory(prefix="fuzz-flags-") as temporary:
            directory = Path(temporary)
            matched = directory / "would-expand.flag"
            matched.write_text("independent glob match", encoding="utf-8")
            wildcard = str(directory / "*.flag")
            result, commands = self.run_plan(directory, "address", extra_flags=f"\t{wildcard}\n-DQUOTE='literal'\t")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertGreater(len(commands), 15)
            for command in commands:
                self.assertIn(wildcard, command)
                self.assertNotIn(str(matched), command)
                self.assertIn("-DQUOTE='literal'", command)
                self.assertIn("-fsanitize=address", command)

    # Purpose: Preserve all target links and exact sanitizer/include contexts while removing repeated compilation.
    # Inputs: Address and undefined command plans, not real binaries. Outputs: Shared builds and complete target links.
    def test_shared_objects_and_target_contracts(self):
        for sanitizer in ("address", "undefined"):
            with self.subTest(sanitizer=sanitizer), tempfile.TemporaryDirectory(prefix="fuzz build ") as temporary:
                result, commands = self.run_plan(Path(temporary), sanitizer)
                self.assertEqual(result.returncode, 0, result.stderr)
                compiled = [args for args in commands if "-c" in args]
                wire = [args for args in compiled if "third_party/lzma_sdk/C/CpuArchByteAccess.cpp" in args]
                self.assertEqual(len(wire), 1, "The shared SDK wire implementation must be compiled once.")
                for name in CORE:
                    instances = [args for args in compiled if f"src/core/{name}.cpp" in args]
                    self.assertEqual(len(instances), 2, name)
                    self.assertEqual(sum("-Ithird_party/miniz" in args for args in instances), 1, name)
                links = {Path(args[args.index("-o") + 1]).name: args for args in commands if "-c" not in args}
                self.assertEqual(set(links), {f"flitzzip_{name}_fuzzer" for name in TARGETS})
                with zipfile.ZipFile(Path(temporary) / "output/flitzzip_ar_fuzzer_seed_corpus.zip") as seeds:
                    self.assertEqual(set(seeds.namelist()), {"empty.ar", "ordinary.ar", "gnu.ar", "bsd.ar"})
                    self.assertEqual(seeds.read("empty.ar"), b"!<arch>\n")
                    for name in ("ordinary.ar", "gnu.ar", "bsd.ar"):
                        data = seeds.read(name)
                        self.assertEqual(data[:8], b"!<arch>\n")
                        offset = 8
                        members = 0
                        while offset < len(data):
                            self.assertEqual(data[offset + 58 : offset + 60], b"`\n")
                            size = int(data[offset + 48 : offset + 58])
                            offset += 60 + size + size % 2
                            members += 1
                        self.assertEqual(offset, len(data))
                        self.assertEqual(members, 2)
                for name, expected_core in TARGETS.items():
                    args = links[f"flitzzip_{name}_fuzzer"]
                    self.assertIn("-fsanitize=fuzzer", args)
                    objects = [Path(arg) for arg in args if Path(arg).stem in CORE and arg.endswith(".o")]
                    self.assertEqual({obj.stem for obj in objects}, set(expected_core), name)
                    self.assertEqual(len(objects), len(expected_core), name)
                    wire_objects = [arg for arg in args if Path(arg).name == "CpuArchByteAccess.o"]
                    self.assertEqual(len(wire_objects), int(name in ("sevenzip", "lzma", "lzip")), name)
                    expected_dir = "miniz-core-objects" if name in ("cpio", "xar") else "core-objects"
                    self.assertTrue(all(obj.parent.name == expected_dir for obj in objects), name)
                for args in commands:
                    self.assertIn(f"-fsanitize={sanitizer}", args)
                    if any(arg.endswith(".cpp") for arg in args):
                        self.assertIn("-std=c++20", args)
                        self.assertIn("-DFLITZZIP_ENABLE_HIP=0", args)
                        self.assertIn("-DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION=1", args)
                    if sanitizer == "undefined":
                        self.assertIn("-fsanitize=alignment", args)
                        self.assertIn("-fno-sanitize-recover=alignment", args)

    # Purpose: Stop on a failed shared compilation rather than linking incomplete or cached targets.
    # Inputs: One injected compiler failure in a shared C++ source. Outputs: Nonzero status and no target links.
    def test_compiler_failure_stops_links(self):
        with tempfile.TemporaryDirectory(prefix="fuzz build ") as temporary:
            result, commands = self.run_plan(Path(temporary), "address", "src/core/file_publish.cpp")
            self.assertEqual(result.returncode, 7, result.stderr)
            self.assertTrue(all("-c" in args for args in commands))

    # Purpose: Keep local fuzz startup aligned with hosted dictionary loading and fail closed on missing inputs.
    # Inputs: The actual smoke dispatch and owned executable fixtures. Outputs: Exact argv and failure propagation.
    def test_smoke_loads_every_dictionary(self):
        source = (ROOT / ".clusterfuzzlite/local_smoke.sh").read_text(encoding="utf-8")
        dispatch = "set -euo pipefail\nruns=1\n" + source[source.index("run_fuzzer() {\n") :]
        fixture = """#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
with Path(os.environ['COMMAND_LOG']).open('a', encoding='utf-8') as stream:
    stream.write(json.dumps([Path(sys.argv[0]).name, args]) + '\\n')
dictionary = [arg.removeprefix('-dict=') for arg in args if arg.startswith('-dict=')]
sys.exit(0 if len(dictionary) == 1 and Path(dictionary[0]).is_file() else 17)
"""
        with tempfile.TemporaryDirectory(prefix="fuzz dispatch ") as temporary:
            directory = Path(temporary)
            log = directory / "calls.jsonl"
            for name in TARGETS:
                target = directory / f"flitzzip_{name}_fuzzer"
                target.write_text(fixture, encoding="utf-8")
                target.chmod(0o700)
                target.with_suffix(".dict").write_text('entry="valid"\n', encoding="utf-8")
            environment = os.environ | {"OUT": str(directory), "COMMAND_LOG": str(log)}
            result = subprocess.run(
                ["bash", "-c", dispatch], env=environment, capture_output=True, text=True, timeout=30, check=False
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            calls = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]
            self.assertEqual({name for name, _ in calls}, {f"flitzzip_{name}_fuzzer" for name in TARGETS})
            self.assertEqual(len(calls), len(TARGETS))
            for name, args in calls:
                self.assertIn(f"-dict={directory / (name + '.dict')}", args)
                self.assertIn("-runs=1", args)
                self.assertIn("-max_len=4096" if name == "flitzzip_path_safety_fuzzer" else "-max_len=1048576", args)
            (directory / "flitzzip_ar_fuzzer.dict").unlink()
            failed = subprocess.run(
                ["bash", "-c", dispatch], env=environment, capture_output=True, text=True, timeout=30, check=False
            )
            self.assertEqual(failed.returncode, 17, failed.stderr)

    # Purpose: Reject an incomplete SDK bridge before any SDK consumer is linked.
    # Inputs: Injected wire implementation compile failure. Outputs: No sevenzip, LZMA or lzip target links.
    def test_sdk_wire_failure_stops_consumers(self):
        with tempfile.TemporaryDirectory(prefix="fuzz build ") as temporary:
            result, commands = self.run_plan(Path(temporary), "address", "third_party/lzma_sdk/C/CpuArchByteAccess.cpp")
            self.assertEqual(result.returncode, 7, result.stderr)
            links = {Path(args[args.index("-o") + 1]).name for args in commands if "-c" not in args}
            self.assertTrue(
                links.isdisjoint({"flitzzip_sevenzip_fuzzer", "flitzzip_lzma_fuzzer", "flitzzip_lzip_fuzzer"})
            )


if __name__ == "__main__":
    unittest.main()
