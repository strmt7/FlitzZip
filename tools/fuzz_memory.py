"""Read Linux RAM counters and verify the local fuzz container's hard limits."""

import argparse
from pathlib import Path

MIB = 1024 * 1024


def available_memory_mib(meminfo: str) -> int:
    """Purpose: Parse available Linux RAM. Inputs: proc meminfo. Outputs: MiB or ValueError."""
    values = [line.split() for line in meminfo.splitlines() if line.startswith("MemAvailable:")]
    if len(values) != 1 or len(values[0]) != 3 or values[0][2] != "kB" or not values[0][1].isascii():
        raise ValueError("Linux MemAvailable counter is missing or malformed")
    counter = values[0][1]
    if not counter.isdecimal() or int(counter) < 1024:
        raise ValueError("Linux MemAvailable counter cannot admit local work")
    return int(counter) // 1024


def verify_limits(root: Path, budget_mib: int) -> None:
    """Purpose: Verify cgroup limits. Inputs: cgroup root and MiB budget. Outputs: None or ValueError/OSError."""
    if budget_mib < 2048:
        raise ValueError("Fuzz RAM budget is below the shared admission minimum")
    expected = budget_mib * MIB
    if (root / "memory.max").is_file():
        memory_file = root / "memory.max"
        swap_file = root / "memory.swap.max"
        expected_swap = 0
    else:
        memory_file = root / "memory/memory.limit_in_bytes"
        swap_file = root / "memory/memory.memsw.limit_in_bytes"
        expected_swap = expected
    if memory_file.read_text(encoding="ascii").strip() != str(expected):
        raise ValueError("Docker did not enforce the requested fuzz RAM limit")
    if swap_file.read_text(encoding="ascii").strip() != str(expected_swap):
        raise ValueError("Docker did not enforce the zero-swap fuzz limit")


def main() -> None:
    """Purpose: Run the resource preflight. Inputs: CLI probe/verify mode. Outputs: Counters or failing exit."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("probe", "verify"))
    parser.add_argument("--budget-mib", type=int)
    args = parser.parse_args()
    if args.mode == "probe":
        if args.budget_mib is not None:
            parser.error("probe does not accept a RAM budget")
        print(available_memory_mib(Path("/proc/meminfo").read_text(encoding="ascii")))
    else:
        if args.budget_mib is None:
            parser.error("verify requires --budget-mib")
        verify_limits(Path("/sys/fs/cgroup"), args.budget_mib)
        print(f"fuzz_memory_limit_bytes={args.budget_mib * MIB} fuzz_swap_limit_bytes=0")


if __name__ == "__main__":
    main()
