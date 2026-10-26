"""Offline regressions for Linux RAM counters and cgroup containment."""

import io
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

from tools import fuzz_memory as resource
from tools.fuzz_memory import MIB, available_memory_mib, verify_limits


class FuzzMemoryTests(unittest.TestCase):
    """Exercise both supported cgroup layouts without exhausting memory."""

    # Purpose: Qualify mode routing and exact preflight output without reading or changing the host's resource policy.
    # Inputs: Real owned proc/cgroup text fixtures behind a path-selection double and ordinary CLI arguments.
    # Outputs: Probe reports available MiB; verification reports exact hard limits through the real limit validator.
    def test_cli_modes_keep_resource_units_and_hard_limit_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            meminfo = root / "meminfo"
            meminfo.write_text("MemAvailable: 8388608 kB\n", encoding="ascii")
            cgroup = root / "cgroup"
            cgroup.mkdir()
            (cgroup / "memory.max").write_text(str(4096 * MIB), encoding="ascii")
            (cgroup / "memory.swap.max").write_text("0", encoding="ascii")
            selected = {"/proc/meminfo": meminfo, "/sys/fs/cgroup": cgroup}
            for arguments, expected in (
                (["probe"], "8192\n"),
                (["verify", "--budget-mib", "4096"], "fuzz_memory_limit_bytes=4294967296 fuzz_swap_limit_bytes=0\n"),
            ):
                output = io.StringIO()
                with (
                    self.subTest(arguments=arguments),
                    patch.object(sys, "argv", ["fuzz_memory", *arguments]),
                    patch.object(resource, "Path", side_effect=selected.__getitem__),
                    redirect_stdout(output),
                ):
                    resource.main()
                self.assertEqual(output.getvalue(), expected)

    # Purpose: Reject incompatible or missing mode arguments before attempting host resource access.
    # Inputs: Probe with a budget and verify without one, including actual portable Python CLI invocations.
    # Outputs: Usage errors return status two, never report successful limits and never consult proc/cgroup paths.
    def test_cli_argument_rejection_precedes_host_access(self) -> None:
        root = Path(__file__).resolve().parents[1]
        for arguments, diagnostic in ((["probe", "--budget-mib", "4096"], "does not accept"), (["verify"], "requires")):
            with self.subTest(arguments=arguments):
                with (
                    patch.object(sys, "argv", ["fuzz_memory", *arguments]),
                    patch.object(resource, "Path") as selected,
                    redirect_stderr(io.StringIO()),
                    self.assertRaises(SystemExit) as failure,
                ):
                    resource.main()
                self.assertEqual(failure.exception.code, 2)
                selected.assert_not_called()
                result = subprocess.run(
                    [sys.executable, "-m", "tools.fuzz_memory", *arguments],
                    cwd=root,
                    capture_output=True,
                    text=True,
                    timeout=30,
                    check=False,
                )
                self.assertEqual((result.returncode, result.stdout), (2, ""))
                self.assertIn(diagnostic, result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    def test_available_memory(self) -> None:
        """Purpose: Validate counter units. Inputs: Synthetic meminfo. Outputs: Exact MiB."""
        self.assertEqual(available_memory_mib("MemTotal: 999999 kB\nMemAvailable: 8389631 kB\n"), 8192)

    def test_invalid_counters(self) -> None:
        """Purpose: Reject missing/corrupt counters. Inputs: Invalid meminfo. Outputs: ValueError."""
        for text in (
            "",
            "MemFree: 8388608 kB\n",
            "MemAvailable: 0 kB",
            "MemAvailable: -100 kB",
            "MemAvailable: 8388608 MB",
            "MemAvailable: 8388608",
            "MemAvailable: +8388608 kB",
            "MemAvailable: 8388608 kB\nMemAvailable: 8388608 kB",
            "MemAvailable: \u0661\u0662\u0663\u0664 kB",
        ):
            with self.subTest(text=text), self.assertRaises(ValueError):
                available_memory_mib(text)

    def test_cgroup_limits(self) -> None:
        """Purpose: Verify hard limits. Inputs: v1/v2 fixtures and deviations. Outputs: Strict acceptance/rejection."""
        for version in (1, 2):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                if version == 2:
                    memory, swap = root / "memory.max", root / "memory.swap.max"
                else:
                    (root / "memory").mkdir()
                    memory = root / "memory/memory.limit_in_bytes"
                    swap = root / "memory/memory.memsw.limit_in_bytes"
                expected = 4096 * MIB
                memory.write_text(f"{expected}\n", encoding="ascii")
                swap.write_text(f"{0 if version == 2 else expected}\n", encoding="ascii")
                verify_limits(root, 4096)
                for invalid in ("max", "0", str(expected - 1), str(expected + 1)):
                    memory.write_text(invalid, encoding="ascii")
                    with self.assertRaisesRegex(ValueError, "RAM limit"):
                        verify_limits(root, 4096)
                memory.write_text(str(expected), encoding="ascii")
                swap.write_text(str(expected + 1), encoding="ascii")
                with self.assertRaisesRegex(ValueError, "zero-swap"):
                    verify_limits(root, 4096)
                swap.unlink()
                with self.assertRaises(OSError):
                    verify_limits(root, 4096)

    def test_missing_and_low_limits(self) -> None:
        """Purpose: Fail closed before work. Inputs: Missing cgroups/low budget. Outputs: Errors, not fallback."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(OSError):
                verify_limits(root, 4096)
            for budget in (-1, 0, 2047):
                with self.subTest(budget=budget), self.assertRaisesRegex(ValueError, "minimum"):
                    verify_limits(root, budget)


if __name__ == "__main__":
    unittest.main()
