"""Feed reviewed public corpora from RAM into Neutron; Hyperfine owns repetition and timing."""

from __future__ import annotations

import argparse
import ctypes
import gzip
import hashlib
import importlib.util
import io
import json
import math
import os
import shlex
import stat
import subprocess
import sys
import tarfile
import time
import zipfile
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
from pathlib import Path, PurePosixPath
from urllib.request import Request, build_opener

from tools import neutron_corpus_ipc as ipc
from tools.benchmark_comparators import NoReleaseRedirects, require_permission
from tools.native_build_provenance import input_path
from tools.native_build_receipt import validate_current

ROOT = Path(__file__).resolve().parents[1]
PINS = ROOT / "docs/benchmarks/corpora/neutron-public-corpora.json"
MAX_FILE = (1 << 32) - 1
MAX_OUTPUT = 65536
MAX_CORPUS = 768 * 1024 * 1024
HYPERFINE = ROOT / ("out/benchmark-tools/hyperfine/unpacked/hyperfine-v1.20.0-x86_64-pc-windows-msvc/hyperfine.exe")


# Purpose: Admit corpus residency while preserving the shared host's physical-memory reserve.
# Inputs: Planned owned bytes, including compressed and decoded source plus one child input copy.
# Outputs: Raises before allocation unless growth fits half of free memory and leaves at least two GiB.
def admit_memory(owned_bytes: int) -> None:
    class MemoryStatus(ctypes.Structure):
        _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong)] + [
            (name, ctypes.c_ulonglong)
            for name in (
                "total",
                "available",
                "page_total",
                "page_available",
                "virtual",
                "virtual_available",
                "extended",
            )
        ]

    if os.name != "nt":
        raise ValueError("this qualification requires Windows and the production HIP boundary")
    status = MemoryStatus()
    status.length = ctypes.sizeof(status)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        raise ctypes.WinError()
    if owned_bytes <= 0 or owned_bytes > min(status.available // 2, max(0, status.available - 2 * 1024**3)):
        raise ValueError("public corpus exceeds current shared-host RAM admission")


# Purpose: Download one fixed reviewed source entirely in RAM with byte and lifetime limits.
# Inputs: Pinned HTTPS URL, maximum bytes, exact expected length and optional authenticated content digest.
# Outputs: Immutable bytes; redirects, excess data, changed identity and incomplete responses fail closed.
def download(spec: dict, *, deadline: float) -> bytes:
    request = Request(spec["url"], headers={"User-Agent": "Mozilla/5.0 SuperZip-public-corpus-research"})
    payload = io.BytesIO()
    with build_opener(NoReleaseRedirects()).open(request, timeout=30) as response:
        if response.status != 200:
            raise ValueError("corpus download did not return a complete HTTPS response")
        if spec.get("etag") and response.headers.get("ETag") != spec["etag"]:
            raise ValueError("published corpus object identity changed; review its pin")
        while chunk := response.read(min(1024 * 1024, spec["archive_bytes"] + 1 - payload.tell())):
            if time.monotonic() > deadline:
                raise TimeoutError("public corpus acquisition deadline exceeded")
            payload.write(chunk)
            if payload.tell() > spec["archive_bytes"]:
                raise ValueError("corpus archive exceeded its declared extent")
    result = payload.getvalue()
    if len(result) != spec["archive_bytes"]:
        raise ValueError("corpus archive length changed")
    if spec.get("archive_sha256") and hashlib.sha256(result).hexdigest() != spec["archive_sha256"]:
        raise ValueError("corpus archive digest changed")
    return result


# Purpose: Validate the complete published ZIP inventory before decoding any member.
# Inputs: ZIP metadata, pinned inventory extent and optional canonical per-file lengths.
# Outputs: Rejects links, encryption, unsafe/duplicate names, unsupported methods and inconsistent inventory.
def validate_members(members: list[zipfile.ZipInfo], spec: dict) -> None:
    if len(members) != spec["file_count"] or sum(member.file_size for member in members) != spec["decoded_bytes"]:
        raise ValueError("published corpus inventory changed")
    seen: set[str] = set()
    for member in members:
        path = PurePosixPath(member.filename)
        mode = member.external_attr >> 16
        if (
            not member.filename
            or member.orig_filename != member.filename
            or "\\" in member.filename
            or ":" in member.filename
            or path.is_absolute()
            or any(part in ("", ".", "..") for part in member.filename.split("/"))
            or member.filename in seen
            or member.is_dir()
            or stat.S_ISLNK(mode)
            or stat.S_IFMT(mode) not in (0, stat.S_IFREG)
            or member.flag_bits & 1
            or member.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)
            or member.file_size <= 0
        ):
            raise ValueError("corpus contains an unsupported member")
        seen.add(member.filename)
    expected = spec.get("file_bytes")
    if expected and {member.filename: member.file_size for member in members} != expected:
        raise ValueError("canonical corpus member names or lengths changed")


