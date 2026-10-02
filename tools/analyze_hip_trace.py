"""Summarize bounded, thread-identified HIP API traces without exposing raw arguments."""

from __future__ import annotations

import argparse
import json
import re
import statistics
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

MAX_TRACE_BYTES = 64 * 1024 * 1024
MAX_LINE_CHARS = 64 * 1024
HEADER = re.compile(r"(?P<time>\d+) us:\s*\[pid:(?P<pid>\d+) tid:\s*0x\s*(?P<tid>[0-9a-fA-F]+)\]\s*(?P<body>.*)")
ENTRY = re.compile(r"(?P<api>hip[A-Za-z0-9_]{1,92})\s+\(\s*(?P<args>.*?)\s*\)\s*\Z")
RETURN = re.compile(r"(?P<api>hip[A-Za-z0-9_]{1,92}): Returned (?P<status>\w+)\b.*\Z")
UNIDENTIFIED_API = re.compile(r"\d+ us:\s*hip[A-Za-z0-9_]+(?:\s+\(|: Returned )")
API_RECORD = re.compile(r"hip[A-Za-z0-9_]+(?:\s+\(|:\s*Returned\b)")
SGR_COLOR = re.compile(r"\x1b\[[0-9;]*m")
BYTE_COUNT = re.compile(r"[0-9]{1,20}\Z")


@dataclass(frozen=True)
class TraceCall:
    """Private parsed call; thread identity and arguments never appear in the public report."""

    thread: tuple[int, int]
    api: str
    started_us: int
    ended_us: int
    arguments: str
    succeeded: bool


# Purpose: Pair per-thread API entry/return records despite cross-thread logger interleaving.
# Inputs: Bounded trace text with level-four thread identity; raw arguments remain private.
# Outputs: Returns complete calls or rejects missing identity, truncated calls, and inconsistent clocks/stacks.
def parse_trace(text: str) -> list[TraceCall]:
    if len(text.encode("utf-8")) > MAX_TRACE_BYTES:
        raise ValueError("HIP trace exceeds the 64 MiB input limit")
    stacks: dict[tuple[int, int], list[tuple[str, int, str]]] = defaultdict(list)
    calls = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        if len(line) > MAX_LINE_CHARS:
            raise ValueError("HIP trace line exceeds the input limit")
        line = SGR_COLOR.sub("", line)
        header = HEADER.search(line)
        if header is None:
            if UNIDENTIFIED_API.search(line):
                raise ValueError("HIP API trace lacks thread identity; capture with AMD_LOG_LEVEL=4")
            continue
        thread = (int(header["pid"]), int(header["tid"], 16))
        timestamp = int(header["time"])
        body = header["body"]
        entry = ENTRY.fullmatch(body)
        returned = RETURN.fullmatch(body)
        if entry is not None:
            stacks[thread].append((entry["api"], timestamp, entry["args"]))
        elif returned is not None:
            if not stacks[thread] or stacks[thread][-1][0] != returned["api"]:
                raise ValueError(f"HIP API trace contains an unmatched return at line {line_number}")
            api, started, arguments = stacks[thread].pop()
            if timestamp < started:
                raise ValueError("HIP API trace contains a reversed host interval")
            calls.append(TraceCall(thread, api, started, timestamp, arguments, returned["status"] == "hipSuccess"))
        elif API_RECORD.match(body):
            raise ValueError(f"HIP API trace contains a malformed API record at line {line_number}")
    if any(stacks.values()):
        raise ValueError("HIP API trace contains incomplete calls")
    if not calls:
        raise ValueError("HIP API trace contains no complete calls")
    return calls


# Purpose: Measure a union of intervals so nested API calls do not double-count host occupancy.
# Inputs: Complete calls belonging to one thread, in any logger order.
# Outputs: Returns traced host microseconds; this is not device execution or overall wall time.
def occupied_microseconds(calls: list[TraceCall]) -> int:
    total = 0
    end = 0
    for call in sorted(calls, key=lambda item: item.started_us):
        total += max(0, call.ended_us - max(end, call.started_us))
        end = max(end, call.ended_us)
    return total


# Purpose: Validate one logged size_t without interpreting pointers or admitting unbounded numeric conversions.
# Inputs: An ASCII decimal byte count from a supported API layout. Outputs: Returns uint64 bytes or raises ValueError.
def byte_count(value: str) -> int:
    if BYTE_COUNT.fullmatch(value) is None:
        raise ValueError("HIP trace byte count is unsupported")
    count = int(value)
    if count > (1 << 64) - 1:
        raise ValueError("HIP trace byte count is unsupported")
    return count


# Purpose: Identify requested allocation extents independently of pointer values and successful ownership.
# Inputs: One private parsed allocation call. Outputs: Returns bytes, None for other APIs, or a value-free error.
def allocation_request(call: TraceCall) -> int | None:
    layouts = {"hipMalloc": 2, "hipMallocAsync": 3, "hipMallocFromPoolAsync": 4, "hipHostMalloc": 3}
    expected = layouts.get(call.api)
    if expected is None:
        return None
    arguments = [value.strip() for value in call.arguments.split(",")]
    if len(arguments) != expected:
        raise ValueError("HIP trace allocation arguments are unsupported")
    return byte_count(arguments[1])


