"""Build an identified NLTK security revision from immutable upstream source."""

from __future__ import annotations

import base64
import csv
import datetime
import hashlib
import io
import json
import os
import re
import stat
import tempfile
import venv
import zipfile
from email.parser import BytesParser
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOT = ROOT / "third_party/upstream/nltk/1bd574cfd4e3bca82c6b145d9271a646df410707"
BUILD_LOCK = ROOT / "tools/requirements/nltk-build.txt"
MAX_SOURCE_BYTES = 64 * 1024 * 1024


def digest(path: Path) -> str:
    """Purpose: Measure bounded artifacts. Inputs: File. Outputs: SHA-256 or size refusal."""
    if path.stat().st_size > MAX_SOURCE_BYTES:
        raise ValueError("NLTK artifact exceeds its reviewed size budget")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expected_digest(path: Path) -> str:
    """Purpose: Read exact artifact admission. Inputs: SHA-256 sidecar. Outputs: Single digest or refusal."""
    fields = path.read_text(encoding="ascii").split()
    if len(fields) != 2 or not re.fullmatch(r"[a-f0-9]{64}", fields[0]):
        raise ValueError("Malformed NLTK artifact digest")
    return fields[0]


def recipe() -> dict:
    """Purpose: Validate source provenance. Inputs: Repository recipe/archive. Outputs: Verified recipe."""
    data = json.loads((SOURCE_ROOT / "build.json").read_text(encoding="utf-8"))
    if (
        data.get("schema") != 1
        or data.get("project") != "nltk"
        or not re.fullmatch(r"[a-f0-9]{40}", data.get("commit", ""))
        or not re.fullmatch(r"3\.10\.3\+superzip\.security[0-9]+\.g[a-f0-9]+", data.get("version", ""))
        or data.get("source") != "nltk-source.zip"
        or type(data.get("source_date_epoch")) is not int
    ):
        raise ValueError("Unsupported NLTK security build recipe")
    archive = SOURCE_ROOT / data["source"]
    if digest(archive) != expected_digest(archive.with_suffix(".zip.sha256")):
        raise ValueError("NLTK source archive digest mismatch")
    return data


def identity() -> str:
    """Purpose: Reuse exact build artifacts. Inputs: Source/recipe/toolchain/wheel pins. Outputs: Cache identity."""
    from tools.crawl4ai_tool import text_identity

    data = recipe()
    values = [digest(SOURCE_ROOT / data["source"])]
    values.extend(
        text_identity(path)
        for path in (
            SOURCE_ROOT / "build.json",
            SOURCE_ROOT / "nltk-wheel.sha256",
            BUILD_LOCK,
        )
    )
    return hashlib.sha256("\n".join(values).encode("ascii")).hexdigest()


def source_entries(source: zipfile.ZipFile, commit: str) -> list[zipfile.ZipInfo]:
    """Purpose: Admit complete source metadata. Inputs: ZIP/commit. Outputs: Bounded canonical members or refusal."""
    prefix = f"nltk-{commit}/"
    entries = source.infolist()
    if len(entries) > 2000 or sum(item.file_size for item in entries) > MAX_SOURCE_BYTES:
        raise ValueError("NLTK source archive exceeds its reviewed extent")
    seen = set()
    for item in entries:
        name = item.filename
        relative = PurePosixPath(name[len(prefix) :])
        tail = name[len(prefix) :]
        kind = (item.external_attr >> 16) & 0o170000
        if (
            not name.startswith(prefix)
            or name != item.orig_filename
            or relative.is_absolute()
            or ".." in relative.parts
            or "\\" in name
            or ":" in name
            or (tail and relative.as_posix() != tail.rstrip("/"))
            or kind not in (0, stat.S_IFREG, stat.S_IFDIR)
            or (kind == stat.S_IFDIR and not item.is_dir())
            or (kind == stat.S_IFREG and item.is_dir())
            or name.casefold() in seen
        ):
            raise ValueError("Unsafe or duplicate NLTK source member")
        seen.add(name.casefold())
    return entries


