"""Run a bounded, hash-checked Windows archive application comparison."""

from __future__ import annotations

import argparse
import bz2
import hashlib
import json
import math
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import UTC, datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = {
    "dickens": (10_192_446, "b24c37886142e11d0ee687db6ab06f936207aa7f2ea1fd1d9a36763c7a507e6a"),
    "mozilla": (51_220_480, "657fc3764b0c75ac9de9623125705831ebbfbe08fed248df73bc2dc66e2a963b"),
    "nci": (33_553_445, "fc63a31770947b8c2062d3b19ca94c00485a232bb91b502021948fee983e1635"),
    "ooffice": (6_152_192, "e7ee013880d34dd5208283d0d3d91b07f442e067454276095ded14f322a656eb"),
    "samba": (21_606_400, "93ba07bc44d8267789c1d911992f40b089ffa2140b4a160fac11ccae9a40e7b2"),
    "xml": (5_345_280, "0e82e54e695c1938e4193448022543845b33020c8be6bf3bf3ead2224903e08c"),
    "x-ray": (8_474_240, "7de9fce1405dc44ae5e6813ed21cd5751e761bd4265655a005d39b9685d1c9ad"),
}
CASES = (
    ("mixed-files", "zip", ("dickens", "ooffice", "samba", "xml", "x-ray"), ("SuperZip", "7-Zip")),
    ("mozilla", "zst", ("mozilla",), ("SuperZip", "Zstd")),
    ("nci", "zst", ("nci",), ("SuperZip", "Zstd")),
)
SOURCES = {
    "SuperZip": "https://github.com/strmt7/SuperZip",
    "7-Zip": "https://github.com/ip7z/7zip/releases/tag/26.03",
    "Zstd": "https://github.com/facebook/zstd/releases/tag/v1.5.7",
}


# Purpose: Hash a corpus file or binary without loading it all into memory.
# Inputs: A verified regular-file path and a hashlib algorithm name.
# Outputs: Returns a lowercase hexadecimal digest.
def digest(path: Path, algorithm: str = "sha256") -> str:
    value = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


# Purpose: Reject modified, missing, or symlinked Silesia inputs before timing.
# Inputs: Directories holding the seven raw files and their original downloads.
# Outputs: Returns a reproducible name/size/SHA-256/source manifest.
def verify_corpus(directory: Path, downloads: Path) -> list[dict]:
    if not directory.is_dir() or not downloads.is_dir():
        raise ValueError("corpus or download directory does not exist")
    manifest = []
    for name, (size, expected_sha256) in sorted(EXPECTED.items()):
        path = directory / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size != size:
            raise ValueError(f"corpus file missing, linked, or wrong size: {name}")
        raw_sha = digest(path)
        if raw_sha != expected_sha256:
            raise ValueError(f"pinned SHA-256 mismatch: {name}")
        downloaded = downloads / f"{name}.bz2"
        if downloaded.is_symlink() or not downloaded.is_file():
            raise ValueError(f"original corpus download is missing: {name}")
        decompressed = hashlib.sha256()
        decoded_bytes = 0
        with bz2.open(downloaded, "rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                decoded_bytes += len(chunk)
                if decoded_bytes > size:
                    raise ValueError(f"corpus download expands beyond expected size: {name}")
                decompressed.update(chunk)
        if decoded_bytes != size or decompressed.hexdigest() != raw_sha:
            raise ValueError(f"corpus download differs from raw file: {name}")
        manifest.append(
            {
                "name": name,
                "bytes": size,
                "sha256": raw_sha,
                "download_url": f"https://sun.aei.polsl.pl/~sdeor/corpus/{name}.bz2",
                "download_sha256": digest(downloaded),
            }
        )
    return manifest


# Purpose: Run one product command with bounded output and a monotonic wall timer.
# Inputs: Executable arguments, working directory, and optional timing flag.
# Outputs: Returns elapsed seconds; raises on timeout or nonzero exit.
def run(args: list[str], cwd: Path, timed: bool = True) -> float:
    started = time.perf_counter()
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True, errors="replace", timeout=120)
    elapsed = time.perf_counter() - started
    if result.returncode:
        details = (result.stderr + result.stdout)[-4000:]
        raise RuntimeError(f"command failed with code {result.returncode}: {args[0]}\n{details}")
    if not math.isfinite(elapsed) or elapsed <= 0:
        raise RuntimeError("invalid wall-clock interval")
    return elapsed if timed else 0.0


