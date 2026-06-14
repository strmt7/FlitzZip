#!/usr/bin/env python3
"""Resolve SuperZip release metadata for local and GitHub Actions packaging."""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


SEMVER_PATTERN = re.compile(
    r"^(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)"
    r"(?:-((?:0|[1-9][0-9]*|[A-Za-z-][0-9A-Za-z-]*)"
    r"(?:\.(?:0|[1-9][0-9]*|[A-Za-z-][0-9A-Za-z-]*))*))?$"
)
PROJECT_VERSION_PATTERN = re.compile(
    r"project\s*\(\s*SuperZip\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class ReleaseMetadata:
    """Resolved release metadata.

    Inputs: field values are validated release and package strings. Outputs: immutable
    metadata used by GitHub Actions and packaging scripts.
    """

    release_version: str
    release_tag: str
    package_version: str
    package_base: str


def parse_release_version(value: str) -> tuple[int, int, int, str | None]:
    """Purpose: Parse a release version string.
    Inputs: `value` is SemVer without a `v` prefix or build metadata.
    Outputs: `(major, minor, patch, prerelease)` tuple; raises ValueError on invalid input.
    """
    match = SEMVER_PATTERN.fullmatch(value)
    if match is None:
        raise ValueError(
            "release version must be SemVer without a v prefix or build metadata, "
            "for example 0.1.0"
        )
    return int(match.group(1)), int(match.group(2)), int(match.group(3)), match.group(4)


def validate_release_version(value: str) -> str:
    """Purpose: Validate and normalize a release version.
    Inputs: `value` is the requested product version.
    Outputs: validated version string; raises ValueError on unsafe or unsupported values.
    """
    stripped = value.strip()
    if not stripped:
        raise ValueError("release version must not be empty")
    if stripped.lower() == "latest" or stripped.lower().startswith("v"):
        raise ValueError("release version must not be latest and must not use a v prefix")
    parse_release_version(stripped)
    return stripped


def read_project_version(repo_root: Path) -> str:
    """Purpose: Read the default product version from CMake project metadata.
    Inputs: `repo_root` points at the repository root.
    Outputs: version string from `CMakeLists.txt`; raises ValueError when missing.
    """
    cmake = repo_root / "CMakeLists.txt"
    match = PROJECT_VERSION_PATTERN.search(cmake.read_text(encoding="utf-8"))
    if match is None:
        raise ValueError("could not resolve SuperZip project VERSION from CMakeLists.txt")
    return validate_release_version(match.group(1))


def read_existing_tags(path: Path | None) -> set[str]:
    """Purpose: Read existing remote tags from a line-oriented file.
    Inputs: `path` may be None or a file containing tag names.
    Outputs: set of normalized tag strings.
    """
    if path is None:
        return set()
    return {
        line.strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    }


def resolve_metadata(
    *,
    requested_version: str,
    repo_root: Path,
    existing_tags: set[str],
) -> ReleaseMetadata:
    """Purpose: Resolve release, tag, and package names from requested inputs.
    Inputs: requested version, repository root, and current remote tags.
    Outputs: `ReleaseMetadata`; raises ValueError when the resolved tag already exists.
    """
    release_version = (
        validate_release_version(requested_version)
        if requested_version.strip()
        else read_project_version(repo_root)
    )
    release_tag = release_version
    if release_tag in existing_tags or f"v{release_tag}" in existing_tags:
        raise ValueError(
            f"release tag {release_tag} already exists; use replace_existing=true "
            "only with an explicit release_version"
        )
    package_base = f"SuperZip-{release_version}-win64"
    return ReleaseMetadata(
        release_version=release_version,
        release_tag=release_tag,
        package_version=release_version,
        package_base=package_base,
    )


def write_github_env(path: Path, values: dict[str, str]) -> None:
    """Purpose: Append release metadata to a GitHub Actions environment file.
    Inputs: `path` is `$GITHUB_ENV`; `values` maps variable names to safe values.
    Outputs: appends environment assignments to `path`.
    """
    with path.open("a", encoding="utf-8") as env_file:
        for key, value in values.items():
            env_file.write(f"{key}={value}\n")


def write_github_output(path: Path, values: dict[str, str]) -> None:
    """Purpose: Append release metadata to a GitHub Actions output file.
    Inputs: `path` is `$GITHUB_OUTPUT`; `values` maps output names to safe values.
    Outputs: appends output assignments to `path`.
    """
    with path.open("a", encoding="utf-8") as output_file:
        for key, value in values.items():
            output_file.write(f"{key}={value}\n")


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    """Purpose: Parse release metadata command-line arguments.
    Inputs: `argv` is the command argument list without program name.
    Outputs: argparse namespace with validated option structure.
    """
    parser = argparse.ArgumentParser(description="Resolve SuperZip release metadata.")
    parser.add_argument("--requested-version", default="")
    parser.add_argument("--existing-tags-file", type=Path, default=None)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument("--allow-existing", action="store_true")
    parser.add_argument("--json-output", type=Path, default=None)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    """Purpose: Resolve metadata and export it for GitHub Actions.
    Inputs: optional `argv` for tests or command-line execution.
    Outputs: process exit code; writes GitHub env/output files when configured.
    """
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        existing_tags = set() if args.allow_existing else read_existing_tags(args.existing_tags_file)
        metadata = resolve_metadata(
            requested_version=args.requested_version,
            repo_root=args.repo_root,
            existing_tags=existing_tags,
        )
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    env_values = {
        "RELEASE_VERSION": metadata.release_version,
        "RELEASE_TAG": metadata.release_tag,
        "PACKAGE_VERSION": metadata.package_version,
        "PACKAGE_BASE": metadata.package_base,
    }
    output_values = {
        "release_version": metadata.release_version,
        "release_tag": metadata.release_tag,
        "package_version": metadata.package_version,
        "package_base": metadata.package_base,
    }
    if args.json_output is not None:
        args.json_output.write_text(
            json.dumps(output_values, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    if "GITHUB_ENV" in os.environ:
        write_github_env(Path(os.environ["GITHUB_ENV"]), env_values)
    if "GITHUB_OUTPUT" in os.environ:
        write_github_output(Path(os.environ["GITHUB_OUTPUT"]), output_values)

    print(f"Release version: {metadata.release_version}")
    print(f"Release tag: {metadata.release_tag}")
    print(f"Package base: {metadata.package_base}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