def extract_source(archive: Path, destination: Path, commit: str) -> Path:
    """Purpose: Unpack only admitted owned source. Inputs: ZIP/destination/commit. Outputs: Complete source tree."""
    prefix = f"nltk-{commit}/"
    with zipfile.ZipFile(archive) as source:
        for item in source_entries(source, commit):
            relative = PurePosixPath(item.filename[len(prefix) :])
            target = destination.joinpath(*relative.parts)
            if item.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(source.read(item))
    return destination


def runtime_source_sha256() -> str:
    """Purpose: Bind every runtime source member. Inputs: Admitted source/recipe. Outputs: Canonical package digest."""
    data = recipe()
    prefix = f"nltk-{data['commit']}/"
    hashes = {}
    with zipfile.ZipFile(SOURCE_ROOT / data["source"]) as source:
        for item in source_entries(source, data["commit"]):
            name = item.filename[len(prefix) :]
            if name.startswith("nltk/") and name.lower().endswith(".py"):
                hashes[name] = hashlib.sha256(source.read(item).replace(b"\r\n", b"\n")).hexdigest()
        if source.read(prefix + "nltk/VERSION").decode("ascii").strip() != data["upstream_version"]:
            raise ValueError("Unexpected upstream NLTK runtime version")
    if not hashes or "nltk/__init__.py" not in hashes:
        raise ValueError("NLTK runtime source is incomplete")
    hashes["nltk/VERSION"] = hashlib.sha256((data["version"] + "\n").encode("ascii")).hexdigest()
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
                    raise ValueError("NLTK repaired runtime contains a native executable file")
                if path.suffix.lower() == ".pyo" or (
                    path.suffix.lower() == ".pyc" and path.parent.name != "__pycache__"
                ):
                    raise ValueError("NLTK repaired runtime contains unreviewed sourceless bytecode")
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
        raise ValueError("NLTK installed runtime source changed; repaired dependency refused")


def identify_build(source: Path, data: dict) -> None:
    """Purpose: Identify a downstream build honestly. Inputs: Owned upstream tree/recipe. Outputs: Version/notice."""
    version = source / "nltk/VERSION"
    if version.read_text(encoding="ascii").strip() != data["upstream_version"]:
        raise ValueError("Unexpected upstream NLTK version")
    version.write_text(data["version"] + "\n", encoding="ascii", newline="\n")
    notice = (
        "SuperZip development-tool security build\n\n"
        "Upstream: NLTK, https://github.com/nltk/nltk\n"
        f"Exact upstream commit: {data['commit']}\n"
        f"Upstream version file: {data['upstream_version']}\n"
        f"Downstream version: {data['version']}\n\n"
        "This is a separately identified downstream build, not a published NLTK release.\n"
        "Upstream Python runtime source is unchanged. SuperZip changed nltk/VERSION to\n"
        "identify this build and setup.cfg to retain this notice in the wheel.\n"
        "The original Apache 2.0 license, authors and project README remain included.\n"
        "This commit contains the merged source repairs for GHSA-8mgp-746c-j5xp.\n"
    )
    (source / "SUPERZIP_BUILD_NOTICE.txt").write_text(notice, encoding="utf-8", newline="\n")
    config = source / "setup.cfg"
    original = config.read_text(encoding="utf-8")
    if original.count("    LICENSE.txt") != 1:
        raise ValueError("Upstream NLTK license packaging changed")
    config.write_text(
        "# SuperZip change: retain the downstream source/build notice with upstream licenses.\n"
        + original.replace("    LICENSE.txt", "    SUPERZIP_BUILD_NOTICE.txt\n    LICENSE.txt"),
        encoding="utf-8",
        newline="\n",
    )