# Purpose: Decode unmodified natural files into bounded RAM; retain every admission rejection explicitly.
# Inputs: A reviewed ZIP and its complete fixed inventory; no extraction path or executable payload consumer exists.
# Outputs: Resident bytes and public hashes in published ZIP order; unsupported extents reject the complete study.
def decode_corpus(payload: bytes, spec: dict) -> tuple[list[dict], dict]:
    if len(payload) > MAX_CORPUS or spec["decoded_bytes"] > MAX_CORPUS:
        raise ValueError("corpus exceeds the controller memory ceiling")
    files: list[dict] = []
    excluded: list[dict] = []
    with zipfile.ZipFile(io.BytesIO(payload)) as archive:
        members = archive.infolist()
        validate_members(members, spec)
        for member in members:
            if member.file_size > MAX_FILE:
                raise ValueError("published corpus member exceeds the CLI extent representation")
            with archive.open(member) as stream:
                data = stream.read(member.file_size + 1)
            if len(data) != member.file_size:
                raise ValueError("decoded corpus member length differs from metadata")
            files.append(
                {"name": member.filename, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "data": data}
            )
    return files, {
        "archive_sha256": hashlib.sha256(payload).hexdigest(),
        "published_file_count": len(members),
        "admitted_file_count": len(files),
        "input_bytes": sum(row["bytes"] for row in files),
        "excluded": excluded,
        "ordering": "published ZIP member order; independent files; no split, padding, repeat or generated replacement",
    }


# Purpose: Read the university's original Canterbury TAR bytes without changing text line endings.
# Inputs: Digest-pinned gzip archive, fixed decoded-container ceiling and canonical eleven-file inventory.
# Outputs: Every natural file and its byte identity in RAM; special members and changed inventory fail closed.
def decode_canterbury(payload: bytes, spec: dict) -> tuple[list[dict], dict]:
    with gzip.GzipFile(fileobj=io.BytesIO(payload)) as stream:
        expanded = stream.read(spec["container_bytes_max"] + 1)
    if len(expanded) > spec["container_bytes_max"]:
        raise ValueError("Canterbury TAR exceeds its decoded-container bound")
    files: list[dict] = []
    with tarfile.open(fileobj=io.BytesIO(expanded), mode="r:") as archive:
        members = archive.getmembers()
        if {row.name: row.size for row in members} != spec["file_bytes"] or len(members) != spec["file_count"]:
            raise ValueError("original Canterbury member names or lengths changed")
        for member in members:
            if not member.isfile() or member.name != PurePosixPath(member.name).name:
                raise ValueError("Canterbury contains a special or non-flat member")
            with archive.extractfile(member) as stream:
                data = stream.read(member.size + 1)
            if len(data) != member.size:
                raise ValueError("Canterbury decoded extent differs from its published inventory")
            files.append(
                {"name": member.name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "data": data}
            )
    return files, {
        "archive_sha256": hashlib.sha256(payload).hexdigest(),
        "published_file_count": len(members),
        "admitted_file_count": len(files),
        "input_bytes": sum(row["bytes"] for row in files),
        "excluded": [],
        "ordering": "original publisher TAR order; independent unmodified files",
    }


# Purpose: Read a child stream with a hard bound while preserving every protocol byte.
# Inputs: An owned pipe; the maximum applies during reading, not after unlimited capture.
# Outputs: Bounded bytes; excess output raises for the launch owner's process-tree cleanup.
def read_bounded(stream) -> bytes:
    output = bytearray()
    while chunk := stream.read(min(4096, MAX_OUTPUT + 1 - len(output))):
        output.extend(chunk)
        if len(output) > MAX_OUTPUT:
            raise ValueError("benchmark process exceeded its output ceiling")
    return bytes(output)


# Purpose: Reuse the repository Windows job contract without importing a same-named installed MCP package.
# Inputs: Exact repository module path; no user-controlled import source is accepted.
# Outputs: The canonical process ownership implementation, or an explicit import failure.
def process_owner():
    spec = importlib.util.spec_from_file_location("superzip_neutron_owned_child", ROOT / "mcp/superzip_mcp.py")
    if spec is None or spec.loader is None:
        raise ValueError("Repository process ownership contract is unavailable")
    owner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(owner)
    return owner


