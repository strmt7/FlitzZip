"""Linux command-contract tests for the production ClusterFuzzLite build script."""

import json
import os
import subprocess
import tempfile
import unittest
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
    # Inputs: Temporary output root, sanitizer and optional failing source. Outputs: Process result and complete argv.
    def run_plan(self, directory: Path, sanitizer: str, fail_source: str = "") -> tuple:
        compiler = directory / "compiler"
        compiler.write_text(COMPILER, encoding="utf-8")
        compiler.chmod(0o700)
        log = directory / "commands.jsonl"
        (directory / "output").mkdir()
        environment = os.environ | {
            "CC": str(compiler),
            "CXX": str(compiler),
            "CFLAGS": f"-fsanitize={sanitizer}",
            "CXXFLAGS": f"-fsanitize={sanitizer}",
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

    # Purpose: Preserve all target links and exact sanitizer/include contexts while removing repeated compilation.
    # Inputs: Address and undefined command plans, not real binaries. Outputs: Eight shared compilations and 14 links.
    def test_shared_objects_and_target_contracts(self):
        for sanitizer in ("address", "undefined"):
            with self.subTest(sanitizer=sanitizer), tempfile.TemporaryDirectory(prefix="fuzz build ") as temporary:
                result, commands = self.run_plan(Path(temporary), sanitizer)
                self.assertEqual(result.returncode, 0, result.stderr)
                compiled = [args for args in commands if "-c" in args]
                for name in CORE:
                    instances = [args for args in compiled if f"src/core/{name}.cpp" in args]
                    self.assertEqual(len(instances), 2, name)
                    self.assertEqual(sum("-Ithird_party/miniz" in args for args in instances), 1, name)
                links = {Path(args[args.index("-o") + 1]).name: args for args in commands if "-c" not in args}
                self.assertEqual(set(links), {f"superzip_{name}_fuzzer" for name in TARGETS})
                for name, expected_core in TARGETS.items():
                    args = links[f"superzip_{name}_fuzzer"]
                    self.assertIn("-fsanitize=fuzzer", args)
                    objects = [Path(arg) for arg in args if Path(arg).stem in CORE and arg.endswith(".o")]
                    self.assertEqual({obj.stem for obj in objects}, set(expected_core), name)
                    self.assertEqual(len(objects), len(expected_core), name)
                    expected_dir = "miniz-core-objects" if name in ("cpio", "xar") else "core-objects"
                    self.assertTrue(all(obj.parent.name == expected_dir for obj in objects), name)
                for args in commands:
                    self.assertIn(f"-fsanitize={sanitizer}", args)
                    if any(arg.endswith(".cpp") for arg in args):
                        self.assertIn("-std=c++20", args)
                        self.assertIn("-DSUPERZIP_ENABLE_HIP=0", args)
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


if __name__ == "__main__":
    unittest.main()
