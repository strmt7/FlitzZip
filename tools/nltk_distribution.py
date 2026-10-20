"""Admit the unmodified official NLTK wheel and verify its installed runtime."""

from __future__ import annotations

import hashlib
import json
import os
import re
import stat
from email.parser import BytesParser
from pathlib import Path

from tools.python_wheel import MAX_SOURCE_BYTES, digest, wheel_files

ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOT = ROOT / "third_party/upstream/nltk/3.10.4-rc1"


def distribution() -> dict:
    """Purpose: Validate exact upstream provenance. Inputs: Tracked record and wheel. Outputs: Admitted metadata."""
    data = json.loads((SOURCE_ROOT / "distribution.json").read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not isinstance(data.get("version"), str) or not isinstance(data.get("tag"), str):
        raise ValueError("Unsupported NLTK upstream distribution record")
    version = data.get("version", "")
    filename = f"nltk-{version}-py3-none-any.whl"
    tag = data.get("tag", "")
    if (
        type(data.get("schema")) is not int
        or data["schema"] != 1
        or data.get("project") != "nltk"
        or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version)
        or not re.fullmatch(re.escape("v" + version) + r"(?:-rc[1-9][0-9]*)?", tag)
        or data.get("filename") != filename
        or data.get("release_url") != f"https://github.com/nltk/nltk/releases/tag/{tag}"
        or data.get("url") != f"https://github.com/nltk/nltk/releases/download/{tag}/{filename}"
        or data.get("release_kind") != ("release_candidate" if "-rc" in tag else "stable")
        or data.get("license") != "Apache-2.0"
        or data.get("license_member") != f"nltk-{version}.dist-info/licenses/LICENSE.txt"
        or not isinstance(data.get("sha256"), str)
        or not re.fullmatch(r"[a-f0-9]{64}", data.get("sha256", ""))
        or type(data.get("size")) is not int
        or not 0 < data["size"] <= MAX_SOURCE_BYTES
    ):
        raise ValueError("Unsupported NLTK upstream distribution record")
    wheel = SOURCE_ROOT / filename
    if wheel.stat().st_size != data["size"] or digest(wheel) != data["sha256"]:
        raise ValueError("NLTK upstream wheel digest or size mismatch")
    return data


def wheel_path() -> Path:
    """Purpose: Install exact upstream bytes without rebuilding. Inputs: Tracked provenance. Outputs: Verified wheel."""
    data = distribution()
    path = SOURCE_ROOT / data["filename"]
    files = wheel_files(path)
    prefix = f"nltk-{data['version']}.dist-info/"
    metadata = BytesParser().parsebytes(files[prefix + "METADATA"])
    wheel = BytesParser().parsebytes(files[prefix + "WHEEL"])
    if (
        metadata["Name"] != "nltk"
        or metadata["Version"] != data["version"]
        or files["nltk/VERSION"].decode("ascii").strip() != data["version"]
        or wheel["Root-Is-Purelib"] != "true"
        or wheel.get_all("Tag") != ["py3-none-any"]
        or len(files[prefix + "licenses/LICENSE.txt"]) < 100
        or len(files[prefix + "licenses/AUTHORS.md"]) < 100
    ):
        raise ValueError("NLTK wheel metadata, runtime or original notices disagree with provenance")
    return path


def identity() -> str:
    """Purpose: Reuse only the admitted distribution. Inputs: Record and verified wheel. Outputs: Cache identity."""
    from tools.crawl4ai_tool import text_identity

    data = distribution()
    return hashlib.sha256(
        (text_identity(SOURCE_ROOT / "distribution.json") + data["sha256"]).encode("ascii")
    ).hexdigest()


def runtime_source_sha256() -> str:
    """Purpose: Bind installed runtime to upstream bytes. Inputs: Official wheel. Outputs: Canonical source digest."""
    files = wheel_files(wheel_path())
    hashes = {
        name: hashlib.sha256(contents.replace(b"\r\n", b"\n")).hexdigest()
        for name, contents in files.items()
        if name.startswith("nltk/") and (name.lower().endswith(".py") or name == "nltk/VERSION")
    }
    if "nltk/__init__.py" not in hashes or "nltk/VERSION" not in hashes:
        raise ValueError("NLTK runtime source is incomplete")
    return hashlib.sha256(json.dumps(hashes, sort_keys=True, separators=(",", ":")).encode("ascii")).hexdigest()


def installed_runtime_sha256(root: Path) -> str:
    """Purpose: Measure installed source without importing it. Inputs: Package directory. Outputs: Bounded digest."""
    root_info = root.lstat()
    if (
        not stat.S_ISDIR(root_info.st_mode)
        or getattr(root_info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
    ):
        raise ValueError("NLTK installed runtime is missing or redirected")
    hashes = {}
    total = entries = 0
    pending = [root]
    while pending:
        with os.scandir(pending.pop()) as children:
            for child in children:
                entries += 1
                info = child.stat(follow_symlinks=False)
                if (
                    entries > 10000
                    or stat.S_ISLNK(info.st_mode)
                    or (getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT)
                ):
                    raise ValueError("NLTK installed runtime exceeds admission or contains a redirect")
                path = Path(child.path)
                if stat.S_ISDIR(info.st_mode):
                    pending.append(path)
                    continue
                name = path.relative_to(root).as_posix()
                if not stat.S_ISREG(info.st_mode):
                    raise ValueError("NLTK installed runtime contains a special file")
                if path.suffix.lower() in (".pyd", ".so", ".dll", ".dylib"):
                    raise ValueError("NLTK admitted runtime contains a native executable file")
                if path.suffix.lower() == ".pyo" or (
                    path.suffix.lower() == ".pyc" and path.parent.name != "__pycache__"
                ):
                    raise ValueError("NLTK admitted runtime contains unreviewed sourceless bytecode")
                if path.suffix.lower() != ".py" and name != "VERSION":
                    continue
                with path.open("rb") as stream:
                    contents = stream.read(MAX_SOURCE_BYTES - total + 1)
                total += len(contents)
                if total > MAX_SOURCE_BYTES:
                    raise ValueError("NLTK installed runtime exceeds its source byte budget")
                hashes["nltk/" + name] = hashlib.sha256(contents.replace(b"\r\n", b"\n")).hexdigest()
    return hashlib.sha256(json.dumps(hashes, sort_keys=True, separators=(",", ":")).encode("ascii")).hexdigest()


def verify_installed_runtime(expected_sha256: str) -> None:
    """Purpose: Refuse cache drift before code import. Inputs: Admitted digest. Outputs: Success or source refusal."""
    import importlib.metadata

    if not re.fullmatch(r"[a-f0-9]{64}", expected_sha256):
        raise ValueError("Malformed NLTK runtime source identity")
    root = Path(importlib.metadata.distribution("nltk").locate_file("nltk"))
    if installed_runtime_sha256(root) != expected_sha256:
        raise ValueError("NLTK installed runtime source changed; upstream dependency refused")