# Purpose: Feed and close an owned child's binary stdin without storing payloads on disk.
# Inputs: Child pipe and immutable resident corpus bytes.
# Outputs: Writes the complete source or propagates transport failure; closing establishes exact EOF.
def write_input(stream, data: bytes) -> None:
    try:
        stream.write(data)
    finally:
        stream.close()


# Purpose: Run an existing executable with bounded binary I/O and an owned lifetime.
# Inputs: Structured argument vector, optional RAM input/environment and finite timeout; no shell interprets arguments.
# Outputs: Complete bounded stdout or explicit failure retaining both diagnostic streams; all owned children finish.
def run_process(arguments: list[str], data: bytes | None, timeout: int, *, env: dict | None = None) -> bytes:
    owner = process_owner()
    admitted_bytes = owner.admit_child_memory_bytes()
    with ExitStack() as stack:
        process = subprocess.Popen(
            arguments,
            cwd=ROOT,
            env=env,
            stdin=subprocess.PIPE if data is not None else subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS | owner.CREATE_SUSPENDED,
        )
        for stream in (process.stdin, process.stdout, process.stderr):
            if stream is not None:
                stack.callback(stream.close)
        containment, pool = None, None
        try:
            containment = owner.ChildContainment(process, admitted_bytes)
            owner.resume_owned_child(process)
            pool = ThreadPoolExecutor(max_workers=3)
            stdout = pool.submit(read_bounded, process.stdout)
            stderr = pool.submit(read_bounded, process.stderr)
            writer = pool.submit(write_input, process.stdin, data) if data is not None else None
            deadline = time.monotonic() + timeout
            while process.poll() is None:
                for future in (stdout, stderr, writer):
                    if future is not None and future.done():
                        future.result()
                if time.monotonic() >= deadline:
                    raise subprocess.TimeoutExpired(arguments, timeout)
                time.sleep(0.01)
            # Descendants cannot keep inherited pipe handles alive after their root exits.
            containment.close()
            output, errors = stdout.result(timeout=5), stderr.result(timeout=5)
            if writer:
                writer.result(timeout=5)
            if process.returncode or errors:
                raise ValueError(
                    f"benchmark child failed ({process.returncode}): "
                    f"stderr={errors.decode('utf-8', errors='replace')} "
                    f"stdout={output.decode('utf-8', errors='replace')}"
                )
            return output
        finally:
            try:
                if containment is not None:
                    containment.close()
            finally:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=5)
                if pool is not None:
                    pool.shutdown(wait=True, cancel_futures=True)


# Purpose: Bind measured telemetry to the exact natural input and required-HIP Neutron execution.
# Inputs: Bounded native protocol bytes and the expected member's byte/hash identity.
# Outputs: Original complete protocol fields; invalid identity, CPU substitution or output writes fail closed.
def parse_stats(payload: bytes, member: dict) -> dict:
    pairs = [item.split("=", 1) for item in payload.decode("ascii").strip().split()]
    if any(len(pair) != 2 for pair in pairs) or len({pair[0] for pair in pairs}) != len(pairs):
        raise ValueError("malformed or duplicate native telemetry field")
    fields = dict(pairs)
    expected = {
        "memory_only": "true",
        "disk_write_bytes": "0",
        "compression_mode": "neutron_star",
        "measurement_protocol": "bytewise-corpus-v1",
        "data_source": "preloaded",
        "gpu_used": "true",
        "source_sha256": member["sha256"],
        "input_bytes": str(member["bytes"]),
        "validated_bytes": str(member["bytes"]),
    }
    if any(fields.get(key) != value for key, value in expected.items()):
        raise ValueError("Neutron corpus telemetry disagrees with the exact RAM source or required backend")
    for key in ("gpu_encode_chunks", "gpu_decode_chunks", "gpu_kernel_launches", "archive_bytes", "output_bytes"):
        if int(fields.get(key, "0")) <= 0:
            raise ValueError("Neutron corpus telemetry lacks complete archive or actual GPU work")
    # Native output_bytes counts compressed payload; restored byte coverage is validated_bytes.
    if int(fields["output_bytes"]) > int(fields["archive_bytes"]):
        raise ValueError("Neutron compressed payload exceeds the complete framed archive")
    seconds = float(fields.get("compress_seconds", "nan"))
    if not math.isfinite(seconds) or seconds < 0:
        raise ValueError("Neutron corpus telemetry lacks a finite compression duration")
    return fields