# Purpose: Construct explicit production commands without invoking a shell.
# Inputs: Product names, case format/files, archive path, output directory, and explicit effort.
# Outputs: Returns the create and extract argument vectors.
def commands(
    tool: str, fmt: str, files: tuple[str, ...], archive: Path, target: Path, binaries: dict, level: int = 5
) -> tuple[list[str], list[str]]:
    if type(level) is not int or not 1 <= level <= 9:
        raise ValueError(f"unsupported comparison effort: {level}")
    if tool == "SuperZip":
        create = [
            str(binaries[tool]),
            "compress",
            "--format",
            fmt,
            "--output",
            str(archive),
            "--compression-level",
            str(level),
            *files,
        ]
        extract = [str(binaries[tool]), "extract", "--format", fmt, "--output", str(target), str(archive)]
    elif tool == "7-Zip" and fmt == "zip":
        create = [
            str(binaries[tool]),
            "a",
            "-tzip",
            "-mm=Deflate",
            f"-mx={level}",
            "-mmt=on",
            "-y",
            str(archive),
            *files,
        ]
        extract = [str(binaries[tool]), "x", str(archive), f"-o{target}", "-y"]
    elif tool == "Zstd" and fmt == "zst" and len(files) == 1:
        create = [str(binaries[tool]), f"-{level}", "-q", "-f", "-o", str(archive), files[0]]
        extract = [str(binaries[tool]), "-d", "-q", "-f", "-o", str(target / files[0]), str(archive)]
    else:
        raise ValueError(f"unsupported tool/format pairing: {tool}/{fmt}")
    return create, extract


# Purpose: Preserve exact switches while omitting machine-specific absolute paths.
# Inputs: One supported tool/format case, its source filenames, and explicit effort.
# Outputs: Returns portable create/extract argument vectors with path placeholders.
def command_templates(tool: str, fmt: str, files: tuple[str, ...], level: int = 5) -> tuple[list[str], list[str]]:
    binaries = {name: Path(f"{{{name}}}") for name in SOURCES}
    create, extract = commands(tool, fmt, files, Path("{archive}"), Path("{output}"), binaries, level)
    return create, extract


# Purpose: Sample host and competing load outside every timed product command.
# Inputs: None; Windows CIM and GPU performance counters are queried read-only.
# Outputs: Returns hardware identity and a coarse CPU/GPU/RAM/disk activity snapshot.
def host_snapshot() -> dict:
    script = (
        "$cpu=Get-CimInstance Win32_Processor | Select-Object -First 1; "
        "$gpu=Get-CimInstance Win32_VideoController | Select-Object -First 1; "
        "$memory=Get-CimInstance Win32_PhysicalMemory; "
        "$cpuLoad=Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor | "
        "Where-Object Name -EQ '_Total' | Select-Object -First 1; "
        "$cs=Get-CimInstance Win32_ComputerSystem; "
        "$os=Get-CimInstance Win32_OperatingSystem; "
        "$paging=Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory | Select-Object -First 1; "
        "$disk=Get-CimInstance Win32_PerfFormattedData_PerfDisk_PhysicalDisk | "
        "Where-Object Name -EQ '_Total' | Select-Object -First 1; "
        f"$volumeDisk=Get-Partition -DriveLetter '{ROOT.drive[0]}' | Get-Disk | Select-Object -First 1; "
        "$engines=Get-Counter '\\GPU Engine(*)\\Utilization Percentage' -MaxSamples 1 "
        "-ErrorAction SilentlyContinue; "
        "$gpuMax=if($engines){($engines.CounterSamples | Measure-Object CookedValue -Maximum).Maximum}else{$null}; "
        "[pscustomobject]@{cpu_model=$cpu.Name.Trim();"
        "cpu_cores=[int]$cpu.NumberOfCores;cpu_threads=[int]$cpu.NumberOfLogicalProcessors;"
        "gpu_model=$gpu.Name;gpu_driver=$gpu.DriverVersion;"
        "os_name=$os.Caption;os_build=$os.BuildNumber;"
        "ram_bytes=[int64]$cs.TotalPhysicalMemory;"
        "ram_modules=@($memory).Count;"
        "ram_configured_mts=[int](($memory | Measure-Object ConfiguredClockSpeed -Minimum).Minimum);"
        "storage_model=$volumeDisk.FriendlyName;"
        "cpu_load_percent=$cpuLoad.PercentProcessorTime;"
        "free_ram_bytes=[int64]$os.FreePhysicalMemory*1024;"
        "paging_pages_per_second=$paging.PagesPerSec;"
        "disk_busy_percent=$disk.PercentDiskTime;gpu_engine_max_percent=$gpuMax;"
        "workspace_bus_type=[string]$volumeDisk.BusType}"
        " | ConvertTo-Json -Compress"
    )
    completed = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", script],
        capture_output=True,
        text=True,
        errors="replace",
        timeout=30,
    )
    if completed.returncode:
        raise RuntimeError(f"host resource sampling failed: {completed.stderr[-1000:]}")
    sample = json.loads(completed.stdout)
    required = (
        "cpu_model",
        "cpu_cores",
        "cpu_threads",
        "gpu_model",
        "gpu_driver",
        "os_name",
        "os_build",
        "ram_bytes",
        "ram_modules",
        "ram_configured_mts",
        "storage_model",
        "cpu_load_percent",
        "free_ram_bytes",
        "paging_pages_per_second",
        "disk_busy_percent",
        "gpu_engine_max_percent",
        "workspace_bus_type",
    )
    if any(sample.get(key) is None for key in required):
        raise RuntimeError(f"host resource sampling missing required fields: {sample}")
    return sample


