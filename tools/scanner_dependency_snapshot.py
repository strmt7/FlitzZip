"""Report the verified installed scanner graph through GitHub's dependency submission API."""

from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
import os
import re
from datetime import UTC, datetime
from pathlib import Path
from urllib.parse import quote

from tools.python_wheel import read_recipe
from tools.scanner_requirements import locked_requirements

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ".github/requirements/requirements-semgrep-linux.txt"
RECIPE = ".github/requirements/semgrep-distribution.json"


def source_hash(path: Path) -> str:
    """Purpose: Bind portable source bytes. Inputs: Owned source file. Outputs: Normalized SHA-256."""
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def package_url(name: str, version: str) -> str:
    """Purpose: Preserve exact distribution identity. Inputs: Admitted name/version. Outputs: Escaped PyPI PURL."""
    return f"pkg:pypi/{quote(name, safe='')}@{quote(version, safe='')}"


def context(environment: dict[str, str]) -> dict:
    """Purpose: Bind publication to its owned Actions run.
    Inputs: Process environment.
    Outputs: Validated snapshot fields.
    """
    values = {
        name: environment.get("GITHUB_" + name, "") for name in ("SHA", "REF", "REPOSITORY", "RUN_ID", "RUN_ATTEMPT")
    }
    if (
        not re.fullmatch(r"[a-f0-9]{40}", values["SHA"])
        or not re.fullmatch(r"refs/(?:heads|tags)/[A-Za-z0-9._/-]+", values["REF"])
        or not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", values["REPOSITORY"])
        or not re.fullmatch(r"[1-9][0-9]*", values["RUN_ID"])
        or not re.fullmatch(r"[1-9][0-9]*", values["RUN_ATTEMPT"])
    ):
        raise ValueError("Scanner dependency snapshot requires an exact Actions source/run identity")
    return {
        "version": 0,
        "sha": values["SHA"],
        "ref": values["REF"],
        "job": {
            "correlator": "security_semgrep",
            "id": values["RUN_ID"] + "/" + values["RUN_ATTEMPT"],
            "html_url": f"https://github.com/{values['REPOSITORY']}/actions/runs/{values['RUN_ID']}",
        },
        "detector": {
            "name": "flitzzip-installed-scanner",
            "version": source_hash(Path(__file__)),
            "url": f"https://github.com/{values['REPOSITORY']}",
        },
        "metadata": {"lock_sha256": source_hash(ROOT / MANIFEST), "distribution_sha256": source_hash(ROOT / RECIPE)},
    }


def installed_graph(pins: dict) -> dict:
    """Purpose: Observe every locked distribution and active metadata edge. Inputs: Full lock. Outputs: Verified graph.
    Runs in the normally resolved scanner environment; package modules are never imported to inspect their metadata.
    """
    from packaging.requirements import Requirement
    from packaging.utils import canonicalize_name

    if not pins:
        raise ValueError("Scanner dependency graph cannot be empty")
    distributions = {}
    active_extras = {name: set(record[1]) for name, record in pins.items()}
    edges = {name: set() for name in pins}
    for name, (version, _extras) in pins.items():
        distribution = importlib.metadata.distribution(name)
        if canonicalize_name(distribution.metadata["Name"]) != name or distribution.version != version:
            raise ValueError("Installed scanner distribution disagrees with its lock")
        distributions[name] = distribution
    pending = list(pins)
    while pending:
        name = pending.pop()
        for text in distributions[name].requires or ():
            requirement = Requirement(text)
            if requirement.marker and not any(
                requirement.marker.evaluate({"extra": extra}) for extra in ("", *active_extras[name])
            ):
                continue
            child = canonicalize_name(requirement.name)
            if child not in pins or pins[child][0] not in requirement.specifier or requirement.url:
                raise ValueError(
                    f"Installed scanner dependency edge is missing or conflicts with its lock: {name} -> {child}"
                )
            if not requirement.extras.issubset(active_extras[child]):
                active_extras[child].update(requirement.extras)
                pending.append(child)
            edges[name].add(package_url(child, pins[child][0]))
    # Each entry is explicitly requested by this flat, complete installation lock.
    return {
        package_url(name, pin[0]): {
            "package_url": package_url(name, pin[0]),
            "scope": "development",
            "relationship": "direct",
            "dependencies": sorted(edges[name]),
        }
        for name, pin in pins.items()
    }


