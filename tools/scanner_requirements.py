"""Canonical admission for the scanners' normal hash-locked requirements."""

import re
import shlex

EXTRAS_PATTERN = r"(?:\[[a-z0-9][a-z0-9_-]*(?:,[a-z0-9][a-z0-9_-]*)*\])?"


# Purpose: Admit only the scanner's single official index, binary policy and bounded package/hash tokens.
# Inputs: Tracked requirements input or lock text. Outputs: Returns normally or raises ValueError on unknown syntax.
def validate_requirements_options(contents: str) -> None:
    tokens = shlex.split(contents.replace("\\\n", ""), comments=True)
    options = {}
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in ("--index-url", "--only-binary"):
            if index + 1 == len(tokens):
                raise ValueError("Missing requirements option value")
            name, value = token, tokens[index + 1]
            index += 2
        elif token == "--only-binary=:all:":
            name, value = "--only-binary", ":all:"
            index += 1
        elif re.fullmatch(r"--hash=sha256:[a-f0-9]{64}", token) or re.fullmatch(
            rf"[a-z][a-z0-9-]*{EXTRAS_PATTERN}(?:==|>=)[0-9][a-zA-Z0-9.+-]*", token
        ):
            index += 1
            continue
        else:
            raise ValueError("Unsupported scanner requirements syntax")
        if name in options:
            raise ValueError("Duplicate requirements option")
        options[name] = value
    if options != {"--index-url": "https://pypi.org/simple", "--only-binary": ":all:"}:
        raise ValueError("Scanner requirements need the official index and binary-only policy")


# Purpose: Validate all locked distributions, including normal requirement extras.
# Inputs: Hash-locked requirements text. Outputs: Unique base package names; malformed or unhashed entries fail.
def validate_locked_packages(contents: str) -> set[str]:
    return set(locked_requirements(contents))


# Purpose: Admit complete pinned package identities without introducing a second requirements parser.
# Inputs: Canonical hash-locked requirements text. Outputs: Unique base names mapped to versions and requested extras.
def locked_requirements(contents: str) -> dict[str, tuple[str, tuple[str, ...]]]:
    validate_requirements_options(contents)
    tokens = shlex.split(contents.replace("\\\n", ""), comments=True)
    if tokens[:4] != ["--index-url", "https://pypi.org/simple", "--only-binary", ":all:"]:
        raise ValueError("The lock must retain its canonical installation policy")
    packages: dict[str, tuple[str, tuple[str, ...]]] = {}
    index = 4
    while index < len(tokens):
        package = re.fullmatch(rf"([a-z0-9-]+){EXTRAS_PATTERN}==([0-9][a-zA-Z0-9.+-]*)", tokens[index])
        if package is None or package[1] in packages:
            raise ValueError("Invalid or duplicate locked distribution")
        requirement = tokens[index].partition("==")[0]
        extras = tuple(requirement.partition("[")[2].rstrip("]").split(",")) if "[" in requirement else ()
        packages[package[1]] = (package[2], extras)
        index += 1
        start = index
        while index < len(tokens) and re.fullmatch(r"--hash=sha256:[a-f0-9]{64}", tokens[index]):
            index += 1
        if index == start:
            raise ValueError("Every locked distribution must retain its artifact hashes")
    return packages
