"""Finalize the reviewed artifact identities after target-specific Semgrep resolution."""

import argparse
import re
import shlex
from pathlib import Path

from tools.atomic_file import atomic_write_bytes
from tools.scanner_requirements import locked_requirements
from tools.semgrep_distribution import recipe

ROOT = Path(__file__).resolve().parents[1]
MAX_LOCK_BYTES = 2 * 1024 * 1024


def bind_artifact_hashes(contents: str) -> str:
    """Canonicalize only the unique Semgrep stanza using the reviewed manifest.

    Input is an admitted target-specific LF lock containing at least one current
    artifact hash. Foreign hashes, package/version drift and malformed locks fail;
    every other package, hash and byte outside this stanza remains unchanged.
    """
    packages = locked_requirements(contents)
    data = recipe()
    version = data["version"]
    if packages.get("semgrep") != (version, ()):
        raise ValueError("Semgrep lock does not match the reviewed distribution")
    pattern = (
        rf"(?m)^semgrep=={re.escape(version)}(?=[ \t]|$)[^\n]*"
        r"(?:\n[ \t]+--hash=sha256:[a-f0-9]{64}[^\n]*)*"
    )
    matches = list(re.finditer(pattern, contents))
    if len(matches) != 1:
        raise ValueError("Semgrep lock needs one supported artifact stanza")
    stanza = matches[0]
    tokens = shlex.split(stanza.group().replace("\\\n", ""), comments=True)
    hashes = [token.removeprefix("--hash=sha256:") for token in tokens[1:]]
    reviewed = sorted({pin["derived_sha256"] for pin in data["wheels"].values()})
    if not hashes or len(set(hashes)) != len(hashes) or not set(hashes) <= set(reviewed):
        raise ValueError("Semgrep lock contains unreviewed or duplicate artifact hashes")
    lines = [f"semgrep=={version} \\"]
    lines.extend(
        f"    --hash=sha256:{digest}" + (" \\" if index + 1 < len(reviewed) else "")
        for index, digest in enumerate(reviewed)
    )
    result = contents[: stanza.start()] + "\n".join(lines) + contents[stanza.end() :]
    if locked_requirements(result) != packages:
        raise ValueError("Artifact binding changed locked package identities")
    return result


def read_lock_bytes(path: Path) -> bytes:
    """Read bounded regular source input; reject a symbolic-link leaf or oversized file."""
    if path.is_symlink() or not path.is_file():
        raise ValueError("Semgrep lock must be a regular source file")
    with path.open("rb") as source:
        contents = source.read(MAX_LOCK_BYTES + 1)
    if len(contents) > MAX_LOCK_BYTES:
        raise ValueError("Semgrep lock exceeds its byte budget")
    return contents


def finalize_lock(path: Path, *, check_only: bool = False) -> bool:
    """Bind a serialized caller-owned lock or check it without mutation.

    Preserve the prior file on validation/write failure and refuse observed input
    changes. Callers serialize writes; this is not a concurrent compare-and-swap.
    """
    before = read_lock_bytes(path)
    result = bind_artifact_hashes(before.decode("utf-8").replace("\r\n", "\n")).encode("utf-8")
    if result == before:
        return False
    if check_only:
        raise ValueError("Semgrep lock needs reviewed artifact binding")
    if read_lock_bytes(path) != before:
        raise ValueError("Semgrep lock changed during artifact binding")
    atomic_write_bytes(path, result)
    return True


def main() -> int:
    """Finalize this checkout's canonical scanner lock, independently of host OS."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Reject a stale binding without writing the lock")
    args = parser.parse_args()
    changed = finalize_lock(ROOT / ".github/requirements/requirements-semgrep-linux.txt", check_only=args.check)
    print("Semgrep artifact binding updated." if changed else "Semgrep artifact binding is current.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
