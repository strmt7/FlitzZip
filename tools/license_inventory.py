"""Validate license-inventory coverage, not legal eligibility or release compliance."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parent.parent
NATIVE = "resources/licenses/license-notices.json"
DEVELOPMENT = "docs/licenses/development-notices.json"


def notice_source(root: Path, name: str) -> Path:
    """Purpose: Admit a local notice source. Inputs: Root/relative path. Outputs: Existing regular source or error."""
    relative = PurePosixPath(name)
    if relative.is_absolute() or ".." in relative.parts or "\\" in name or ":" in name:
        raise ValueError("Notice path must be a relative repository file")
    path = root / relative
    for parent in (path, *path.parents):
        if parent == root:
            break
        if parent.is_symlink() or (parent.exists() and getattr(parent.stat(), "st_file_attributes", 0) & 0x400):
            raise ValueError("Notice path contains a reparse point")
    if not path.is_file() or not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"Notice source is absent or outside the repository: {name}")
    return path


def audit(root: Path = ROOT) -> dict:
    """Purpose: Detect missing notices. Inputs: Repository root. Outputs: Coverage result or explicit error."""
    native = json.loads(notice_source(root, NATIVE).read_text(encoding="utf-8"))["notices"]
    development = json.loads(notice_source(root, DEVELOPMENT).read_text(encoding="utf-8"))["notices"]
    covered = set()
    for row in native:
        source = row["source"]
        notice_source(root, source)
        parts = PurePosixPath(source).parts
        if len(parts) >= 3 and parts[0] == "third_party":
            covered.add(parts[1])
    present = {path.name for path in (root / "third_party").iterdir() if path.is_dir()}
    unreviewed = present - covered - {"upstream", "notices"}
    if unreviewed:
        raise ValueError(f"Unreviewed vendored dependency roots: {', '.join(sorted(unreviewed))}")
    sources = set()
    for row in development:
        source = row["source"]
        if source in sources or not row.get("upstream", "").startswith("https://") or not row.get("terms"):
            raise ValueError("Development notices need unique sources and explicit upstream terms")
        sources.add(source)
        actual = hashlib.sha256(notice_source(root, source).read_bytes()).hexdigest()
        if actual != row.get("sha256"):
            raise ValueError(f"Development notice differs from its reviewed original: {source}")
    for path in (root / "third_party/notices").iterdir():
        if path.is_file() and path.relative_to(root).as_posix() not in sources:
            raise ValueError(f"Unreviewed adapted-tool notice: {path.name}")
    return {
        "coverage_passed": True,
        "native_dependency_roots": sorted(covered),
        "native_notices": len(native),
        "development_notices": len(development),
        "legal_clearance_claimed": False,
        "release_source_and_rebuild_verified": False,
    }


if __name__ == "__main__":
    print(json.dumps(audit(), indent=2))