# Purpose: Summarize the same host interval statistics for API and numeric argument groups.
# Inputs: A nonempty collection of complete calls. Outputs: Counts failures and costs without returning raw arguments.
def summarize_cost(group: list[TraceCall]) -> dict:
    durations = [call.ended_us - call.started_us for call in group]
    return {
        "calls": len(group),
        "failed_calls": sum(not call.succeeded for call in group),
        "traced_host_total_us": sum(durations),
        "traced_host_median_us": statistics.median(durations),
        "traced_host_max_us": max(durations),
    }


# Purpose: Extract only numeric copy volume and direction from a supported synchronous HIP transfer.
# Inputs: One private parsed call; pointer values are never returned.
# Outputs: Returns direction/byte count or None for other calls; rejects malformed copy argument layouts.
def transfer_volume(call: TraceCall) -> tuple[str, int] | None:
    if call.api not in {"hipMemcpy", "hipMemcpyWithStream"} or not call.succeeded:
        return None
    arguments = [value.strip() for value in call.arguments.split(",")]
    directions = {"hipMemcpyHostToDevice", "hipMemcpyDeviceToHost", "hipMemcpyDeviceToDevice", "hipMemcpyHostToHost"}
    expected = 5 if call.api == "hipMemcpyWithStream" else 4
    if len(arguments) != expected or arguments[3] not in directions:
        raise ValueError("HIP trace transfer arguments are unsupported")
    return arguments[3], byte_count(arguments[2])


# Purpose: Produce a privacy-preserving aggregate report for a complete trace or explicit host-time window.
# Inputs: Complete calls and optional inclusive start/exclusive end bounds in the trace clock's microseconds.
# Outputs: Returns API/size/direction costs and transfer volumes; no pointers, paths, PIDs, or thread IDs.
def build_report(calls: list[TraceCall], start_us: int | None = None, end_us: int | None = None) -> dict:
    if (start_us is not None and start_us < 0) or (end_us is not None and end_us < 0):
        raise ValueError("HIP trace time bounds must be nonnegative")
    if start_us is not None and end_us is not None and start_us >= end_us:
        raise ValueError("HIP trace time window is empty or reversed")
    selected = [
        call
        for call in calls
        if (start_us is None or call.started_us >= start_us) and (end_us is None or call.ended_us < end_us)
    ]
    if not selected:
        raise ValueError("HIP trace window contains no complete calls")
    groups: dict[str, list[TraceCall]] = defaultdict(list)
    threads: dict[tuple[int, int], list[TraceCall]] = defaultdict(list)
    transfers: dict[str, int] = defaultdict(int)
    allocations: dict[tuple[str, int], list[TraceCall]] = defaultdict(list)
    copy_groups: dict[tuple[str, str, int], list[TraceCall]] = defaultdict(list)
    for call in selected:
        groups[call.api].append(call)
        threads[call.thread].append(call)
        requested = allocation_request(call)
        if requested is not None:
            allocations[call.api, requested].append(call)
        transfer = transfer_volume(call)
        if transfer is not None:
            transfers[transfer[0]] += transfer[1]
            copy_groups[call.api, transfer[0], transfer[1]].append(call)
    return {
        "schema_version": 1,
        "measurement_kind": "instrumented_hip_api_diagnostic_not_benchmark",
        "complete_calls": len(calls),
        "selected_calls": len(selected),
        "excluded_calls": len(calls) - len(selected),
        "selected_thread_count": len(threads),
        "selected_trace_span_us": max(call.ended_us for call in selected) - min(call.started_us for call in selected),
        "traced_host_occupied_worker_us": sum(occupied_microseconds(group) for group in threads.values()),
        "transfer_bytes": dict(sorted(transfers.items())),
        "apis": [{"api": api, **summarize_cost(group)} for api, group in sorted(groups.items())],
        # Requests are not live/peak VRAM, physical driver extents or proof of successful allocation.
        "allocation_requests": [
            {"api": api, "requested_bytes": requested, **summarize_cost(group)}
            for (api, requested), group in sorted(allocations.items())
        ],
        "transfer_calls": [
            {"api": api, "direction": direction, "bytes_per_call": count, **summarize_cost(group)}
            for (api, direction, count), group in sorted(copy_groups.items())
        ],
    }


# Purpose: Read one bounded trace without following it while the producing process is still appending.
# Inputs: A completed local UTF-8 trace; the caller controls the path and preserves the raw evidence separately.
# Outputs: Returns decoded text or rejects oversized, unreadable, or non-UTF-8 input.
def read_trace(path: Path) -> str:
    with path.open("rb") as source:
        content = source.read(MAX_TRACE_BYTES + 1)
    if len(content) > MAX_TRACE_BYTES:
        raise ValueError("HIP trace exceeds the 64 MiB input limit")
    return content.decode("utf-8-sig")


# Purpose: Run the offline HIP trace analyzer and emit sanitized JSON only.
# Inputs: Trace path and optional timestamp bounds; does not launch workloads or change host configuration.
# Outputs: Writes a diagnostic report or a value-free error and nonzero exit status.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--start-us", type=int)
    parser.add_argument("--end-us", type=int)
    arguments = parser.parse_args()
    try:
        report = build_report(parse_trace(read_trace(arguments.trace)), arguments.start_us, arguments.end_us)
    except (OSError, UnicodeError):
        parser.exit(1, "HIP trace analysis failed: unreadable trace\n")
    except ValueError as error:
        parser.exit(1, f"HIP trace analysis failed: {error}\n")
    print(json.dumps(report, indent=2, allow_nan=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