# Purpose: Adapt the native benchmark to a read-only resident corpus while Hyperfine owns repetitions.
# Inputs: Inherited bounded mapping descriptor, fixed native binary, block size and per-file deadline.
# Outputs: Exact HIP observations in fixed RAM slots; no network, pickle or filesystem payload transport.
def worker(descriptor: dict, cli: Path, block: int, timeout: int) -> None:
    with ExitStack() as stack:
        source, results, files = ipc.open_session(stack, descriptor)
        with ipc.exclusive_writer(descriptor["mutex"]):
            first = ipc.completed_slots(results)
            if first % len(files) or first >= descriptor["file_count"] * descriptor["runs"]:
                raise ValueError("Incomplete preceding corpus run or excess repetition")
            for index, member in enumerate(files):
                admit_memory(2 * member["bytes"])
                start = descriptor["manifest_bytes"] + member["offset"]
                data = bytes(source[start : start + member["bytes"]])
                if hashlib.sha256(data).hexdigest() != member["sha256"]:
                    raise ValueError("RAM corpus transport identity changed")
                command = [
                    str(cli),
                    "memory-benchmark",
                    "--source-stdin",
                    "--source-bytes",
                    str(len(data)),
                    "--source-sha256",
                    member["sha256"],
                    "--neutron-star",
                    "--block-size-kib",
                    str(block),
                ]
                protocol = run_process(command, data, timeout)
                parse_stats(protocol, member)
                ipc.publish_observation(results, first + index, protocol)


