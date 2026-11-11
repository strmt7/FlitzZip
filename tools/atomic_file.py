"""Publish complete tool-owned files without modifying their previous bytes in place."""

import os
import tempfile
from pathlib import Path


def atomic_write_bytes(target: Path, content: bytes) -> None:
    """Replace a caller-admitted destination after a complete, flushed temporary write.

    The caller serializes destination updates. This function owns only its new
    temporary file in the same directory. Write, flush and replacement failures
    preserve the previous destination; cleanup errors propagate.
    """
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=target.parent, prefix=".atomic-", delete=False) as output:
            temporary = Path(output.name)
            if output.write(content) != len(content):
                raise OSError("Incomplete atomic file write")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, target)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
