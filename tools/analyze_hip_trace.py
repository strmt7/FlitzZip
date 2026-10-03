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
API_NAME = r"(?:__)?hip[A-Za-z0-9_]{1,92}"
ENTRY = re.compile(rf"(?P<api>{API_NAME})\s+\(\s*(?P<args>.*?)\s*\)\s*\Z")
RETURN = re.compile(rf"(?P<api>{API_NAME}): Returned (?P<status>\w+)\b.*\Z")
UNIDENTIFIED_API = re.compile(r"\d+ us:\s*(?:__)?hip[A-Za-z0-9_]+(?:\s+\(|: Returned )")
API_RECORD = re.compile(r"(?:__)?hip[A-Za-z0-9_]+(?:\s+\(|:\s*Returned\b)")
# AMD hip_error.cpp logs entry but returns directly for these two empty-argument queries.
# hipPeekAtLastError uses HIP_RETURN and must retain the normal complete-call contract.
ENTRY_ONLY_STATUS_QUERIES = frozenset({"hipGetLastError", "hipExtGetLastError"})
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


@dataclass(frozen=True)
class TraceEntry:
    """Private entry record; an unpaired record does not establish duration, status, or a unique call."""

    thread: tuple[int, int]
    api: str
    started_us: int
    arguments: str


@dataclass(frozen=True)
class ParsedTrace:
    """Complete calls and separately retained, explicitly admitted untimed logger records."""

    calls: list[TraceCall]
    untimed_entries: list[TraceEntry]


# Purpose: Identify the narrow runtime logging exception without guessing status or elapsed time.
# Inputs: One pending private entry and the explicit caller opt-in.
# Outputs: Returns true only for an admitted empty-argument status-query record.
def is_entry_only_query(entry: TraceEntry, allowed: bool) -> bool:
    return allowed and entry.api in ENTRY_ONLY_STATUS_QUERIES and not entry.arguments


# Purpose: Pair per-thread API records and retain explicitly admitted entry-only status queries separately.
# Inputs: Bounded level-four trace text and an opt-in for the two documented empty-argument query layouts.
# Outputs: Returns complete calls/untimed records; rejects all other incomplete, unidentified or inconsistent calls.
def parse_trace_records(text: str, allow_entry_only_status_queries: bool = False) -> ParsedTrace:
    if len(text.encode("utf-8")) > MAX_TRACE_BYTES:
        raise ValueError("HIP trace exceeds the 64 MiB input limit")
    stacks: dict[tuple[int, int], list[TraceEntry]] = defaultdict(list)
    calls = []
    untimed_entries = []
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
            stacks[thread].append(TraceEntry(thread, entry["api"], timestamp, entry["args"]))
        elif returned is not None:
            stack = stacks[thread]
            while (
                stack
                and stack[-1].api != returned["api"]
                and is_entry_only_query(stack[-1], allow_entry_only_status_queries)
            ):
                pending = stack.pop()
                if timestamp < pending.started_us:
                    raise ValueError("HIP API trace contains a reversed host interval")
                untimed_entries.append(pending)
            if not stack or stack[-1].api != returned["api"]:
                raise ValueError(f"HIP API trace contains an unmatched return at line {line_number}")
            pending = stack.pop()
            if timestamp < pending.started_us:
                raise ValueError("HIP API trace contains a reversed host interval")
            calls.append(
                TraceCall(
                    thread,
                    pending.api,
                    pending.started_us,
                    timestamp,
                    pending.arguments,
                    returned["status"] == "hipSuccess",
                )
            )
        elif API_RECORD.match(body):
            raise ValueError(f"HIP API trace contains a malformed API record at line {line_number}")
    for stack in stacks.values():
        while stack and is_entry_only_query(stack[-1], allow_entry_only_status_queries):
            untimed_entries.append(stack.pop())
        if stack:
            raise ValueError("HIP API trace contains incomplete calls")
    if not calls:
        raise ValueError("HIP API trace contains no complete calls")
    return ParsedTrace(calls, untimed_entries)


# Purpose: Preserve the strict complete-call parser for existing consumers.
# Inputs: Bounded level-four trace text. Outputs: Returns calls or rejects any unpaired entry, including status queries.
def parse_trace(text: str) -> list[TraceCall]:
    return parse_trace_records(text).calls


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
# Inputs: Complete calls, optional inclusive start/exclusive end bounds and explicitly admitted untimed records.
# Outputs: Returns costs/volumes and separate entry-record counts; no inferred status, duration or private identities.
def build_report(
    calls: list[TraceCall],
    start_us: int | None = None,
    end_us: int | None = None,
    untimed_entries: list[TraceEntry] | None = None,
) -> dict:
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
    report = {
        "schema_version": 1 if untimed_entries is None else 2,
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
    if untimed_entries is not None:
        report["untimed_entry_records"] = summarize_untimed_entries(untimed_entries, start_us, end_us)
    return report


# Purpose: Account for every admitted logger entry independently of timed calls and unique-call inference.
# Inputs: Private empty-argument query records and already validated timestamp bounds.
# Outputs: Returns total/selected/excluded record counts and explicit unknown status/duration per API.
def summarize_untimed_entries(entries: list[TraceEntry], start_us: int | None, end_us: int | None) -> dict:
    groups: dict[str, int] = defaultdict(int)
    for entry in entries:
        if not is_entry_only_query(entry, True):
            raise ValueError("HIP trace contains an unsupported untimed entry")
        if (start_us is None or entry.started_us >= start_us) and (end_us is None or entry.started_us < end_us):
            groups[entry.api] += 1
    selected = sum(groups.values())
    return {
        "policy": "explicit_empty_argument_status_query_entries",
        "total_records": len(entries),
        "selected_records": selected,
        "excluded_records": len(entries) - selected,
        "apis": [
            {"api": api, "entry_records": count, "duration_us": None, "status": None, "unique_calls": None}
            for api, count in sorted(groups.items())
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
    parser.add_argument(
        "--allow-entry-only-status-queries",
        action="store_true",
        help="retain empty hipGetLastError/hipExtGetLastError entries with unknown cost/status",
    )
    arguments = parser.parse_args()
    try:
        parsed = parse_trace_records(read_trace(arguments.trace), arguments.allow_entry_only_status_queries)
        entries = parsed.untimed_entries if arguments.allow_entry_only_status_queries else None
        report = build_report(parsed.calls, arguments.start_us, arguments.end_us, entries)
    except (OSError, UnicodeError):
        parser.exit(1, "HIP trace analysis failed: unreadable trace\n")
    except ValueError as error:
        parser.exit(1, f"HIP trace analysis failed: {error}\n")
    print(json.dumps(report, indent=2, allow_nan=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