# Purpose: Identify the x64 Release/HIP configuration that produced the tested executable.
# Inputs: SuperZip executable path under the repository's CMake build tree.
# Outputs: Returns build settings or raises when release identity cannot be proved.
def build_identity(binary: Path) -> dict:
    if binary != (ROOT / "build" / "Release" / "superzip_cli.exe").resolve():
        raise ValueError("comparison requires the repository x64 Release SuperZip binary")
    cache = ROOT / "build" / "CMakeCache.txt"
    if not cache.is_file():
        raise ValueError("CMake build cache is missing")
    values = {}
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if ":" in line and "=" in line:
            key_type, value = line.split("=", 1)
            key, _ = key_type.split(":", 1)
            values[key] = value
    if values.get("CMAKE_GENERATOR_PLATFORM", "").lower() != "x64" or values.get("SUPERZIP_ENABLE_HIP") != "ON":
        raise ValueError("comparison requires a HIP-enabled x64 Release build")
    return {
        "configuration": "Release",
        "platform": values["CMAKE_GENERATOR_PLATFORM"],
        "generator": values.get("CMAKE_GENERATOR"),
        "hip_enabled": True,
        "hip_arch": values.get("SUPERZIP_HIP_ARCH"),
        "cmake_cache_sha256": digest(cache),
    }


# Purpose: Prove that an extracted tree exactly matches the case manifest.
# Inputs: Output directory, expected case filenames, and source SHA-256 lookup.
# Outputs: Raises if any path, size, or content differs.
def verify_tree(target: Path, files: tuple[str, ...], manifest: dict[str, dict]) -> None:
    actual = {str(path.relative_to(target)).replace("\\", "/") for path in target.rglob("*") if path.is_file()}
    if actual != set(files):
        raise RuntimeError(f"extracted paths differ: expected {files}, got {sorted(actual)}")
    for name in files:
        path = target / name
        if path.is_symlink() or path.stat().st_size != manifest[name]["bytes"]:
            raise RuntimeError(f"extracted size or type differs: {name}")
        if digest(path) != manifest[name]["sha256"]:
            raise RuntimeError(f"extracted SHA-256 differs: {name}")


# Purpose: Safely clear only benchmark-owned per-run output after verification.
# Inputs: Path created below the unique temporary case directory and its root.
# Outputs: Removes that path or raises if containment is not proven.
def discard(path: Path, owned_root: Path) -> None:
    if not path.resolve().is_relative_to(owned_root.resolve()):
        raise RuntimeError("refusing to remove a path outside the benchmark-owned directory")
    if path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


