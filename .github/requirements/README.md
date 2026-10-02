# Python Tooling Locks

The `.in` files declare direct tools and advisory minimum versions. The `.txt`
files contain the complete, hash-locked dependency graph used by CI. Update the
inputs and resolve the graph together. Do not silently force a transitive version
outside its parent's declared requirements. All installed tools and dependencies
are pinned to official PyPI artifacts; no downstream wheel transformation is
needed for the current graph.

## Refresh

The October 2, 2026 Semgrep refresh used uv 0.12.18. Resolve Linux tools for the Ubuntu
24.04 x64 / CPython 3.14 runner, and Windows linters for the oldest supported
local interpreter, CPython 3.12. Also validate the Windows lock against CI's
CPython 3.14. Run from the repository root:

```powershell
foreach ($lane in 'semgrep', 'gvm-tools', 'zizmor', 'lint') {
    $platform = 'x86_64-manylinux_2_39'
    $python = '3.14'
    $suffix = 'linux'
    if ($lane -eq 'lint') {
        $platform = 'x86_64-pc-windows-msvc'
        $python = '3.12'
        $suffix = 'windows'
    }
    $stem = ".github/requirements/requirements-$lane-$suffix"
    uv pip compile "$stem.in" --output-file "$stem.txt" `
        --python-version $python --python-platform $platform `
        --generate-hashes --emit-index-url --emit-build-options `
        --no-annotate --no-header --upgrade --no-python-downloads
    if ($LASTEXITCODE -ne 0) { throw "Resolution failed: $lane" }
}
```

Validate installation with `pip --require-hashes --only-binary=:all:` on the
target OS, run `pip check`, and exercise each tool before publishing. Cross-OS
`pip download` checks wheel availability and hashes only when used with
`--no-deps`: pip still evaluates dependency environment markers against the
host. Use uv's explicit target platform for dependency resolution, and hosted
Linux execution for the final runtime check. Never count a Windows-only marker
failure as evidence that the Linux dependency graph is broken.

## Compatibility Review

Semgrep 1.179.0's official `pyjwt[crypto]>=2.15.0,<3` declaration supports patched
PyJWT 2.15.1. The complete graph resolves for the Linux CPython 3.14 target.
Normal wheel-only hash-locked installation, `pip check` and all five runtime
tests pass on Linux CPython 3.14.7, including the actual MCP JWT verifier,
signed/invalid token controls, the pre-verification parser regression and
CLI/SARIF controls. The previous reviewed local metadata revision is retired:
its wheel builder, manifest, local version and `--find-links` are removed.

Offline tests enforce official input/lock parity, every dependency's SHA-256
pin and the actual workflow's normal installation and coverage controls. CI
retains the runtime tests, full scanner and coverage audit. See
[the scanning guide](../../docs/security-code-scanning.md#scanner-dependency-remediation)
for the earlier compatibility evidence and remaining coverage limits. Dependabot
can monitor the official Semgrep pin normally again. Local success is not evidence
that a newly pushed workflow or GitHub alert has passed its acceptance gate.

Semgrep 1.179.0 still requires MCP exactly 1.29.0. This is above the
patched minimum for the three MCP Dependabot advisories; upgrading to MCP 2.x
would violate Semgrep's contract. The SDK belongs to scanner tooling, not
SuperZip's native application runtime.

This release also requires `exceptiongroup~=1.2.0`,
`jsonschema~=4.25.1`, `wcmatch~=8.3`, OpenTelemetry 1.37 / 0.58b0, and indirectly
`wrapt<2` and `importlib-metadata<8.8.0`. Consequently PRs 26, 29, 30, 31,
32, 33, and 34 cannot be applied independently. Keep these constraints until
the parent packages support newer versions; do not remove scans or override
dependency metadata to make an update appear successful.

The refreshed locks supersede the versions proposed in PRs 27, 28, 35, and
46. GitHub alert closure and PR disposition require a verified push; resolving
a local lock is not remote remediation evidence. Grouped version-update PRs
keep related actions and Python requirements reviewable as coherent sets.
