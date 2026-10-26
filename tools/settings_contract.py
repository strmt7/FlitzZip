"""Validate the append-only settings evolution contract without importing the application."""

from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path

from jsonschema import Draft202012Validator

from tools.native_build_provenance import input_path

ROOT = Path(__file__).resolve().parents[1]
ANNOTATIONS = frozenset(("title", "description", "examples", "$comment"))


def validation_signature(value):
    """Purpose: Freeze every validation keyword. Inputs: Schema subtree. Outputs: Rules without prose annotations."""
    if isinstance(value, dict):
        return {key: validation_signature(item) for key, item in value.items() if key not in ANNOTATIONS}
    if isinstance(value, list):
        return [validation_signature(item) for item in value]
    return value


def unique_fields(pairs: list[tuple]) -> dict:
    """Purpose: Reject ambiguous schema declarations. Inputs: JSON object members. Outputs: Unique dictionary."""
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("duplicate settings contract member")
        value[key] = item
    return value


def reject_constant(token: str) -> None:
    """Purpose: Keep evidence valid JSON. Inputs: Nonfinite parser token. Outputs: Always raises ValueError."""
    raise ValueError(f"nonfinite settings contract token: {token}")


def read_contract_json(root: Path, name: str) -> dict:
    """Purpose: Contain evidence reads. Inputs: Checkout and relative regular file. Outputs: Bounded JSON object."""
    path = input_path(root, name)
    with path.open("rb") as stream:
        data = stream.read(256 * 1024 + 1)
    if len(data) > 256 * 1024:
        raise ValueError("settings contract exceeds its evidence budget")
    value = json.loads(data, object_pairs_hook=unique_fields, parse_constant=reject_constant)
    if not isinstance(value, dict):
        raise ValueError("settings contract must be an object")
    return value


def validate_evolution(contract: dict, current: dict, frozen: dict[int, dict]) -> None:
    """Purpose: Enforce stable keys. Inputs: Registry and current/frozen schemas. Outputs: Rejects unsafe drift."""
    Draft202012Validator.check_schema(current)
    generation = contract["currentGeneration"]
    if (
        type(generation) is not int
        or not 1 <= generation <= 65535
        or contract["currentProfile"] != f"settings-v{generation}.json"
    ):
        raise ValueError("profile filename must identify its schema generation")
    properties = current["properties"]
    if properties["schema"] != {"const": f"flitzzip.settings.v{generation}"}:
        raise ValueError("schema identity and generation disagree")
    if set(properties).intersection(contract["retiredKeys"]):
        raise ValueError("retired persisted identifiers must never be reused")
    if current.get("additionalProperties") is not True:
        raise ValueError("unknown extension fields must remain preservable")
    for previous_generation, previous in frozen.items():
        if previous_generation != generation:
            continue
        for name, before in previous["properties"].items():
            after = properties.get(name)
            if after is None or validation_signature(before) != validation_signature(after):
                raise ValueError(f"breaking change to {name} requires a new schema generation and migration")
        before_root = {key: value for key, value in previous.items() if key not in ("properties", "required")}
        after_root = {key: value for key, value in current.items() if key not in ("properties", "required")}
        if validation_signature(before_root) != validation_signature(after_root):
            raise ValueError("changing root validation rules requires a new schema generation")
        if not set(current.get("required", [])).issubset(previous.get("required", [])):
            raise ValueError("adding a required field is a breaking schema change")
    for name, rule in properties.items():
        if name != "schema" and "default" not in rule:
            raise ValueError(f"persisted field has no explicit default: {name}")
        if "default" in rule:
            Draft202012Validator(rule).validate(rule["default"])
    readable = contract["readableGenerations"]
    if any(type(value) is not int for value in readable) or sorted(set(readable)) != list(range(1, generation + 1)):
        raise ValueError("every historical generation must remain readable")
    if set(frozen) != {value for value in readable if value >= 4}:
        raise ValueError("every versioned profile generation needs its immutable baseline schema")
    for version in frozen:
        if contract["frozenSchemas"].get(str(version)) != f"tests/fixtures/settings/schema-v{version}-baseline.json":
            raise ValueError("historical schema evidence must not be redirected")
    fixture_generations = {row["generation"] for row in contract["fixtures"]}
    if fixture_generations != set(readable):
        raise ValueError("every readable generation needs an independent fixture")
    for version in readable:
        visited = set()
        while version != generation:
            if version in visited:
                raise ValueError("migration graph contains a cycle")
            visited.add(version)
            target = contract["migrationEdges"].get(str(version))
            if type(target) is not int or not version < target <= generation:
                raise ValueError("migration graph has a missing or non-forward step")
            version = target
    for group in contract["coupledFields"]:
        if len(group) < 2 or len(set(group)) != len(group) or not set(group).issubset(properties):
            raise ValueError("coupled preference groups must identify distinct current fields")


def assert_fixtures_unchanged(root: Path = ROOT, base: str | None = None) -> None:
    """Purpose: Keep append-only evidence. Inputs: Checkout and trusted Git base. Outputs: Rejects edits/deletions."""
    if base is None:
        candidate = (
            os.environ.get("PR_BASE") if os.environ.get("EVENT_NAME") == "pull_request" else os.environ.get("PUSH_BASE")
        )
        if candidate and (not re.fullmatch(r"[0-9a-f]{40}", candidate) or set(candidate) == {"0"}):
            if os.environ.get("EVENT_NAME") != "push" or set(candidate) != {"0"}:
                raise ValueError("historical settings comparison requires a valid Git base")
            candidate = None
        base = candidate or ("HEAD~1" if os.environ.get("EVENT_NAME") == "workflow_dispatch" else "HEAD")
    changed = subprocess.check_output(
        ["git", "-C", str(root), "diff", "--name-only", "--diff-filter=MD", base, "--", "tests/fixtures/settings"],
        timeout=15,
        text=True,
    ).splitlines()
    if changed:
        raise ValueError("historical settings evidence is append-only; retain original fixtures: " + ", ".join(changed))


def load_contract(root: Path = ROOT) -> tuple[dict, dict, dict[int, dict]]:
    """Purpose: Load local schema evidence. Inputs: Checkout root. Outputs: Registry, schema and frozen baselines."""
    contract = read_contract_json(root, "resources/settings/compatibility.json")
    schema = read_contract_json(root, contract["currentSchema"])
    frozen = {int(version): read_contract_json(root, name) for version, name in contract["frozenSchemas"].items()}
    return contract, schema, frozen