# Purpose: Time one alternating, independently decoded application case.
# Inputs: Case contract, binaries, source directory, run count, hashes, effort, and inter-round pause.
# Outputs: Returns all size, timing, and round-trip evidence for one case.
def measure_case(
    case: tuple,
    binaries: dict,
    corpus: Path,
    runs: int,
    manifest: dict,
    work_root: Path,
    level: int = 5,
    round_pause_ms: int = 250,
) -> dict:
    name, fmt, files, tools = case
    input_bytes = sum(manifest[file]["bytes"] for file in files)
    if input_bytes > 64 * 1024 * 1024:
        raise ValueError("filesystem case exceeds 64 MiB")
    results = {
        tool: {
            "tool": tool,
            "archive_bytes": [],
            "archive_sha256": [],
            "compress_seconds": [],
            "extract_seconds": [],
            "independent_verified": False,
        }
        for tool in tools
    }
    for tool in tools:
        create, extract = command_templates(tool, fmt, files, level)
        results[tool]["create_argv"] = create
        results[tool]["extract_argv"] = extract
    with tempfile.TemporaryDirectory(prefix=f"comparison-{name}-", dir=work_root) as temporary:
        owned = Path(temporary).resolve()
        exemplars = {}
        for iteration in range(-1, runs):
            order = tools if iteration % 2 == 0 else tuple(reversed(tools))
            for tool in order:
                round_dir = owned / f"create-{iteration}-{tool}"
                round_dir.mkdir()
                archive = round_dir / ("case.zip" if fmt == "zip" else f"{files[0]}.zst")
                create, _ = commands(tool, fmt, files, archive, round_dir / "unused", binaries, level)
                elapsed = run(create, corpus, timed=iteration >= 0)
                if not archive.is_file() or archive.stat().st_size <= 0:
                    raise RuntimeError(f"missing archive: {name}/{tool}")
                other = next(candidate for candidate in tools if candidate != tool)
                independent_dir = round_dir / "independent"
                independent_dir.mkdir()
                _, independent_extract = commands(other, fmt, files, archive, independent_dir, binaries, level)
                run(independent_extract, corpus, timed=False)
                verify_tree(independent_dir, files, manifest)
                discard(independent_dir, owned)
                if iteration >= 0:
                    results[tool]["compress_seconds"].append(elapsed)
                    results[tool]["archive_bytes"].append(archive.stat().st_size)
                    results[tool]["archive_sha256"].append(digest(archive))
                    results[tool]["independent_verified"] = True
                if iteration == runs - 1:
                    exemplars[tool] = archive
                else:
                    discard(round_dir, owned)
            if iteration < runs - 1:
                time.sleep(round_pause_ms / 1000)
        reference_tool = tools[1]
        reference_archive = exemplars[reference_tool]
        reference_sha = digest(reference_archive)
        reference_bytes = reference_archive.stat().st_size
        for iteration in range(-1, runs):
            order = tools if iteration % 2 == 0 else tuple(reversed(tools))
            for tool in order:
                target = owned / f"extract-{iteration}-{tool}"
                target.mkdir()
                _, extract = commands(tool, fmt, files, reference_archive, target, binaries, level)
                elapsed = run(extract, corpus, timed=iteration >= 0)
                verify_tree(target, files, manifest)
                if iteration >= 0:
                    results[tool]["extract_seconds"].append(elapsed)
                discard(target, owned)
            if iteration < runs - 1:
                time.sleep(round_pause_ms / 1000)
    for tool in tools:
        results[tool]["extract_reference_sha256"] = reference_sha
    return {
        "name": name,
        "format": fmt,
        "files": list(files),
        "input_bytes": input_bytes,
        "extract_reference_tool": reference_tool,
        "extract_reference_bytes": reference_bytes,
        "extract_reference_sha256": reference_sha,
        "results": [results[tool] for tool in tools],
    }


# Purpose: Require source/binary identity without treating unrelated local edits as product changes.
# Inputs: Git checkout root and tested SuperZip executable.
# Outputs: Returns clean source commit, executable SHA-256, and unrelated dirty path names.
def source_identity(binary: Path) -> tuple[str, str, list[str]]:
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    status = subprocess.check_output(
        ["git", "status", "--porcelain", "--untracked-files=all"], cwd=ROOT, text=True
    ).splitlines()
    relevant = (
        "src/",
        "include/",
        "CMakeLists.txt",
        "cmake/",
        ".github/workflows/benchmark-graph.yml",
        "tools/run_archive_comparison.py",
        "tools/render_comparison_graph.py",
        "tools/render_tradeoff_graph.py",
        "docs/comparative-benchmark-methodology.md",
    )
    dirty = [line[3:].replace("\\", "/") for line in status]
    if any(path.startswith(relevant) for path in dirty):
        raise RuntimeError("product or comparison method is dirty; commit and rebuild before publication")
    return commit, digest(binary), dirty


