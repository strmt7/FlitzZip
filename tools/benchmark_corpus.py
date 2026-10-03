"""Admit one exact RAM benchmark input through the shared permission manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from tools.benchmark_comparators import PERMISSIONS_PATH
from tools.run_archive_comparison import verify_manifest


# Purpose: Hash corpus admission metadata with a hard read ceiling even if its pathname changes.
# Inputs: Manifest or permission catalog path; metadata must fit the canonical one-MiB JSON boundary.
# Outputs: Lowercase SHA-256 or ValueError on excess bytes; allocation and I/O stay bounded.
def metadata_digest(path: Path) -> str:
    with path.open("rb") as stream:
        payload = stream.read(1024 * 1024 + 1)
    if not payload or len(payload) > 1024 * 1024:
        raise ValueError("corpus admission metadata must contain 1..1048576 bytes")
    return hashlib.sha256(payload).hexdigest()


# Purpose: Freeze the selected source and permission metadata around shared manifest admission.
# Inputs: Reviewed manifest, flat corpus directory and one exact manifest filename.
# Outputs: Private source path and public provenance; any admission or concurrent metadata change raises.
def inspect_corpus(manifest: Path, directory: Path, filename: str) -> dict:
    before = (metadata_digest(manifest), metadata_digest(PERMISSIONS_PATH))
    files, _, metadata = verify_manifest(manifest, directory)
    selected = next((row for row in files if row["name"] == filename), None)
    if selected is None:
        raise ValueError("selected corpus file is absent from the reviewed manifest")
    if before != (metadata_digest(manifest), metadata_digest(PERMISSIONS_PATH)):
        raise ValueError("corpus manifest or permission catalog changed during admission")
    return {
        "path": str((directory / filename).absolute()),
        "manifest_path": str(manifest.absolute()),
        "catalog_path": str(PERMISSIONS_PATH.absolute()),
        "input_bytes": selected["bytes"],
        "source_sha256": selected["sha256"],
        "provenance": {
            "name": metadata["name"],
            "manifest_sha256": before[0],
            "permission_catalog_sha256": before[1],
            "file": selected,
        },
    }


# Purpose: Return bounded admission metadata without running compression or acquiring inputs.
# Inputs: Explicit manifest/root/file arguments supplied by the PowerShell benchmark controller.
# Outputs: One JSON object on success; permission, identity or schema errors propagate as a nonzero exit.
def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--file", required=True)
    args = parser.parse_args()
    print(json.dumps(inspect_corpus(args.manifest, args.root, args.file), ensure_ascii=True))


if __name__ == "__main__":
    main()