# Purpose: Hold immutable native output and build-transaction identities for an entire measurement batch.
# Inputs: Validated receipt and ExitStack owning all Windows read-only, deny-write/delete handles.
# Outputs: Prevents rebuilding or replacing measured outputs; acquisition failures unwind every owned handle.
def hold_build(stack: ExitStack, receipt: dict) -> None:
    library = ctypes.WinDLL("kernel32", use_last_error=True)
    create = library.CreateFileW
    create.argtypes = [
        ctypes.c_wchar_p,
        ctypes.c_ulong,
        ctypes.c_ulong,
        ctypes.c_void_p,
        ctypes.c_ulong,
        ctypes.c_ulong,
        ctypes.c_void_p,
    ]
    create.restype = ctypes.c_void_p
    close = library.CloseHandle
    close.argtypes = [ctypes.c_void_p]
    names = list(receipt["receipt"]["outputs_sha256"]) + [
        "build/native-build.lock",
        "build/native-build-receipt.json",
        "build/native-build-transaction.json",
        "build/CMakeCache.txt",
        "tools/neutron_corpus_benchmark.py",
        "tools/neutron_corpus_ipc.py",
        HYPERFINE.relative_to(ROOT).as_posix(),
        "mcp/superzip_mcp.py",
        "tools/benchmark_permissions.json",
        "docs/benchmarks/corpora/neutron-public-corpora.json",
    ]
    for name in names:
        path = input_path(ROOT, name)
        handle = create(str(path), 0x80000000, 1, None, 3, 0x80, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        stack.callback(close, handle)


# Purpose: Preserve completed observations on stdout and aggregate sizes without duplicating every RAM protocol.
# Inputs: Owned slots after all workers exit, complete corpus metadata and descriptor; partial output stays unqualified.
# Outputs: Count and small per-run totals; raw protocols remain visible even when a later native operation failed.
def emit_observations(output: memoryview, descriptor: dict, files: list[dict]) -> tuple[int, list[dict]]:
    admit_memory(16 * ipc.MAX_PROTOCOL)
    totals = [
        {"run": run + 1, "input_bytes": 0, "archive_bytes": 0, "compress_seconds": 0.0}
        for run in range(descriptor["runs"])
    ]
    count = 0
    for slot, row in enumerate(ipc.observations(output, descriptor, files, parse_stats, require_complete=False)):
        print(
            json.dumps(
                {"observation": slot + 1, "expected": len(files) * descriptor["runs"], "study_qualified": False, **row}
            ),
            flush=True,
        )
        total = totals[slot // len(files)]
        total["input_bytes"] += int(row["stats"]["input_bytes"])
        total["archive_bytes"] += int(row["stats"]["archive_bytes"])
        total["compress_seconds"] += float(row["stats"]["compress_seconds"])
        count += 1
    return count, totals


# Purpose: Serialize arguments for Hyperfine's shell_words parser while retaining direct process execution.
# Inputs: Nonempty structured argument vector; Windows paths are quoted for this parser, not for cmd.exe.
# Outputs: Exact command string for --shell none; no intermediate shell or environment expansion is introduced.
def hyperfine_command(arguments: list[str]) -> str:
    if not arguments or any(not isinstance(value, str) or "\0" in value for value in arguments):
        raise ValueError("Invalid Hyperfine argument vector")
    return shlex.join(arguments)


# Purpose: Run a complete public-corpus study with existing Hyperfine, fixed residency and exact native qualification.
# Inputs: Reviewed preset, bounded repetitions/block size/deadlines and current successful HIP build.
# Outputs: Progress plus complete raw observations and Hyperfine summary on stdout; no corpus/archive/report files.
def study(args: argparse.Namespace) -> None:
    require_permission(args.corpus, "execute")
    require_permission("Hyperfine", "execute", version="1.20.0")
    spec = json.loads(PINS.read_text(encoding="utf-8"))[args.corpus]
    with ExitStack() as stack:
        before = validate_current(ROOT, args.configuration, True)
        hold_build(stack, before)
        if validate_current(ROOT, args.configuration, True) != before:
            raise ValueError("native identity changed while acquiring the measurement lease")
        banner = run_process([str(HYPERFINE), "--version"], None, 10).decode("ascii").strip()
        if banner != "hyperfine 1.20.0":
            raise ValueError("Installed Hyperfine does not match the reviewed version")
        with HYPERFINE.open("rb") as executable:
            hyperfine_sha256 = hashlib.file_digest(executable, "sha256").hexdigest()
        admit_memory(2 * spec["archive_bytes"] + spec["decoded_bytes"] + 2 * spec["max_file_bytes"])
        payload = download(spec, deadline=time.monotonic() + 180)
        decoder = decode_canterbury if args.corpus == "Canterbury" else decode_corpus
        files, provenance = decoder(payload, spec)
        del payload
        print(json.dumps({"corpus": args.corpus, **provenance}), flush=True)
        descriptor = ipc.prepare_session(stack, files, args.runs, admit_memory)
        _, output, _ = ipc.open_session(stack, descriptor)
        environment = {**os.environ, ipc.SESSION_ENV: json.dumps(descriptor)}
        command = hyperfine_command(
            [
                sys.executable,
                "-B",
                "-m",
                "tools.neutron_corpus_benchmark",
                "--worker",
                "--configuration",
                args.configuration,
                "--block-size-kib",
                str(args.block_size_kib),
                "--file-timeout",
                str(args.file_timeout),
            ]
        )
        try:
            summary = run_process(
                [
                    str(HYPERFINE),
                    "--runs",
                    str(args.runs),
                    "--warmup",
                    "0",
                    "--shell",
                    "none",
                    "--style",
                    "basic",
                    "--output",
                    "inherit",
                    "--command-name",
                    f"Neutron/{args.corpus}",
                    command,
                ],
                None,
                args.suite_timeout,
                env=environment,
            )
        finally:
            count, totals = emit_observations(output, descriptor, files)
        if count != len(files) * args.runs or validate_current(ROOT, args.configuration, True) != before:
            raise ValueError("Incomplete corpus study or changed native qualification")
        print(
            json.dumps(
                {
                    "native_inputs_sha256": before["inputs_sha256"],
                    "native_receipt_sha256": before["receipt_sha256"],
                    "hyperfine": summary.decode("utf-8"),
                    "hyperfine_sha256": hyperfine_sha256,
                    "memory_only": True,
                    "disk_write_bytes": 0,
                    "observation_count": count,
                    "study_qualified": True,
                    "totals": totals,
                    "timing_qualified": False,
                    "timing_limit": (
                        "Hyperfine measures feeder/process overhead too; host-isolation qualification is separate"
                    ),
                }
            ),
            flush=True,
        )


# Purpose: Route the public-corpus controller or its private RAM feeder with bounded options.
# Inputs: Reviewed corpus/options; private worker mode requires an inherited bounded mapping descriptor.
# Outputs: Study/protocol stdout or an actionable nonzero failure; no native build is invoked implicitly.
def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", choices=("Canterbury", "Govdocs1Thread0"), default="Canterbury")
    parser.add_argument("--configuration", choices=("Release", "Debug", "RelWithDebInfo"), default="Release")
    parser.add_argument("--runs", type=int, choices=range(1, 11), default=3)
    parser.add_argument("--block-size-kib", type=int, choices=(256, 512, 1024, 2048, 4096, 8192, 16384), default=256)
    parser.add_argument("--file-timeout", type=int, choices=range(1, 3601), default=300)
    parser.add_argument("--suite-timeout", type=int, choices=range(1, 7201), default=3600)
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        descriptor = json.loads(os.environ[ipc.SESSION_ENV])
        worker(
            descriptor,
            ROOT / f"build/{args.configuration}/superzip_cli.exe",
            args.block_size_kib,
            args.file_timeout,
        )
    else:
        study(args)


if __name__ == "__main__":
    main()