# Purpose: Parse bounded benchmark inputs and write a reviewable raw JSON record.
# Inputs: Explicit corpus, official CLI binaries, run count, and new ignored output path.
# Outputs: Writes one result file only after all three cases pass independent round trips.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--downloads", type=Path, required=True)
    parser.add_argument("--superzip", type=Path, required=True)
    parser.add_argument("--sevenzip", type=Path, required=True)
    parser.add_argument("--zstd", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--level", type=int, choices=range(1, 10), default=5)
    parser.add_argument("--round-pause-ms", type=int, default=250)
    args = parser.parse_args()
    if args.runs < 5 or args.runs > 10:
        parser.error("publication requires 5-10 timed runs per direction")
    if not 0 <= args.round_pause_ms <= 1000:
        parser.error("round pause must be between 0 and 1000 ms")
    corpus = args.corpus.resolve(strict=True)
    binaries = {
        "SuperZip": args.superzip.resolve(strict=True),
        "7-Zip": args.sevenzip.resolve(strict=True),
        "Zstd": args.zstd.resolve(strict=True),
    }
    output = args.output.resolve()
    benchmark_root = (ROOT / "out" / "benchmarks").resolve()
    if not output.is_relative_to(benchmark_root) or output.exists():
        parser.error("output must be a new file beneath repository out/benchmarks")
    output.parent.mkdir(parents=True, exist_ok=True)
    files = verify_corpus(corpus, args.downloads.resolve(strict=True))
    manifest = {entry["name"]: entry for entry in files}
    commit, binary_hash, unrelated_dirty = source_identity(binaries["SuperZip"])
    build = build_identity(binaries["SuperZip"])
    tool_data = []
    for name, path in binaries.items():
        version_args = [str(path), "--version"] if name != "7-Zip" else [str(path), "i"]
        version_output = subprocess.run(version_args, capture_output=True, text=True, errors="replace", timeout=20)
        if version_output.returncode:
            raise RuntimeError(f"could not read version of {name}")
        version = next(
            (line.strip() for line in (version_output.stdout + version_output.stderr).splitlines() if line.strip()),
            "unknown",
        )
        tool_data.append(
            {"name": name, "version": version[:160], "binary_sha256": digest(path), "source_url": SOURCES[name]}
        )
    host_before = host_snapshot()
    cases = []
    for case in CASES:
        print(f"measuring {case[0]} ({case[1]}), level {args.level}", flush=True)
        resource_before = host_snapshot()
        measured = measure_case(
            case, binaries, corpus, args.runs, manifest, output.parent, args.level, args.round_pause_ms
        )
        measured["resource_before"] = resource_before
        measured["resource_after"] = host_snapshot()
        cases.append(measured)
        for result in measured["results"]:
            print(
                f"  {result['tool']}: {statistics.median(result['compress_seconds']):.3f}s create, "
                f"{statistics.median(result['extract_seconds']):.3f}s extract, "
                f"{result['archive_bytes'][0]:,} bytes",
                flush=True,
            )
    record = {
        "schema_version": 1,
        "benchmark_kind": "archive_application_comparison",
        "methodology": "docs/comparative-benchmark-methodology.md",
        "measured_at_utc": datetime.now(UTC).isoformat(),
        "source_commit": commit,
        "source_dirty": False,
        "superzip_binary_sha256": binary_hash,
        "unrelated_dirty_count": len(unrelated_dirty),
        "host": {
            "os": platform.platform(),
            "os_name": host_before["os_name"],
            "os_build": host_before["os_build"],
            "cpu": host_before["cpu_model"],
            "cpu_cores": host_before["cpu_cores"],
            "cpu_threads": host_before["cpu_threads"],
            "gpu": host_before["gpu_model"],
            "gpu_driver": host_before["gpu_driver"],
            "ram_bytes": host_before["ram_bytes"],
            "ram_modules": host_before["ram_modules"],
            "ram_configured_mts": host_before["ram_configured_mts"],
            "storage_model": host_before["storage_model"],
            "workspace_volume": ROOT.drive,
            "storage_bus_type": host_before["workspace_bus_type"],
            "resource_before": host_before,
            "resource_after": host_snapshot(),
        },
        "build": build,
        "corpus": {
            "name": "Silesia selected files",
            "source_url": "https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia",
            "files": files,
        },
        "tools": tool_data,
        "settings": {
            "level": args.level,
            "cache": "warm",
            "warmups_per_command": 1,
            "order": "alternating AB/BA",
            "timer": "Python perf_counter around subprocess",
            "runs": args.runs,
            "round_pause_ms": args.round_pause_ms,
        },
        "cases": cases,
    }
    output.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"saved {output}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"comparison failed: {error}", file=sys.stderr)
        sys.exit(1)