def validate_snapshot(snapshot: dict, environment: dict[str, str]) -> None:
    """Purpose: Refuse stale, partial or redirected publication.
    Inputs: Owned artifact/current run.
    Outputs: Admission or error.
    """
    expected = context(environment)
    if (
        not isinstance(snapshot, dict)
        or set(snapshot) != {*expected, "scanned", "manifests"}
        or type(snapshot["version"]) is not int
    ):
        raise ValueError("Scanner dependency snapshot fields differ from the publication contract")
    if any(snapshot[key] != value for key, value in expected.items()):
        raise ValueError("Scanner dependency snapshot source, recipe or run changed")
    if not isinstance(snapshot["scanned"], str) or not re.fullmatch(
        r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", snapshot["scanned"]
    ):
        raise ValueError("Scanner dependency snapshot timestamp is malformed")
    datetime.strptime(snapshot["scanned"], "%Y-%m-%dT%H:%M:%SZ")
    manifests = snapshot["manifests"]
    if not isinstance(manifests, dict) or set(manifests) != {MANIFEST}:
        raise ValueError("Scanner dependency snapshot manifest scope changed")
    manifest = manifests[MANIFEST]
    if (
        not isinstance(manifest, dict)
        or set(manifest) != {"name", "file", "resolved"}
        or manifest["name"] != MANIFEST
        or manifest["file"] != {"source_location": MANIFEST}
    ):
        raise ValueError("Scanner dependency snapshot manifest identity changed")
    pins = locked_requirements((ROOT / MANIFEST).read_text(encoding="utf-8"))
    expected_urls = {package_url(name, pin[0]) for name, pin in pins.items()}
    graph = manifest["resolved"]
    if not expected_urls or not isinstance(graph, dict) or set(graph) != expected_urls:
        raise ValueError("Scanner dependency snapshot omits or adds locked distributions")
    for purl, record in graph.items():
        if (
            not isinstance(record, dict)
            or set(record) != {"package_url", "scope", "relationship", "dependencies"}
            or record["package_url"] != purl
            or record["scope"] != "development"
            or record["relationship"] != "direct"
            or not isinstance(record["dependencies"], list)
            or any(not isinstance(child, str) or child not in graph for child in record["dependencies"])
            or record["dependencies"] != sorted(set(record["dependencies"]))
        ):
            raise ValueError("Scanner dependency snapshot has an invalid package or edge")


def main() -> None:
    """Purpose: Produce or admit a source-bound local snapshot.
    Inputs: CLI/run environment.
    Outputs: JSON; never sends credentials.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("build", "validate"))
    parser.add_argument("path", type=Path)
    args = parser.parse_args()
    if args.command == "build":
        pins = locked_requirements((ROOT / MANIFEST).read_text(encoding="utf-8"))
        snapshot = context(os.environ) | {
            "scanned": datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "manifests": {
                MANIFEST: {"name": MANIFEST, "file": {"source_location": MANIFEST}, "resolved": installed_graph(pins)}
            },
        }
        validate_snapshot(snapshot, os.environ)
        with args.path.open("x", encoding="utf-8") as stream:
            json.dump(snapshot, stream, indent=2)
    else:
        if args.path.stat().st_size > 1024 * 1024:
            raise ValueError("Scanner dependency snapshot exceeds its byte budget")
        validate_snapshot(read_recipe(args.path), os.environ)


if __name__ == "__main__":
    main()
