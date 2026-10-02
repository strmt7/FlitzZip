"""Tests for complete, interleaved and privacy-preserving HIP API trace analysis."""

from __future__ import annotations

import io
import json
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest.mock import patch

from tools import analyze_hip_trace as trace


# Purpose: Build a synthetic logger record without using a host process, pointer, or installation identity.
# Inputs: Host timestamp, thread token, and API body.
# Outputs: Returns the level-four HIP logger layout used by the parser tests.
def line(time: int, thread: str, body: str) -> str:
    return f":3: :0 : {time} us: [pid:1 tid: 0x{thread}] {body}\n"


class HipTraceTests(unittest.TestCase):
    # Purpose: Separate allocation size and transfer direction without losing failure or window accounting.
    # Inputs: Complete interleaved calls, including failed allocation and copy records.
    # Outputs: Requires exact numeric groups, unchanged API totals and no raw private argument material.
    def test_allocation_and_transfer_cost_groups(self):
        calls = [
            trace.TraceCall((1, 1), "hipMalloc", 10, 20, "private-a, 512", True),
            trace.TraceCall((1, 2), "hipMalloc", 10, 40, "private-b, 512", False),
            trace.TraceCall((1, 1), "hipMallocAsync", 21, 24, "private-c, 512, private-stream", True),
            trace.TraceCall(
                (1, 1), "hipMallocFromPoolAsync", 25, 26, "private-d, 0, private-pool, private-stream", True
            ),
            trace.TraceCall((1, 1), "hipHostMalloc", 27, 28, "private-e, 128, 0", True),
            trace.TraceCall((1, 1), "hipMalloc", 29, 30, "private-h, 128", True),
            trace.TraceCall((1, 1), "hipMemcpy", 41, 46, "private-f, private-g, 128, hipMemcpyHostToDevice", True),
            trace.TraceCall((1, 1), "hipMemcpy", 47, 49, "private-f, private-g, 128, hipMemcpyDeviceToHost", True),
            trace.TraceCall((1, 1), "hipMemcpy", 50, 51, "private-f, private-g, 128, hipMemcpyHostToDevice", False),
            trace.TraceCall((1, 1), "hipMemcpy", 53, 54, "private-f, private-g, 64, hipMemcpyHostToDevice", True),
        ]
        report = trace.build_report(calls)
        allocated = {(item["api"], item["requested_bytes"]): item for item in report["allocation_requests"]}
        self.assertEqual(len(allocated), 5)
        self.assertEqual(allocated["hipMalloc", 512]["calls"], 2)
        self.assertEqual(allocated["hipMalloc", 512]["failed_calls"], 1)
        self.assertEqual(allocated["hipMalloc", 512]["traced_host_total_us"], 40)
        self.assertEqual(allocated["hipMalloc", 512]["traced_host_median_us"], 20)
        self.assertEqual(allocated["hipMalloc", 128]["calls"], 1)
        self.assertEqual(sum(item["calls"] for item in report["transfer_calls"]), 3)
        self.assertEqual(len(report["transfer_calls"]), 3)
        self.assertEqual(report["transfer_bytes"], {"hipMemcpyDeviceToHost": 128, "hipMemcpyHostToDevice": 192})
        narrowed = trace.build_report(calls, 21, 29)
        self.assertEqual(sum(item["calls"] for item in narrowed["allocation_requests"]), 3)
        self.assertEqual(narrowed["transfer_calls"], [])
        rendered = json.dumps(report)
        for token in ("private", "arguments", "pid:", "tid:"):
            self.assertNotIn(token, rendered)

    # Purpose: Refuse unsupported sizes/layouts consistently instead of treating arbitrary numeric text as bytes.
    # Inputs: Valid uint64 boundaries and malformed sizes for every admitted allocation API and synchronous copies.
    # Outputs: Requires exact bounds, explicit rejection and no allocation interpretation for unrelated APIs.
    def test_allocation_size_and_layout_validation(self):
        for value in ("0", "128", str((1 << 64) - 1)):
            self.assertEqual(trace.byte_count(value), int(value))
        for value in ("", "-1", "+1", "0x10", "1.0", "\u0661", "9" * 21, str(1 << 64)):
            with self.subTest(value=value), self.assertRaises(ValueError):
                trace.byte_count(value)
            with self.assertRaises(ValueError):
                trace.transfer_volume(
                    trace.TraceCall((1, 1), "hipMemcpy", 1, 2, f"p, q, {value}, hipMemcpyHostToDevice", True)
                )
        for api in ("hipMalloc", "hipMallocAsync", "hipMallocFromPoolAsync", "hipHostMalloc"):
            with self.subTest(api=api), self.assertRaises(ValueError):
                trace.allocation_request(trace.TraceCall((1, 1), api, 1, 2, "p", True))
        self.assertIsNone(trace.allocation_request(trace.TraceCall((1, 1), "hipFree", 1, 2, "p", True)))

    # Purpose: Admit the runtime's ANSI-colored file logs without weakening API pairing.
    # Inputs: One colored call/return pair and one colored record lacking thread identity.
    # Outputs: Checks exact duration and continued rejection of unidentified calls.
    def test_colored_logger_records(self):
        text = line(1, "a", "\x1b[32m hipFree ( pointer ) \x1b[0m") + line(
            9, "a", "\x1b[1;31mhipFree: Returned hipSuccess :\x1b[0m"
        )
        report = trace.build_report(trace.parse_trace(text))
        self.assertEqual(report["apis"][0]["traced_host_total_us"], 8)
        with self.assertRaises(ValueError):
            trace.parse_trace(":3: :0 : 1 us: \x1b[32m hipFree ( pointer )\x1b[0m\n")

    # Purpose: Preserve exact pairing when threads interleave and one API invokes another.
    # Inputs: Two threads with a nested API and successful returned records.
    # Outputs: Checks durations and union occupancy without counting nested time twice.
    def test_interleaved_nested_calls(self):
        text = (
            line(10, " a", "hipMalloc ( pointer, 16 )")
            + line(12, "b", "hipFree ( pointer )")
            + line(14, "a", "hipGetDevice ( pointer )")
            + line(16, "b", "hipFree: Returned hipSuccess :")
            + line(17, "a", "hipGetDevice: Returned hipSuccess : 0")
            + line(20, "a", "hipMalloc: Returned hipSuccess : pointer: duration: 10 us")
        )
        report = trace.build_report(trace.parse_trace(text))
        self.assertEqual(report["complete_calls"], 3)
        self.assertEqual(report["selected_thread_count"], 2)
        self.assertEqual(report["traced_host_occupied_worker_us"], 14)
        self.assertEqual(sum(api["traced_host_total_us"] for api in report["apis"]), 17)

    # Purpose: Keep trace reports free of raw identities, arguments, and failed-copy volume.
    # Inputs: Synthetic successful/failed copies with private-looking pointer argument text.
    # Outputs: Checks sanitized JSON, exact successful byte counts and visible failure counts.
    def test_copy_volume_and_privacy(self):
        text = (
            line(1, "a", "hipMemcpyWithStream ( private-host, private-device, 128, hipMemcpyDeviceToHost, stream )")
            + line(9, "a", "hipMemcpyWithStream: Returned hipSuccess :")
            + line(10, "a", "hipMemcpy ( private-host, private-device, 64, hipMemcpyHostToDevice )")
            + line(15, "a", "hipMemcpy: Returned hipErrorInvalidValue : private-detail")
        )
        report = trace.build_report(trace.parse_trace(text))
        self.assertEqual(report["transfer_bytes"], {"hipMemcpyDeviceToHost": 128})
        self.assertEqual(sum(api["failed_calls"] for api in report["apis"]), 1)
        rendered = json.dumps(report)
        for token in ("private", "pid:", "tid:", "arguments", "hipErrorInvalidValue"):
            self.assertNotIn(token, rendered)

    # Purpose: Fail closed on incomplete, missing-identity and inconsistent API call streams.
    # Inputs: Malformed or insufficient trace fragments.
    # Outputs: Requires ValueError instead of fabricating a duration or silently dropping an API record.
    def test_invalid_traces(self):
        fragments = (
            "nothing here\n",
            ":3: :0 : 10 us: hipMalloc ( pointer, 1 )\n",
            line(1, "a", "hipMalloc ( pointer, 1 )"),
            line(2, "a", "hipMalloc: Returned hipSuccess :"),
            line(1, "a", "hipMalloc ( pointer, 1 )") + line(3, "a", "hipFree: Returned hipSuccess :"),
            line(3, "a", "hipMalloc ( pointer, 1 )") + line(1, "a", "hipMalloc: Returned hipSuccess :"),
            line(1, "a", "hipMalloc ( private-truncated-arguments")
            + line(2, "a", "hipFree ( pointer )")
            + line(3, "a", "hipFree: Returned hipSuccess :"),
            "x" * (trace.MAX_LINE_CHARS + 1),
        )
        for fragment in fragments:
            with self.subTest(fragment=fragment[:40]), self.assertRaises(ValueError):
                trace.parse_trace(fragment)

    # Purpose: Require explicit complete-call selection when an analysis window cuts API intervals.
    # Inputs: Two complete calls plus invalid or empty timestamp windows.
    # Outputs: Checks selection/exclusion counts and rejects invalid bounds.
    def test_time_window(self):
        calls = [
            trace.TraceCall((1, 1), "hipFree", 10, 20, "pointer", True),
            trace.TraceCall((1, 1), "hipFree", 30, 40, "pointer", True),
        ]
        report = trace.build_report(calls, 21, 41)
        self.assertEqual(report["selected_calls"], 1)
        self.assertEqual(report["excluded_calls"], 1)
        for bounds in ((-1, 40), (10, -1), (20, 20), (40, 20), (11, 20)):
            with self.subTest(bounds=bounds), self.assertRaises(ValueError):
                trace.build_report(calls, *bounds)

    # Purpose: Reject unsupported transfer layouts instead of misreporting byte counts or directions.
    # Inputs: Synchronous-copy records with missing, negative, or unknown fields.
    # Outputs: Requires ValueError and permits unrelated or failed calls without interpreting arguments.
    def test_transfer_argument_validation(self):
        for arguments in ("p, q", "p, q, -1, hipMemcpyDeviceToHost", "p, q, 8, hipMemcpyDefault"):
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                trace.transfer_volume(trace.TraceCall((1, 1), "hipMemcpy", 1, 2, arguments, True))
        self.assertIsNone(trace.transfer_volume(trace.TraceCall((1, 1), "hipFree", 1, 2, "p", True)))

    # Purpose: Enforce the binary read limit before parsing and accept the logger's UTF-8 byte-order mark.
    # Inputs: Temporary valid, oversized, and invalid-UTF-8 files under a small test-specific byte bound.
    # Outputs: Requires exact BOM removal and exceptions for oversized or undecodable input.
    def test_bounded_binary_read(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "trace.log"
            path.write_bytes(b"\xef\xbb\xbfvalid")
            self.assertEqual(trace.read_trace(path), "valid")
            with patch.object(trace, "MAX_TRACE_BYTES", 8):
                path.write_bytes(b"x" * 9)
                with self.assertRaises(ValueError):
                    trace.read_trace(path)
                with self.assertRaises(ValueError):
                    trace.parse_trace("x" * 9)
            path.write_bytes(b"private-detail\xff")
            with self.assertRaises(UnicodeError):
                trace.read_trace(path)

    # Purpose: Keep decoder errors from exposing raw trace bytes, arguments, or a supplied file path.
    # Inputs: A simulated Unicode decoding failure while the command-line analyzer reads a private path.
    # Outputs: Requires a value-free diagnostic and a failing exit code.
    def test_unreadable_trace_cli_error_is_private(self):
        error = UnicodeDecodeError("utf-8", b"private-argument\xff", 16, 17, "private-decoder-detail")
        captured = io.StringIO()
        with (
            patch("sys.argv", ["analyze_hip_trace", "private-path"]),
            patch.object(trace, "read_trace", side_effect=error),
            redirect_stderr(captured),
            self.assertRaises(SystemExit) as exited,
        ):
            trace.main()
        self.assertEqual(exited.exception.code, 1)
        self.assertEqual(captured.getvalue(), "HIP trace analysis failed: unreadable trace\n")


if __name__ == "__main__":
    unittest.main()