def canonical_wheel(built: Path, destination: Path, data: dict, *, project: str = "nltk") -> None:
    """Purpose: Make pure wheels reproducible. Inputs: Standard wheel/recipe. Outputs: Canonical standard wheel."""
    with zipfile.ZipFile(built) as original:
        entries = original.infolist()
        if len(entries) > 2000 or sum(item.file_size for item in entries) > MAX_SOURCE_BYTES:
            raise ValueError("NLTK wheel exceeds its reviewed extent")
        names = [item.filename for item in entries]
        if len(names) != len(set(names)):
            raise ValueError("Duplicate NLTK wheel member")
        files = {}
        for name in names:
            if PurePosixPath(name).is_absolute() or ".." in PurePosixPath(name).parts or "\\" in name:
                raise ValueError("Unsafe NLTK wheel member")
            if not name.endswith("/"):
                files[name] = original.read(name)
    info = f"{project}-{data['version']}.dist-info/"
    metadata_path = info + "METADATA"
    metadata = BytesParser().parsebytes(files[metadata_path])
    if metadata["Name"].casefold() != project or metadata["Version"] != data["version"]:
        raise ValueError(f"{project} wheel identity mismatch")
    # Setuptools writes platform line endings; normalize generated metadata only.
    files[metadata_path] = files[metadata_path].replace(b"\r\n", b"\n")
    record_path = info + "RECORD"
    files.pop(record_path)
    record = io.StringIO()
    writer = csv.writer(record, lineterminator="\n")
    for name, contents in sorted(files.items()):
        value = base64.urlsafe_b64encode(hashlib.sha256(contents).digest()).rstrip(b"=").decode("ascii")
        writer.writerow((name, "sha256=" + value, len(contents)))
    writer.writerow((record_path, "", ""))
    files[record_path] = record.getvalue().encode("utf-8")
    stamp = datetime.datetime.fromtimestamp(data["source_date_epoch"], datetime.UTC)
    # Stored members avoid cross-host zlib variation; Python code and license payloads stay byte-exact.
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_STORED) as output:
        for name, contents in sorted(files.items()):
            member = zipfile.ZipInfo(name, stamp.timetuple()[:6])
            member.create_system = 3
            member.external_attr = 0o100644 << 16
            output.writestr(member, contents)


def build_wheel(home: Path, output_directory: Path, data: dict) -> Path:
    """Purpose: Use pinned standard packaging. Inputs: External cache/output/recipe. Outputs: Canonical wheel."""
    from tools.crawl4ai_tool import installation_lock, run, text_identity, tool_environment

    build_home = home / ("nltk-builder-" + text_identity(BUILD_LOCK)[:16])
    python = build_home / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    env = tool_environment(home)
    env.update(SOURCE_DATE_EPOCH=str(data["source_date_epoch"]), PYTHONHASHSEED="0")
    with installation_lock(build_home):
        if not python.exists():
            venv.EnvBuilder(with_pip=True).create(build_home)
        run(
            [
                str(python),
                "-m",
                "pip",
                "install",
                "--quiet",
                "--require-hashes",
                "--only-binary=:all:",
                "-r",
                str(BUILD_LOCK),
            ],
            env,
        )
        run([str(python), "-m", "pip", "check"], env, 60)
        output_directory.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="nltk-source-", dir=output_directory) as owned:
            source = extract_source(SOURCE_ROOT / data["source"], Path(owned) / "source", data["commit"])
            identify_build(source, data)
            built_directory = Path(owned) / "built"
            # --no-deps is confined to wheel construction; runtime installation resolves the full graph normally.
            run(
                [
                    str(python),
                    "-m",
                    "pip",
                    "wheel",
                    "--no-index",
                    "--no-deps",
                    "--no-build-isolation",
                    "--wheel-dir",
                    str(built_directory),
                    str(source),
                ],
                env,
                180,
            )
            filename = f"nltk-{data['version']}-py3-none-any.whl"
            destination = output_directory / filename
            canonical_wheel(built_directory / filename, destination, data)
    return destination


def ensure_wheel(home: Path) -> Path:
    """Purpose: Admit only the reviewed repaired wheel. Inputs: External cache. Outputs: Hash-verified wheel."""
    from tools.crawl4ai_tool import installation_lock

    data = recipe()
    directory = home / ("nltk-wheel-" + identity()[:16])
    wheel = directory / f"nltk-{data['version']}-py3-none-any.whl"
    expected = expected_digest(SOURCE_ROOT / "nltk-wheel.sha256")
    with installation_lock(directory):
        if not wheel.exists():
            wheel = build_wheel(home, directory, data)
        if digest(wheel) != expected:
            raise ValueError("NLTK security wheel digest mismatch; runtime installation refused")
    return wheel
