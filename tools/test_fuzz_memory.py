"""Offline regressions for Linux RAM counters and cgroup containment."""

import tempfile
import unittest
from pathlib import Path

from tools.fuzz_memory import MIB, available_memory_mib, verify_limits


class FuzzMemoryTests(unittest.TestCase):
    """Exercise both supported cgroup layouts without exhausting memory."""

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
